#!/usr/bin/env bash
# Builds dist/bbport-windows/ (and dist/bbport-windows.zip): Bloodborne.exe (the launcher and the
# game preparation, a static C++ program), bb-probe.exe with the MSYS2 CLANG64 DLLs it needs,
# the launcher's translations and the game patches. Run from an MSYS2 CLANG64 shell after
# `bash build.sh`. FSR 4 assets in fsr4_shaders/ are included when present.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
for exe in bb-probe.exe bb-gpu-capabilities.exe bb-play.exe Bloodborne.exe; do
    [[ -f out/$exe ]] || { echo "Build first: bash build.sh (out/$exe is missing)" >&2; exit 1; }
done

# The package is assembled in a fresh staging folder and zipped from there; dist/bbport-windows
# (a playable copy that may hold saves and settings) is only refreshed afterwards.
dest=out/stage/bbport-windows
rm -rf -- out/stage
mkdir -p "$dest/bin" "$dest/launcher"
llvm-strip -o "$dest/Bloodborne.exe" out/Bloodborne.exe
llvm-strip -o "$dest/Play Bloodborne.exe" out/bb-play.exe
cp -r launcher/lang "$dest/launcher/"
cp launcher/bloodborne.ico launcher/bloodborne.png "$dest/launcher/"
# The executables without debug information (out/ keeps the symbols for crash reports).
for exe in bb-probe.exe bb-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# Every DLL the game executables load from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/bb-probe.exe" "$dest/bin/bb-gpu-capabilities.exe" |
    awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/bin/"
    done
cp -r patches "$dest/"
cp LICENSE README.md README.ru.md packaging/windows/README-Windows.txt "$dest/"
# FSR 4 assets (tools/fetch_fsr4_assets.sh, or copied to the repository root by hand).
if [[ -d fsr4_shaders ]]; then
    cp -r fsr4_shaders "$dest/"
else
    echo "fsr4_shaders/ not found: the package has no FSR 4 (players can download it in the launcher)" >&2
fi
# DLSS (NVIDIA RTX): the MSVC-built bridge and NVIDIA's runtime, next to bb-probe.exe
# (packaging/windows/build_dlss.sh). Without them the DLSS option stays unavailable.
mkdir -p "$dest/licenses"
if [[ -f out/bbport_dlss.dll && -f out/nvngx_dlss.dll ]]; then
    cp out/bbport_dlss.dll out/nvngx_dlss.dll "$dest/bin/"
    cp out/NVIDIA-DLSS-LICENSE.txt "$dest/licenses/"
    cp gpu/dlss_bridge/LICENSE.txt "$dest/licenses/bbport_dlss-LICENSE.txt"
else
    echo "DLSS bridge not built (packaging/windows/build_dlss.sh): no DLSS in this package" >&2
fi
# Licenses of what is compiled in: online co-op (wepoll, cpp-httplib), the launcher (GLFW,
# pugixml, libdeflate, miniz, JSON for Modern C++) and the MSYS2 DLLs the game loads.
cp gpu/third_party/wepoll/LICENSE "$dest/licenses/wepoll-LICENSE.txt"
cp gpu/third_party/httplib/LICENSE "$dest/licenses/cpp-httplib-LICENSE.txt"
cp gpu/third_party/miniz/LICENSE "$dest/licenses/miniz-LICENSE.txt"
cp packaging/windows/licenses/*.txt "$dest/licenses/"
for package in protobuf abseil-cpp openssl miniupnpc zlib glfw pugixml; do
    license=/clang64/share/licenses/$package/LICENSE
    if [[ -f $license ]]; then cp "$license" "$dest/licenses/$package-LICENSE.txt"; fi
done
cp /clang64/share/licenses/libdeflate/COPYING "$dest/licenses/libdeflate-LICENSE.txt"
mkdir -p dist
rm -f dist/bbport-windows.zip
(cd out/stage && powershell -NoProfile -Command \
    "Compress-Archive -Path bbport-windows -DestinationPath ../../dist/bbport-windows.zip")

# Refresh dist/bbport-windows, keeping what players create there (saves, settings, mods), and
# only while nothing runs from it: deleting a running launcher's files breaks it.
play=dist/bbport-windows
running=$(powershell -NoProfile -Command \
    "@(Get-Process | Where-Object { \$_.Path -like '$(cygpath -w "$PWD/$play")\\*' }).Count" | tr -d '\r')
if [[ ${running:-0} != 0 ]]; then
    echo "dist/bbport-windows is in use ($running processes): not refreshed; the zip is ready." >&2
else
    mkdir -p "$play"
    find "$play" -mindepth 1 -maxdepth 1 ! -name user ! -name mods ! -name bbport.ini \
        ! -name mods.json ! -name patches.json -exec rm -rf {} +
    cp -r "$dest/." "$play/"
fi
du -sh "$dest" dist/bbport-windows.zip
