#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the isolated arm64 VrApi resource bundle; never installs or patches an APK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import urllib.request

NDK_VERSION = "27.3.13750724"
OPENXR_COMMIT = "7f9285bce1ce8b69bb75554bf788666579d0c35e"
HEADERS = {
    "openxr.h": "df412d0088098c91f28ac3f6eff45d88d17bf50e14197370c3e51ce97be1f323",
    "openxr_platform.h": "1db2c45a1747dd5c62d2a7521ddbe1952a5af36e866485979eb78eada2a40b10",
    "openxr_platform_defines.h": "59a5369c34013beeea505aa0465768acad558ac016bab3320780e2a653a07bfb",
}
NOTICE_FILES = ("OPENXR-SDK.txt", "ANDROID-NDK.txt")


def ndk_root(explicit):
    candidates = [explicit, os.environ.get("ANDROID_NDK_HOME"), os.environ.get("ANDROID_NDK_ROOT")]
    sdk_roots = [os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT")]
    if os.environ.get("LOCALAPPDATA"):
        sdk_roots.append(str(Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk"))
    sdk_roots += [str(Path.home() / "Android/Sdk"), str(Path.home() / "Library/Android/sdk")]
    candidates += [str(Path(sdk) / "ndk" / NDK_VERSION) for sdk in sdk_roots if sdk]
    for candidate in candidates:
        if candidate and (Path(candidate) / "source.properties").is_file():
            root = Path(candidate).resolve()
            revision = next((line.split("=", 1)[1].strip() for line in (root / "source.properties").read_text().splitlines() if line.startswith("Pkg.Revision")), "")
            if revision != NDK_VERSION:
                raise SystemExit(f"VrApi requires NDK {NDK_VERSION}, found {revision} at {root}")
            return root
    raise SystemExit(f"Install NDK {NDK_VERSION}; set ANDROID_NDK_HOME or pass --ndk.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="resource root, not a .so filename")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    notices = [source / "licenses" / name for name in NOTICE_FILES]
    missing_notices = [str(notice) for notice in notices if not notice.is_file()]
    if missing_notices:
        raise SystemExit(f"Native dependency notice is missing: {', '.join(missing_notices)}")
    build = source / "build"
    headers = build / "include/openxr"
    headers.mkdir(parents=True, exist_ok=True)
    for name, expected in HEADERS.items():
        target = headers / name
        if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == expected:
            continue
        url = f"https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/{OPENXR_COMMIT}/include/openxr/{name}"
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise SystemExit(f"OpenXR header checksum mismatch: {name}")
        target.write_bytes(data)
    host = {"Windows": "windows-x86_64", "Linux": "linux-x86_64", "Darwin": "darwin-x86_64"}.get(platform.system())
    if not host:
        raise SystemExit(f"Unsupported NDK host: {platform.system()}")
    toolchain = ndk_root(args.ndk) / "toolchains/llvm/prebuilt" / host
    compiler = toolchain / "bin" / ("clang++.exe" if os.name == "nt" else "clang++")
    if not compiler.is_file():
        raise SystemExit(f"NDK compiler is missing: {compiler}")
    files = [source / name for name in ("runtime.cpp", "graphics.cpp", "input.cpp")]
    flags = ["--target=aarch64-linux-android29", f"--sysroot={toolchain / 'sysroot'}", "-std=c++17", "-O2", "-g", "-fPIC", "-fvisibility=hidden", "-fvisibility-inlines-hidden", "-Wall", "-Wextra", "-Werror=return-type", "-DXR_NO_PROTOTYPES", "-DXR_USE_PLATFORM_ANDROID", "-DXR_USE_GRAPHICS_API_VULKAN", "-DXR_USE_GRAPHICS_API_OPENGL_ES", "-DXR_USE_TIMESPEC", f"-I{build / 'include'}"]
    # Compile database is useful for native diagnostics, but is build output, not source.
    database = [{"directory": str(source), "file": str(file), "arguments": [str(compiler), *flags, "-c", str(file)]} for file in files]
    (build / "compile_commands.json").write_text(json.dumps(database, indent=2))
    library = build / "libvrapi.so"
    subprocess.run([str(compiler), *flags, "-shared", "-static-libstdc++", *(str(file) for file in files), "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384", f"-Wl,--version-script={source / 'exports.map'}", "-llog", "-landroid", "-lvulkan", "-lEGL", "-lGLESv3", "-ldl", "-o", str(library)], check=True)
    resource_root = args.output.resolve()
    destination = resource_root / "vrapi/arm64-v8a/libvrapi.bin"
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(library, destination)
    notice_directory = resource_root / "vrapi/licenses"
    notice_directory.mkdir(parents=True, exist_ok=True)
    for notice in notices:
        shutil.copyfile(notice, notice_directory / notice.name)
    print(f"VrApi payload: {destination}")
    print(f"Dependency notices: {notice_directory}")
    print(f"SHA-256: {hashlib.sha256(destination.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
