#!/usr/bin/env bash
# futari-nx (2026-10-19): the game shaders of Renut's renderer for the Switch (deko3d), after a PC build of the tree
# (FUTARI_RENUT_RENDERER on: out/pc/xenos_recomp holds Renut's XenosRecomp, out/pc/renut_gpu its early alpha HLSL and
# shader_cache.cpp). banjo-nx's chain (docs/deko3d-port-faq.md "Shaders" there):
#   1. XenosRecomp again over out/renut_shader_dump with the early alpha copies (BANJO_HLSL_OVERRIDE) and
#      BANJO_DEKO3D_SPIRV_DIR: one <vs|ps>_<hash>.spv per shader (-DBANJO_DEKO3D, D3D clip space);
#   2. tools/deko3d_shaders.py --no-uam: SPIRV-Cross to GLSL 4.60 for uam (tools/deko3d_glsl.py's bindings);
#   3. tools/deko3d_glsl_opt.py --check: banjo-nx's text passes, each output compiled with the PC uam;
#   4. tools/deko3d_shader_package.py: out/renut_switch/deko3d_glsl.bin + deko3d_glsl_switch.cpp (.incbin);
#   5. tools/renut_switch_shader_cache.py: out/renut_switch/shader_cache_switch.cpp + spirv_cache.zst.
# Game data: out/ only.
#   bash tools/renut_deko3d_shaders.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON:-python}"
RECOMP="$ROOT/out/pc/xenos_recomp/build/XenosRecomp/XenosRecomp.exe"
DUMP="$ROOT/out/renut_shader_dump"
COMMON="$ROOT/app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h"
EARLY="$ROOT/out/pc/renut_gpu/early_alpha_hlsl"
OUT="$ROOT/out/deko3d_shaders"
for f in "$RECOMP" "$COMMON" "$DUMP/manifest.csv"; do
    [ -e "$f" ] || { echo "renut_deko3d_shaders: no $f (build the PC tree with FUTARI_RENUT_RENDERER first)"; exit 1; }
done
rm -rf "$OUT"
mkdir -p "$OUT/spirv"
if [ -d "$EARLY" ]; then
    BANJO_HLSL_OVERRIDE="$EARLY" BANJO_DEKO3D_SPIRV_DIR="$OUT/spirv" "$RECOMP" "$DUMP" "$OUT/recomp_deko3d.cpp" "$COMMON"
else
    echo "renut_deko3d_shaders: no early alpha HLSL ($EARLY): the shaders without their early alpha copies"
    BANJO_DEKO3D_SPIRV_DIR="$OUT/spirv" "$RECOMP" "$DUMP" "$OUT/recomp_deko3d.cpp" "$COMMON"
fi
rm -f "$OUT/recomp_deko3d.cpp"
echo "SPIR-V: $(ls "$OUT/spirv"/*.spv 2>/dev/null | wc -l) shaders, $(ls "$OUT/spirv"/*.fail 2>/dev/null | wc -l) failed"
"$PY" "$ROOT/tools/deko3d_shaders.py" --spirv "$OUT/spirv" --out "$OUT" --no-uam --jobs 3
"$PY" "$ROOT/tools/deko3d_glsl_opt.py" --in "$OUT/glsl" --out "$OUT/glsl_opt" --check --jobs 3
"$PY" "$ROOT/tools/deko3d_shader_package.py" --glsl "$OUT/glsl_opt" --out "$ROOT/out/renut_switch"
"$PY" "$ROOT/tools/renut_switch_shader_cache.py" --pc "$ROOT/out/pc"
# The .incbin package is not dependency-tracked: its source is touched so the next Switch build embeds the new one.
touch "$ROOT/out/renut_switch/deko3d_glsl_switch.cpp" "$ROOT/out/renut_switch/shader_cache_switch.cpp"
