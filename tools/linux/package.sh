#!/usr/bin/env bash
# Puts a Linux build and your game files together in one folder that runs on its own:
#
#   out/futari-nx-<preset>/
#     futari                 the executable (RPATH $ORIGIN: its libraries are found next to it)
#     *.so                   the SDK runtime, the GPU plugin and the eight game cores
#     futari.toml            your settings (app/futari_linux.toml; an existing one is kept)
#     game_root/             the files of your disc (assets/game_root)
#     dlc/                   optional: Black Label and Ver 1.01, installed by the game at its first start
#
#   tools/linux/package.sh [--preset linux-arm64|linux-amd64] [--out DIR]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
preset=""
out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset="$2"; shift ;;
        --out) out="$2"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
if [ -z "$preset" ]; then
    case "$(uname -m)" in
        aarch64|arm64) preset=linux-arm64 ;;
        *) preset=linux-amd64 ;;
    esac
fi
build="$root/out/$preset"
out="${out:-$root/out/futari-nx-$preset}"
if [ ! -x "$build/futari" ]; then
    echo "$build/futari not found: run tools/linux/build.sh first" >&2
    exit 1
fi
if [ ! -f "$root/assets/game_root/default.xex" ]; then
    echo "assets/game_root/default.xex not found" >&2
    exit 1
fi

mkdir -p "$out"
cp -f "$build/futari" "$out/"
find "$build" -maxdepth 1 -name '*.so' -exec cp -f {} "$out/" \;
if [ ! -f "$out/futari.toml" ]; then
    cp "$root/app/futari_linux.toml" "$out/futari.toml"
fi
mkdir -p "$out/game_root"
cp -a "$root/assets/game_root/." "$out/game_root/"
# DLC packages (tools/linux/build.sh --game-dir moves a game folder's dlc/ there): installed by the game at its next
# start.
if [ -d "$root/out/dlc" ] && [ ! -e "$out/dlc" ]; then
    cp -a "$root/out/dlc" "$out/dlc"
fi
echo "done: $out"
