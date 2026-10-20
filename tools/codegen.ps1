# Translates the game (default.xex and the eight game-core DLLs) to C++ with the host code generator. It builds
# nothing: build the generator first (tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue").
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\codegen.ps1
#
# Input:   assets\game_root\default.xex, assets\game_root\media\Module\*.xex   (tools\extract_iso.py puts them there)
#          app\futari_manifest.toml          (project, game paths, includes)
# Output:  app\generated\                    (C++ of the game and generated\rexglue.cmake; not committed)
#          out\codegen.log                   (log of the generator)
param(
    [string]$Rexglue = '',
    # Post-codegen performance passes (tools/perf_codegen_post.py: direct calls + guest memory without volatile).
    # The code generation options themselves are in app/perf_overrides*.toml, included by the manifest.
    [switch]$Perf,
    # Regenerate every module even when its stamp says it is up to date (the stamp does not cover the generator
    # binary itself: use after changing sdk/src/codegen).
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$app = Join-Path $root 'app'
if (-not $Rexglue) {
    $Rexglue = Get-ChildItem (Join-Path $root 'sdk\out') -Recurse -Filter rexglue.exe -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Rexglue -or -not (Test-Path $Rexglue)) { throw 'rexglue.exe not found; build it first (see the header)' }
$xex = Join-Path $root 'assets\game_root\default.xex'
if (-not (Test-Path $xex)) { throw "$xex not found; extract your disc with tools\extract_iso.py first" }

New-Item -ItemType Directory -Force (Join-Path $root 'out') | Out-Null
$log = Join-Path $root 'out\codegen.log'
Write-Host "== codegen with $Rexglue"
Push-Location $app
try {
    $env:REX_MAX_JUMP_TABLE_ENTRIES = '1024'
    # Decrypted images of every binary, for the analysis tools and tools\extract_shaders.py: out\default_image.bin
    # and out\default_image.<core>.bin, renamed below to out\<core>_image.bin.
    $env:REXGLUE_DUMP_IMAGE = Join-Path $root 'out\default_image.bin'
    $forceArg = if ($Force) { '--force ' } else { '' }
    & $env:ComSpec /c "`"$Rexglue`" ${forceArg}codegen futari_manifest.toml > `"$log`" 2>&1"
    $code = $LASTEXITCODE
} finally { Pop-Location }
Get-Content $log -Tail 30
if ($code -ne 0) { throw "codegen failed (exit $code), see out\codegen.log" }
Get-ChildItem (Join-Path $root 'out') -Filter 'default_image.*.bin' | ForEach-Object {
    $core = $_.Name -replace '^default_image\.(.+)\.bin$', '$1'
    Move-Item -Force $_.FullName (Join-Path $root "out\${core}_image.bin")
}

if ($Perf) {
    python (Join-Path $root 'tools\post_codegen.py')
    if ($LASTEXITCODE -ne 0) { throw 'post_codegen.py failed' }
}
Write-Host "done: $app\generated"
