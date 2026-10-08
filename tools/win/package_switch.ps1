# Assembles the SD card folder of the Switch build: out\switch_sd\switch\futari-nx\ with futari.nro, the shader
# package (futari_shaders.mesp + .idx), futari.toml and, with -WithGame, your extracted disc (game_root\) and DLC (dlc\).
# Copy the contents of out\switch_sd to the root of the SD card.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\package_switch.ps1 [-WithGame] [-Dlc D:\mushihemesama\dlc]
param(
    [switch]$WithGame,
    [string]$Dlc = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$nro = Join-Path $root 'out\switch\futari.nro'
if (-not (Test-Path $nro)) { throw "$nro not found: build it first (tools\win\build_switch.ps1)" }
$shaders = Join-Path $root 'out\shaders\futari_shaders.mesp'
if (-not (Test-Path $shaders)) { throw "$shaders not found: build the shader package first (README, step 4)" }
$dest = Join-Path $root 'out\switch_sd\switch\futari-nx'
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item -Force $nro, $shaders, "$shaders.idx" $dest
if (-not (Test-Path (Join-Path $dest 'futari.toml'))) {
    Copy-Item (Join-Path $root 'app\futari_switch.toml') (Join-Path $dest 'futari.toml')
}
Copy-Item -Force (Join-Path $root 'app\switch-test-readme.txt') (Join-Path $root 'out\switch_sd\README-test.txt')
if ($WithGame) {
    $game = Join-Path $root 'assets\game_root'
    robocopy $game (Join-Path $dest 'game_root') /E /NFL /NDL /NJH /NJS /MT:8 | Out-Null
}
if ($Dlc) {
    robocopy $Dlc (Join-Path $dest 'dlc') /E /NFL /NDL /NJH /NJS /MT:8 | Out-Null
}
Get-ChildItem $dest | Select-Object Name, Length
