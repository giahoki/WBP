#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out

# Third-party libraries (FSR-Vulkan, Dear ImGui, magic_enum, miniz, xbyak, LibAtrac9) are part of
# the repository: a plain clone builds without downloading anything.
third_party_check() {
    local missing=()
    for file in gpu/third_party/fsr-vulkan/CMakeLists.txt gpu/third_party/imgui/imgui.h \
                gpu/third_party/xbyak/xbyak/xbyak.h gpu/third_party/miniz/miniz.h \
                gpu/third_party/magic_enum/include/magic_enum/magic_enum.hpp third_party/LibAtrac9/C/src/decoder.h; do
        [[ -f $file ]] || missing+=("$file")
    done
    if (( ${#missing[@]} )); then
        echo "Missing third-party sources: ${missing[*]}" >&2
        echo "They belong to the repository: restore them with git checkout -- gpu/third_party third_party" >&2
        exit 1
    fi
}
# This port's changes to FSR-Vulkan (gpu/patches/fsr-vulkan). The copy in the repository has them
# already; they are applied (from the repository root) only to a fresh upstream copy.
apply_fsr_patches() {
    local patch
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if git apply --ignore-whitespace --directory=gpu/third_party/fsr-vulkan --reverse --check "$patch" 2>/dev/null; then
            continue  # already applied
        elif git apply --ignore-whitespace --directory=gpu/third_party/fsr-vulkan --check "$patch" 2>/dev/null; then
            git apply --ignore-whitespace --directory=gpu/third_party/fsr-vulkan "$patch"
            echo "Applied $patch"
        else
            echo "Note: $patch neither applies nor reverts cleanly; using FSR-Vulkan as it is" >&2
        fi
    done
}

# Windows: MSYS2 CLANG64 (packages in packaging/windows/README.md). The GPU library is static
# there and CMake links wbp-game.exe as well; ThinLTO on (BB_LTO=OFF: off), clang PGO from
# pgo/wbp.profdata when it is there.
case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=msys2-env.sh
    source ./msys2-env.sh
    for tool in clang cmake ninja pkg-config; do
        command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (packaging/windows/README.md)." >&2; exit 1; }
    done
    # Profile-guided optimization (clang): BB_PGO=generate builds an instrumented game that
    # writes pgo/*.profraw while it is played; the next build merges them into
    # pgo/wbp.profdata and uses it (BB_PGO=off: without). One profile serves every player.
    pgo=${BB_PGO:-}
    if [[ -z $pgo ]]; then
        if compgen -G 'pgo/*.profraw' >/dev/null; then
            llvm-profdata merge -o pgo/wbp.profdata pgo/*.profraw
            mkdir -p pgo/recorded && mv pgo/*.profraw pgo/recorded/
            echo "PGO: merged the recorded profiles into pgo/wbp.profdata"
        fi
        if [[ -f pgo/wbp.profdata ]]; then pgo=use; else pgo=off; fi
    fi
    mkdir -p pgo
    # --launcher: only Bloodborne.exe (the game runtime stays as it was built).
    if [[ ${1:-} != --launcher ]]; then
    third_party_check
    apply_fsr_patches
    cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" -DBB_PGO_DIR="$PWD/pgo" \
        -DBB_LTO="${BB_LTO:-ON}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
    echo "GPU library and loader: LTO ${BB_LTO:-ON}, PGO $pgo"
    if ! ninja -C out/gpu wbp-game bb-gpu-capabilities bb-play > out/gpu-build.log 2>&1; then
        grep -v '^\[' out/gpu-build.log | tail -40 >&2
        echo 'Build failed (full log: out/gpu-build.log)' >&2; exit 1
    fi
    cp out/gpu/wbp-game.exe out/gpu/bb-gpu-capabilities.exe out/gpu/bb-play.exe out/
    echo "Built $PWD/out/wbp-game.exe"
    fi
    # The launcher and the game preparation (Bloodborne.exe, launcher/CMakeLists.txt).
    cmake -S launcher -B out/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
    if ! ninja -C out/launcher > out/launcher-build.log 2>&1; then
        grep -v '^\[' out/launcher-build.log | tail -40 >&2
        echo 'Launcher build failed (full log: out/launcher-build.log)' >&2; exit 1
    fi
    cp out/launcher/Bloodborne.exe out/
    echo "Built $PWD/out/Bloodborne.exe"
    exit 0
    ;;
esac
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
third_party_check
apply_fsr_patches
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" >/dev/null
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
runtime=(src/runtime*.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/wbp-game
echo "Built $PWD/out/wbp-game"
# GPU check for run.sh (live_resolution=auto): links only the Vulkan loader.
"$CC" -std=c11 -O2 -Wall -Wextra -Werror tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
if [[ ${1:-} == --test ]]; then
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" -o out/pad-test
    out/pad-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
fi
