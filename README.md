<p align="center"><img src="assets/overport.svg" alt="overport logo"></p>

# OVRPort CLI

OVRPort is a command-line tool for patching legally obtained Android
applications that target Quest headsets so they can use supported OpenXR
headsets.

This repository is a downstream fork of
[`ovrport/app`](https://github.com/ovrport/app). It preserves the upstream
patcher and its supported-platform tiers while adding CLI-only release
packaging and an **opt-in, experimental** direct VrApi-to-OpenXR compatibility
path. Builds from this fork are named from the repository that produced them,
so they are not presented as official upstream releases.

## Build channels

| Channel | Tag | Contents |
| --- | --- | --- |
| Stable | `v<major>.<minor>.<patch>` | The normal OVRPort CLI JAR. The experimental VrApi adapter is not built or packaged. |
| Experimental | `v<major>.<minor>.<patch>-<prerelease>` (for example, `v1.2.3-vrapi.1`) | The CLI JAR with the project-built VrApi compatibility resource enabled. |

The GitHub Actions release workflow is configured but has not yet been run on
GitHub. When run manually it can select either channel; a matching tag selects
stable or experimental automatically. Tagged runs are configured to create a
**draft** GitHub release for maintainer inspection before manual
publication. Release names and artifacts include this repository's name,
version and channel.

Neither build channel enables VrApi translation or replaces a game's
`libvrapi.so` by default. The experimental adapter is present only when Gradle
is invoked with `-PwithVrApi=true`; selecting its replacement patch remains
explicit.

## Automatic platform message compatibility

Both channels include a separate ARM64 `libovrplatformcompat.so` resource.
After the selected patches run, the patcher checks the APK's
`libovrplatformloader.so` dynamic exports. If `ovrMessageType_ToString` is
missing, it adds the companion as a native dependency and packages it alongside
the loader. A loader that already defines the public symbol is left unchanged.
No additional CLI patch name is required.

The companion provides only the SDK's allocation-free message-type-to-string
mapping, including legacy values and the `UNKNOWN` fallback. It does not
change platform initialization, authentication, or entitlement results, and is
independent of the experimental VrApi adapter.

## Meta XR Audio telemetry on x86_64 emulators

`patch_disable_meta_xr_audio_telemetry` is optional and not recommended by
default. Meta XR Audio's Unity plugin creates a telemetry dispatcher that
loads `libandroid.so` and calls `JNI_GetCreatedJavaVMs` through it. Android's
ARM64 translation layer on x86_64 emulators has no trampoline for that
function, so the game aborts with `Bad 'JNI_GetCreatedJavaVMs' call` as its
audio starts. The patch makes that library load return nothing, which the
dispatcher already handles by skipping telemetry; audio is unaffected. It
matches `libMetaXRAudioUnity.so` as shipped in North Star 1.0.1 and leaves any
other build unchanged.

## Custom OpenXR dispatcher

`patch_replace_openxr_dispatcher=<path>` is optional and not recommended by
default. It replaces the `libopenxr_loader.so` dispatcher that
`patch_copy_libraries` packages from the downloaded library bundle with one
you supply, such as an open-source or locally built dispatcher. `<path>` is
either a directory containing `<abi>/libopenxr_loader.so` for every ABI the APK
ships, or a single `.so` file for an APK whose only ABI is `arm64-v8a`. Vendor
loaders and `libopenxr_loader_generic.so` are left unchanged. The patch fails
without replacing anything when the path or a required ABI file is missing.
Add it with `--extra-patches` so it runs after `patch_copy_libraries`; the path
cannot contain `,` or `;`.

```bash
java -jar <cli.jar> patch \
  --input=/path/to/application.apk \
  --extra-patches=patch_replace_openxr_dispatcher=/path/to/prebuilt
```

## Experimental direct VrApi compatibility

Experimental builds add the non-recommended `Replace VrApi with experimental
OpenXR compatibility layer` patch. It replaces an existing arm64-v8a
`libvrapi.so` in a selected APK with this project's OpenXR-backed compatibility
library. The patch is CLI opt-in. It fails rather than partially modifying an
APK when its required arm64 resource is absent or a participating ABI is
unsupported, and it cannot be combined with the patch that removes VrApi.

The initial engineering target is the arm64 Vulkan build of **The Climb 2**
running through AXRB. This experimental path does not establish complete
gameplay compatibility.

The adapter currently supports only `arm64-v8a`, Vulkan, primary-stereo
projection layers (including head-locked projections) and a limited set of other
projection flags and blend modes. It does not support GLES, 32-bit ABIs or
non-projection VrApi layers. Projection matrices use the conventional GL-depth
form expected to be converted by the target's CryEngine Vulkan renderer;
this is not a general Vulkan-depth or cross-engine projection implementation.

Display-refresh queries use the runtime's reported display rate rather than
assuming a Quest-specific rate. With AXRB's optional
`XR_AXRB_system_display_refresh_rate` extension, the value is available before
Vulkan registration, session creation, or the first frame. Other runtimes use
`XR_FB_display_refresh_rate` once a session exists; frame-period estimation is
used only when neither direct-rate extension is available. An unavailable
direct rate returns zero, not a fabricated fallback. These queries do not
request a headset refresh-mode change or override a game's own frame limiter.

For local AXRB startup diagnosis, adding `r_variable_rate_shading = 0` to
`/storage/emulated/0/Android/data/com.crytek.climb2/files/user.cfg` gets past the
initial device-extension check. Preserve any existing settings in that file.
This setting does not resolve other compatibility limitations. The patch does
not modify game configuration automatically or advertise unsupported Vulkan
extensions.

The disabled-foveation path supplies a real, immutable `1x1` `RG8_UNORM`
texture, initialized to full density on every array layer and shared across
the swapchain's buffer indices. Initialization must complete before the image
is returned. Hardware foveation is still reported as unsupported; this ordinary
sampled texture is **not** a fragment-density attachment or an implementation
of Vulkan shading-rate extensions. No foveation backend is required for this
full-rate resource compatibility path.

Initial eye-level tracking waits for a valid tracked head pose in a visible
session, establishes a yaw/position origin, and reports that change through the
recenter counter. This counter survives VR-session and Vulkan-system teardown
but resets on full `vrapi_Shutdown()`. Heading extraction accounts for headset
pitch. Legacy Touch
controller poses use the inverse of Meta's native-to-grip rigid transform rather
than returning raw OpenXR grip poses. The adapter reports the concrete Quest
compatibility identity (`259`), not a hardware-detected headset model.

Fixed-to-view projections use OpenXR `VIEW` space and the runtime's head-relative
per-eye poses, rather than timewarping the images from the application's render
`HeadPose`. Ordinary projections retain their application-space behavior.
This is generic VrApi compatibility, with no package-name checks. AXRB also
needs matching updated guest/host components that preserve projection reference
spaces through transport.

Eye-FOV system properties use valid runtime recommendations from
`XR_EPIC_view_configuration_fov` when available. Once views are located, their
live optical FOV supersedes any provisional startup recommendation; rendering
continues to use the located per-eye projections.

Projection conversion accepts either sign of the vertical texture scale and
preserves the image-edge tangent directions in core OpenXR `XrFovf`. For
Vulkan's top-left origin, a negative scale gives ordinary vertical FOV ordering;
a positive scale gives a reversed vertical FOV. No optional image-layout
extension is required. The AXRB guest normalizes each eye's FOV and applies the
corresponding crop-relative flip, including cancellation with an explicit
image-layout flip.


The current Vulkan interop path assumes the application's synchronization
queue belongs to the first graphics-capable queue family and uses queue index
0 for the OpenXR graphics binding. It submits its copies on the queue provided
by VrApi rather than rejecting distinct emulator handles for the same queue.
Other queue-family arrangements remain unsupported and unverified.

The compatibility library is built from source in this repository and uses the
OpenXR loader already handled by OVRPort's existing library packaging. This
repository does not redistribute Meta's proprietary VrApi library or SDK.
Users must supply applications they are legally entitled to use.


## Downloads

The configured workflow produces one versioned runnable CLI shadow JAR for the
selected stable or experimental channel.

After the configured workflow has run, use this repository's GitHub
**Releases** page for tagged draft/released builds or the **Actions** run for
branch and pull-request artifacts. Check the channel in both the artifact
filename and release title before downloading.

## Building from source

### Requirements

- JDK 17 or newer. The CLI JAR it builds runs on Java 8 or newer, whichever
  JDK builds it.
- Android SDK command-line tools for installing the pinned NDK
- Gradle through the included wrapper
- Python 3 and Android NDK `27.3.13750724` for the automatic native companion
  (and, when enabled, the experimental VrApi adapter)

Set `ANDROID_NDK_HOME` to that NDK, or install it under an Android SDK exposed
through `ANDROID_HOME`/`ANDROID_SDK_ROOT`. The native builder can locate either
layout.

`gradle.properties` is the source of the numeric application version.
Prerelease text belongs in the Git tag and artifact metadata, not in
`appVersion`.

### Stable/default build

The default profile does not build or package the experimental adapter:

```bash
./gradlew :overportcli:shadowJar
```

On Windows, use `gradlew.bat` instead of `./gradlew`.

The platform companion and its SDK/NDK notices are built automatically for
every profile. To supply a prebuilt resource root instead:

```bash
python native/platform/build_platform.py \
  --ndk "$ANDROID_NDK_HOME" --output "$PWD/build/platform-resource"
./gradlew :overportcli:shadowJar \
  -PplatformResourceRoot="$PWD/build/platform-resource"
```

The root must contain `platform/arm64-v8a/libovrplatformcompat.bin`,
`platform/licenses/OCULUS-PLATFORM-SDK.txt`, and
`platform/licenses/ANDROID-NDK.txt`. Missing resources fail the build.

### Experimental opt-in build

With NDK `27.3.13750724` installed, the exact opt-in profile is:

```bash
./gradlew :overportcli:shadowJar -PwithVrApi=true
```

That profile builds the native resource automatically. To build it explicitly
or reuse one payload across packaging jobs:

```bash
python native/vrapi/build.py \
  --ndk "$ANDROID_NDK_HOME" \
  --output "$PWD/build/vrapi-resource"

./gradlew :overportcli:shadowJar \
  -PwithVrApi=true \
  -PvrApiResourceRoot="$PWD/build/vrapi-resource"
```

The generated resource root contains
`vrapi/arm64-v8a/libvrapi.bin` and the dependency notices under
`vrapi/licenses/`. The payload is an Android arm64 shared library stored with a
`.bin` resource suffix; it is not an upstream or proprietary VrApi binary.
Omitting `withVrApi` (or setting it to `false`) keeps both the payload and its
native notices out of the normal distribution.

### Experimental CLI opt-in

The experimental patch is not part of the recommended patch set. Use an
experimental CLI JAR and name it explicitly:

```bash
java -jar <experimental-cli.jar> patches
java -jar <experimental-cli.jar> patch \
  --input=/path/to/application.apk \
  --extra-patches=patch_vrapi_openxr
```

`--patches` replaces the default recommended selection. `--extra-patches`
adds patches after either the defaults or an explicit `--patches` selection.
Both options use a quoted, semicolon-separated value, for example
`--extra-patches='patch_vrapi_openxr;another_patch'`. Keep the original APK
and review the output before installing it.

The former Compose desktop and Android applications have been removed. AXRB
is the maintained GUI and invokes this CLI as a separately installed process;
it does not redistribute the CLI or Java. For integrations,
`java -jar <cli.jar> patches --json` prints a JSON array of `{name, recommended}`
entries from the patch engine. Unknown patches, duplicate selections and the
conflicting VrApi replacement/removal combination are rejected before patching.

## Release process

Tags must use `v<major>.<minor>.<patch>[-prerelease]`. A suffix automatically
selects the experimental channel; a tag without a suffix always selects stable
and therefore cannot package the experimental native resource. The workflow
has not yet been run on GitHub. When run, it executes the CLI test suite before
building, requires the expected JAR to exist, and uploads only that JAR to the
draft release.

Before publishing a draft, a maintainer must:

1. verify that the tag's numeric version matches `appVersion` in
   `gradle.properties`;
2. inspect and test the CLI JAR and, for an experimental release, test patched
   output on real target hardware; and
3. review the generated notes and explicitly publish the draft in GitHub.

No signing keys or publishing credentials are committed to the repository.

## Upstream supported platforms

These tiers describe upstream OVRPort patching support, not validation of the
experimental direct VrApi adapter.

### Tier 1

- Pico 4 Ultra

### Tier 2

- Pico 4 / 4 Pro
- Pico Neo 3
- Oculus Quest 2

### Tier 3

- Oculus Quest (1)
- Meta Quest 3 / 3S
- YVR/PFDMR headsets

Other OpenXR-compatible headsets may work but require additional testing.

## Reporting issues

Attach `adb logcat` output when reporting a problem. For game compatibility
issues, first enable verbose OVRPort logging:

```text
adb shell setprop debug.sys.ovrport.verboselogging 1
```

State the build channel, artifact version, source and target ABI, headset,
OpenXR runtime and the selected patches. Do not report an experimental result
as stable behavior.

## Legal, licensing and attribution

Think of this tool like an emulator or compatibility utility: you must legally
obtain the application files you process. Do not use it to distribute
copyrighted games, proprietary SDKs or vendor libraries.

OVRPort CLI is licensed under the GNU General Public License v3; see
[`LICENSE`](LICENSE). The experimental native adapter's pinned OpenXR-SDK
headers are offered under `Apache-2.0 OR MIT`; this distribution uses the MIT
option. Statically linked Android NDK runtime portions are covered by the
Apache License 2.0 with LLVM exception and the applicable legacy MIT,
University of Illinois/NCSA and BSD notices. Exact attributions and license
texts are in [`native/vrapi/licenses`](native/vrapi/licenses/) and are copied
into every generated experimental resource root.

The platform message mapping uses current and legacy Meta/Oculus SDK identifiers.
Their notices are retained in
[`native/platform/licenses`](native/platform/licenses/). Every generated platform
resource root includes those notices and the shared Android NDK attribution
under `platform/licenses/`; no proprietary platform binary is bundled.

This project is neither affiliated with nor endorsed by Meta.
