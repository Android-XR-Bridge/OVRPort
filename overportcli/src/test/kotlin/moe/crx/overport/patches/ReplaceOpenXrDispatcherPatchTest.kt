package moe.crx.overport.patches

import com.reandroid.apk.ApkModule
import moe.crx.overport.commands.PatchApplicationCommand
import moe.crx.overport.patching.PatchExecutor
import moe.crx.overport.patching.PatchStore
import org.junit.jupiter.api.io.TempDir
import java.io.File
import kotlin.test.Test
import kotlin.test.assertContains
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse

class ReplaceOpenXrDispatcherPatchTest {
    @TempDir
    lateinit var directory: File

    private fun bundled(abi: String) = "bundled-dispatcher-$abi".encodeToByteArray()
    private fun supplied(abi: String) = "supplied-dispatcher-$abi".encodeToByteArray()

    private fun writeFile(path: String, bytes: ByteArray): File {
        return directory.resolve(path).apply {
            parentFile.mkdirs()
            writeBytes(bytes)
        }
    }

    private fun addApkLibraries(abi: String): File {
        writeFile("root/lib/$abi/libopenxr_loader_generic.so", "generic-$abi".encodeToByteArray())
        writeFile("root/lib/$abi/libopenxr_loader_meta.so", "meta-$abi".encodeToByteArray())
        writeFile("root/lib/$abi/libgame.so", "game-$abi".encodeToByteArray())
        return writeFile("root/lib/$abi/$OPENXR_DISPATCHER", bundled(abi))
    }

    private fun addPrebuilt(vararg abis: String): File {
        abis.forEach { abi -> writeFile("prebuilt/$abi/$OPENXR_DISPATCHER", supplied(abi)) }
        return directory.resolve("prebuilt")
    }

    private fun snapshot(): Map<String, List<Byte>> {
        return directory.resolve("root").walk()
            .filter { it.isFile }
            .associate { it.relativeTo(directory).invariantSeparatorsPath to it.readBytes().toList() }
    }

    private fun applyPatch(vararg arguments: String) {
        val executor = PatchExecutor(directory.resolve("libraries"), directory, ApkModule())
        PATCH_REPLACE_OPENXR_DISPATCHER.executor(executor, arguments.toList())
    }

    @Test
    fun `patch is optional`() {
        assertFalse(PATCH_REPLACE_OPENXR_DISPATCHER in PatchStore.recommended())
    }

    @Test
    fun `directory replaces the dispatcher of every ABI and nothing else`() {
        val arm64 = addApkLibraries("arm64-v8a")
        val arm32 = addApkLibraries("armeabi-v7a")
        val config = writeFile("root/lib/arm64-v8a/liboverport.config.so", "{}".encodeToByteArray())
        val prebuilt = addPrebuilt("arm64-v8a", "armeabi-v7a", "x86_64")
        val before = snapshot()

        applyPatch(prebuilt.path)

        assertContentEquals(supplied("arm64-v8a"), arm64.readBytes())
        assertContentEquals(supplied("armeabi-v7a"), arm32.readBytes())
        assertFalse(directory.resolve("root/lib/x86_64").exists())
        assertContentEquals("{}".encodeToByteArray(), config.readBytes())
        val changed = snapshot().filter { (path, bytes) -> before[path] != bytes }.keys
        assertEquals(
            setOf("root/lib/arm64-v8a/$OPENXR_DISPATCHER", "root/lib/armeabi-v7a/$OPENXR_DISPATCHER"),
            changed,
        )
    }

    @Test
    fun `single file replaces an arm64-only dispatcher`() {
        val arm64 = addApkLibraries("arm64-v8a")
        val library = writeFile("custom/libcustom.so", supplied("arm64-v8a"))

        applyPatch(library.path)

        assertContentEquals(supplied("arm64-v8a"), arm64.readBytes())
    }

    @Test
    fun `single file fails when the APK has other ABIs`() {
        addApkLibraries("arm64-v8a")
        addApkLibraries("armeabi-v7a")
        val library = writeFile("custom/libcustom.so", supplied("arm64-v8a"))
        val before = snapshot()

        val failure = assertFailsWith<IllegalArgumentException> { applyPatch(library.path) }

        assertContains(failure.message.orEmpty(), "armeabi-v7a")
        assertEquals(before, snapshot())
    }

    @Test
    fun `directory missing an ABI fails without replacing any dispatcher`() {
        addApkLibraries("arm64-v8a")
        addApkLibraries("armeabi-v7a")
        val prebuilt = addPrebuilt("arm64-v8a")
        val before = snapshot()

        val failure = assertFailsWith<IllegalArgumentException> { applyPatch(prebuilt.path) }

        assertContains(failure.message.orEmpty(), "armeabi-v7a")
        assertEquals(before, snapshot())
    }

    @Test
    fun `missing path fails`() {
        addApkLibraries("arm64-v8a")
        val missing = directory.resolve("does-not-exist")

        val failure = assertFailsWith<IllegalArgumentException> { applyPatch(missing.path) }

        assertContains(failure.message.orEmpty(), missing.path)
    }

    @Test
    fun `patch requires exactly one path`() {
        addApkLibraries("arm64-v8a")

        for (arguments in listOf(emptyArray<String>(), arrayOf(""), arrayOf("a", "b"))) {
            val failure = assertFailsWith<IllegalArgumentException> { applyPatch(*arguments) }
            assertContains(failure.message.orEmpty(), "takes one argument")
        }
    }

    @Test
    fun `APK without a dispatcher fails`() {
        writeFile("root/lib/arm64-v8a/libgame.so", "game".encodeToByteArray())
        val prebuilt = addPrebuilt("arm64-v8a")

        val failure = assertFailsWith<IllegalArgumentException> { applyPatch(prebuilt.path) }

        assertContains(failure.message.orEmpty(), PATCH_COPY_LIBRARIES.name)
    }

    @Test
    fun `extra patch runs after copied libraries`() {
        directory.resolve("root/lib/arm64-v8a").mkdirs()
        writeFile("libraries/lib/arm64-v8a/$OPENXR_DISPATCHER", bundled("arm64-v8a"))
        writeFile("libraries/lib/arm64-v8a/libopenxr_loader_generic.so", "generic".encodeToByteArray())
        val prebuilt = addPrebuilt("arm64-v8a")
        val selection = PatchApplicationCommand.mergePatchSelections(
            patches = linkedMapOf(PATCH_COPY_LIBRARIES.name to emptyList()),
            extraPatches = PatchApplicationCommand.parsePatchList(
                "${PATCH_REPLACE_OPENXR_DISPATCHER.name}=${prebuilt.path}"
            ),
        )
        val executor = PatchExecutor(directory.resolve("libraries"), directory, ApkModule())

        PatchStore.select(selection.keys).forEach { it.executor(executor, selection.getValue(it.name)) }

        assertContentEquals(
            supplied("arm64-v8a"),
            directory.resolve("root/lib/arm64-v8a/$OPENXR_DISPATCHER").readBytes(),
        )
        assertContentEquals(
            "generic".encodeToByteArray(),
            directory.resolve("root/lib/arm64-v8a/libopenxr_loader_generic.so").readBytes(),
        )
    }

    @Test
    fun `dispatcher replacement before copied libraries is rejected`() {
        val failure = assertFailsWith<IllegalArgumentException> {
            PatchStore.select(listOf(PATCH_REPLACE_OPENXR_DISPATCHER.name, PATCH_COPY_LIBRARIES.name))
        }

        assertContains(failure.message.orEmpty(), "must come after ${PATCH_COPY_LIBRARIES.name}")
    }
}
