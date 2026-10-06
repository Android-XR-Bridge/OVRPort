package moe.crx.overport.patches

import moe.crx.overport.patching.Patch
import java.io.File

internal const val OPENXR_DISPATCHER = "libopenxr_loader.so"
private const val SINGLE_FILE_ABI = "arm64-v8a"
private const val PATCH_NAME = "patch_replace_openxr_dispatcher"

internal fun replaceOpenXrDispatcher(workspace: File, arguments: List<String>): Map<File, File> {
    require(arguments.size == 1 && arguments.single().isNotEmpty()) {
        "$PATCH_NAME takes one argument: a directory containing <abi>/$OPENXR_DISPATCHER " +
            "or a single $SINGLE_FILE_ABI $OPENXR_DISPATCHER file."
    }
    val source = File(arguments.single())
    require(source.exists()) { "Cannot apply $PATCH_NAME: ${source.path} does not exist." }

    val targets = workspace.resolve("root/lib").listFiles()
        ?.filter { it.isDirectory }
        ?.mapNotNull { abiDirectory -> abiDirectory.resolve(OPENXR_DISPATCHER).takeIf { it.isFile } }
        ?.sortedBy { it.parentFile.name }
        .orEmpty()
    val replacements = targets.associateWith { target ->
        val abi = target.parentFile.name
        when {
            source.isDirectory -> source.resolve("$abi/$OPENXR_DISPATCHER")
            abi == SINGLE_FILE_ABI -> source
            else -> null
        }
    }
    val missingAbis = replacements.filterValues { it?.isFile != true }.keys.map { it.parentFile.name }

    val problems = buildList {
        if (targets.isEmpty()) {
            add("No existing lib/<abi>/$OPENXR_DISPATCHER was found; select it after ${PATCH_COPY_LIBRARIES.name}.")
        }
        if (missingAbis.isNotEmpty()) {
            if (source.isDirectory) {
                add("${source.path} has no $OPENXR_DISPATCHER for: ${missingAbis.joinToString()}.")
            } else {
                add(
                    "A single file replaces only $SINGLE_FILE_ABI; pass a directory containing " +
                        "<abi>/$OPENXR_DISPATCHER to also cover: ${missingAbis.joinToString()}."
                )
            }
        }
    }
    require(problems.isEmpty()) {
        "Cannot apply $PATCH_NAME: ${problems.joinToString(" ")}"
    }

    return replacements.mapValues { (target, replacement) ->
        replacement!!.copyTo(target, true)
        replacement
    }
}

val PATCH_REPLACE_OPENXR_DISPATCHER = Patch(PATCH_NAME, false) { arguments ->
    selectWorkspace {
        replaceOpenXrDispatcher(file, arguments).forEach { (target, replacement) ->
            println("Replaced lib/${target.parentFile.name}/$OPENXR_DISPATCHER with ${replacement.path}")
        }
    }
}
