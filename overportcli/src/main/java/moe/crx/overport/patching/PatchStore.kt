package moe.crx.overport.patching

import moe.crx.overport.patches.*

object PatchStore {
    private val PATCHES = mutableMapOf<String, Patch>()

    private fun register(patch: Patch) {
        if (PATCHES[patch.name] != null) {
            throw IllegalStateException("Patch with name ${patch.name} is already registered.")
        }

        PATCHES[patch.name] = patch
    }

    fun all(): List<Patch> {
        return PATCHES.values.toList()
    }

    fun get(name: String): Patch? {
        return PATCHES[name]
    }

    fun recommended(): List<Patch> {
        return all().filter { it.isRecommended }
    }

    fun select(names: Iterable<String>): List<Patch> {
        val requestedNames = names.toList()
        requestedNames.firstOrNull { it !in PATCHES }?.let {
            throw IllegalArgumentException("Unknown patch: $it")
        }
        val selectedNames = requestedNames.toSet()
        require(
            PATCH_VRAPI_OPENXR.name !in selectedNames ||
                PATCH_REMOVE_VRAPI.name !in selectedNames
        ) {
            "${PATCH_VRAPI_OPENXR.name} conflicts with ${PATCH_REMOVE_VRAPI.name}; select only one."
        }
        require(
            PATCH_AC_NEXUS_NO_APPSW_72.name !in selectedNames ||
                PATCH_AC_NEXUS_NO_APPSW_90.name !in selectedNames
        ) {
            "${PATCH_AC_NEXUS_NO_APPSW_72.name} conflicts with ${PATCH_AC_NEXUS_NO_APPSW_90.name}; select only one."
        }
        require(
            PATCH_REPLACE_OPENXR_DISPATCHER.name !in selectedNames ||
                PATCH_COPY_LIBRARIES.name !in selectedNames ||
                requestedNames.indexOf(PATCH_COPY_LIBRARIES.name) <
                requestedNames.indexOf(PATCH_REPLACE_OPENXR_DISPATCHER.name)
        ) {
            "${PATCH_REPLACE_OPENXR_DISPATCHER.name} must come after ${PATCH_COPY_LIBRARIES.name}, " +
                "which would otherwise overwrite the supplied dispatcher."
        }
        return requestedNames.map(PATCHES::getValue)
    }

    init {
        register(PATCH_COPY_LIBRARIES)
        register(PATCH_COPY_OVRPLUGIN_VRAPI)
        register(PATCH_REPLACE_ICON_LABEL)
        register(PATCH_FIX_MIN_ANDROID_SDK)
        register(PATCH_GENERATE_CONFIG)
        register(PATCH_REMOVE_LOCALIZED_NAMES)
        register(PATCH_CLEAN_UP_FRIDA)
        register(PATCH_OCULUS_UNITY)
        register(PATCH_OCULUS_UNREAL)
        register(PATCH_VR_METADATA)
        register(PATCH_LAUNCHER_ENTRY)
        register(PATCH_REMOVE_USES_LIBRARY)
        register(PATCH_FIX_UNREAL_CRASH)
        register(PATCH_META_XR_AUDIO)
        register(PATCH_MARK_AS_DEBUGGABLE)
        register(PATCH_MARK_ALLOW_BACKUP)
        register(PATCH_REMOVE_VRAPI)
        register(PATCH_VRAPI_OPENXR)
        register(PATCH_REMOVE_UNREAL_FORCE_QUIT)
        register(PATCH_FORCE_PASSTHROUGH)
        register(PATCH_DISABLE_SPACE_WARP)
        register(PATCH_DISABLE_CONTROLLER_OFFSET)
        register(PATCH_DISABLE_META_XR_AUDIO_TELEMETRY)
        register(PATCH_AC_NEXUS_NO_APPSW_72)
        register(PATCH_AC_NEXUS_NO_APPSW_90)
        register(PATCH_REPLACE_OPENXR_DISPATCHER)
    }
}
