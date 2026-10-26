#!/usr/bin/env bash
# Builds the Linux version from your own disc image, on the machine that will run it (AArch64; x86-64 works too).
# Every step is skipped when its output is already there, so the script can be run again after a failure or an
# update; docs/building-linux.md explains each step.
#
#   tools/linux/build.sh [--iso PATH] [--preset linux-arm64|linux-amd64] [--force-codegen] [--jobs N]
#
# --iso            your Xbox 360 disc image (needed the first time, when assets/game_root/ is empty)
# --preset         default: linux-arm64 on AArch64, linux-amd64 on x86-64
# --force-codegen  regenerate app/generated even when it is up to date
# --jobs           parallel compile jobs (default: the number of processors; each generated file needs ~1-2 GB)
#
# Result: out/<preset>/futari with its libraries and futari.toml (tools/linux/package.sh copies it into a folder
# with the game files).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
iso=""
preset=""
force=()
jobs="$(nproc)"
while [ $# -gt 0 ]; do
    case "$1" in
        --iso) iso="$2"; shift ;;
        --preset) preset="$2"; shift ;;
        --force-codegen) force=(--force) ;;
        --jobs) jobs="$2"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
if [ -z "$preset" ]; then
    case "$(uname -m)" in
        aarch64|arm64) preset=linux-arm64 ;;
        x86_64) preset=linux-amd64 ;;
        *) echo "unsupported processor $(uname -m)" >&2; exit 1 ;;
    esac
fi
cd "$root"

echo "=== tools ==="
missing=0
for tool in cmake ninja clang clang++ python3 git glslangValidator pkg-config; do
    if ! command -v "$tool" > /dev/null; then echo "missing: $tool" >&2; missing=1; fi
done
if [ "$preset" = linux-arm64 ] && ! command -v dxc > /dev/null; then
    echo "missing: dxc (DirectXShaderCompiler; docs/building-linux.md)" >&2
    missing=1
fi
python3 -c "import xxhash" 2> /dev/null || { echo "missing: the Python xxhash module" >&2; missing=1; }
[ "$missing" -eq 0 ] || exit 1

echo "=== SDK third-party sources ==="
if [ ! -f sdk/thirdparty/sdl3/CMakeLists.txt ]; then
    python3 tools/fetch_thirdparty.py
fi

echo "=== game files ==="
if [ ! -f assets/game_root/default.xex ]; then
    if [ -z "$iso" ]; then
        echo "assets/game_root/default.xex is missing: pass --iso with your disc image" >&2
        exit 1
    fi
    python3 tools/extract_iso.py "$iso" -o assets/game_root
fi

echo "=== code generator (host) ==="
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DREXGLUE_ENABLE_TRACY=OFF
cmake --build out/host --target rexglue -j "$jobs"
rexglue="$(find sdk/out -type f -name rexglue -perm -u+x -printf '%T@ %p\n' | sort -rn | head -n 1 | cut -d' ' -f2-)"

echo "=== codegen and the post-codegen steps ==="
tools/linux/codegen.sh --perf --rexglue "$root/$rexglue" "${force[@]}"

echo "=== Renut's hooks ==="
python3 tools/renut_futari_hooks.py
python3 tools/post_codegen.py
python3 tools/perf_codegen_post.py --check

echo "=== shader containers ==="
if [ ! -f out/renut_shader_dump/manifest.csv ]; then
    python3 tools/renut_dump_shaders.py
fi

echo "=== build ==="
cmake --preset "$preset" -S app
cmake --build "out/$preset" -j "$jobs"
echo "done: out/$preset/futari"
