#!/usr/bin/env bash
# Linux form of tools/codegen.ps1: translates the game (default.xex and the eight game-core DLLs) to C++ with the host
# code generator. It builds nothing: build the generator first (tools/linux/build.sh does both).
#
#   tools/linux/codegen.sh [--perf] [--force] [--rexglue PATH]
#
# Input:   assets/game_root/default.xex, assets/game_root/media/Module/*.xex   (tools/extract_iso.py puts them there)
#          app/futari_manifest.toml          (project, game paths, includes)
# Output:  app/generated/                    (C++ of the game and generated/rexglue.cmake; not committed)
#          out/codegen.log                   (log of the generator)
#          out/<module>_image.bin            (decompressed images, for the post-codegen steps and the shader tools)
#
# --perf   run tools/post_codegen.py afterwards (the build runs it again by itself after every codegen it starts)
# --force  regenerate every module even when its stamp says it is up to date (after a change of sdk/src/codegen, or
#          when app/generated came from another machine)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
app="$root/app"
perf=0
force=()
rexglue=""
while [ $# -gt 0 ]; do
    case "$1" in
        --perf) perf=1 ;;
        --force) force=(--force) ;;
        --rexglue) rexglue="$2"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ -z "$rexglue" ]; then
    # The newest generator the SDK build left in sdk/out/<platform>/.
    rexglue="$(find "$root/sdk/out" -type f -name rexglue -perm -u+x -printf '%T@ %p\n' 2>/dev/null |
        sort -rn | head -n 1 | cut -d' ' -f2-)"
fi
if [ -z "$rexglue" ] || [ ! -x "$rexglue" ]; then
    echo "rexglue not found; build it first (tools/linux/build.sh)" >&2
    exit 1
fi
xex="$root/assets/game_root/default.xex"
if [ ! -f "$xex" ]; then
    echo "$xex not found; extract your disc with tools/extract_iso.py first" >&2
    exit 1
fi

mkdir -p "$root/out"
log="$root/out/codegen.log"
echo "== codegen with $rexglue"
code=0
(
    cd "$app"
    export REX_MAX_JUMP_TABLE_ENTRIES=1024
    # Decrypted images of every binary: out/default_image.bin and out/default_image.<core>.bin, renamed below.
    export REXGLUE_DUMP_IMAGE="$root/out/default_image.bin"
    "$rexglue" "${force[@]}" codegen futari_manifest.toml > "$log" 2>&1
) || code=$?
tail -n 30 "$log"
if [ "$code" -ne 0 ]; then
    echo "codegen failed (exit $code), see out/codegen.log" >&2
    exit "$code"
fi
for image in "$root"/out/default_image.*.bin; do
    [ -e "$image" ] || continue
    core="${image##*/default_image.}"
    core="${core%.bin}"
    mv -f "$image" "$root/out/${core}_image.bin"
done

if [ "$perf" -eq 1 ]; then
    python3 "$root/tools/post_codegen.py"
fi
echo "done: $app/generated"
