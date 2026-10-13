#!/usr/bin/env bash
# Builds wbp_dlss.dll (gpu/dlss_bridge, the only code that uses the NVIDIA DLSS SDK) with
# MSVC and puts it with NVIDIA's nvngx_dlss.dll and its license next to out/wbp-game.exe;
# package.sh ships them. build.bat runs this for "all" and "pgo".
#
# The SDK: DLSS_SDK_ROOT, else third_party/DLSS-SDK (not in the repository). When that is
# missing it is fetched from github.com/NVIDIA/DLSS: a shallow, sparse clone of only the
# headers, the static library, the runtime DLL and the license.
# Needs Visual Studio 2022 or 2026 with the C++ workload; its own CMake is used unless
# CMAKE_EXE names one. Exit code 2: DLSS skipped (no Visual Studio or no SDK), the build
# goes on without DLSS.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

sdk=${DLSS_SDK_ROOT:-third_party/DLSS-SDK}
if [[ ! -f $sdk/include/nvsdk_ngx_vk.h ]]; then
    if [[ -n ${DLSS_SDK_ROOT:-} ]]; then
        echo "DLSS: DLSS_SDK_ROOT=$sdk has no include/nvsdk_ngx_vk.h; skipped" >&2
        exit 2
    fi
    echo "DLSS: fetching the SDK (github.com/NVIDIA/DLSS) into $sdk"
    rm -rf "$sdk"
    if ! git clone --quiet --depth 1 --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git "$sdk" ||
       ! git -C "$sdk" sparse-checkout set --no-cone /include/ /LICENSE.txt \
            /lib/Windows_x86_64/x64/nvsdk_ngx_s.lib /lib/Windows_x86_64/rel/nvngx_dlss.dll; then
        rm -rf "$sdk"
        echo "DLSS: could not fetch the SDK; skipped" >&2
        exit 2
    fi
fi

vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
vs_path=""
vs_major=""
if [[ -x $vswhere ]]; then
    vs_path=$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
        -property installationPath 2>/dev/null | tr -d '\r')
    vs_major=$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
        -property installationVersion 2>/dev/null | tr -d '\r' | cut -d. -f1)
fi
if [[ -z $vs_path ]]; then
    echo "DLSS: Visual Studio with the C++ workload not found; skipped" >&2
    exit 2
fi
cmake=${CMAKE_EXE:-$(cygpath -u "$vs_path")/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe}
[[ -x $cmake ]] || cmake=cmake
generator=${DLSS_VS_GENERATOR:-}
if [[ -z $generator ]]; then
    case $vs_major in
        18) generator="Visual Studio 18 2026" ;;
        *) generator="Visual Studio 17 2022" ;;
    esac
fi

# Only the Vulkan headers: MSYS2's C headers must not reach MSVC.
rm -rf out/vkinclude && mkdir -p out/vkinclude
cp -r /c/msys64/clang64/include/vulkan /c/msys64/clang64/include/vk_video out/vkinclude/
"$cmake" -S gpu/dlss_bridge -B out/dlss-bridge -G "$generator" -A x64 \
    -DDLSS_SDK_ROOT="$(cygpath -m "$sdk")" -DVULKAN_INCLUDE="$(cygpath -m "$PWD/out/vkinclude")" >/dev/null
"$cmake" --build out/dlss-bridge --config Release >/dev/null
cp out/dlss-bridge/Release/wbp_dlss.dll "$sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll" out/
cp "$sdk/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
echo "DLSS: out/wbp_dlss.dll and out/nvngx_dlss.dll ready"
