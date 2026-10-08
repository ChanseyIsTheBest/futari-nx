# Builds the Nintendo Switch NRO: out\switch\futari.nro (devkitA64 GCC through tools\switch\cmake\switch-devkitA64.cmake,
# libnx, the static NVK driver of the Mesa Horizon SDK). The generated game code must exist (tools\codegen.ps1 on the
# PC: a cross build cannot run the code generator).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\build_switch.ps1 [-Jobs 3] [-Clean]
#       [-Pgo none|generate|use] [-BuildDir out\switch]
#
# -Pgo (profile-guided optimisation, sa2-nx's design): generate = the instrumented NRO that collects counts on the
# console (app/src/futari_pgo.cpp; build it in its own folder, e.g. -BuildDir out\switch_pgo); use = the optimised NRO
# from the counts tools\win\pgo_merge.ps1 merged into pgo\; none = neither. Without -Pgo the build keeps its setting.
#
# Overrides: $env:FUTARI_NVK_SDK (folder with lib\libvulkan.a), $env:DEVKITPRO (default C:\devkitPro).
param(
    [int]$Jobs = 3,
    [switch]$Clean,
    [ValidateSet('', 'none', 'generate', 'use')][string]$Pgo = '',
    [string]$BuildDir = 'out\switch'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$vs = if ($env:FUTARI_VSDIR) { $env:FUTARI_VSDIR } else { 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' }
$cm = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
# One CMake + Ninja (Windows paths): never MSYS2's.
$env:PATH = (($env:PATH -split ';') | Where-Object { $_ -and $_ -notmatch 'msys2\\usr\\bin' }) -join ';'
$env:PATH = "$cm\CMake\bin;$cm\Ninja;C:\Python314;$env:PATH"
if (-not $env:DEVKITPRO) { $env:DEVKITPRO = 'C:/devkitPro' }
$nvk = if ($env:FUTARI_NVK_SDK) { $env:FUTARI_NVK_SDK } else {
    'C:/devkitPro/mesa-switch-main-nfsmw-source/mesa-switch-main/mesa-unified-install/opt/devkitpro/portlibs/switch' }
if (-not (Test-Path (Join-Path $nvk 'lib\libvulkan.a'))) { throw "no lib\libvulkan.a under $nvk (set FUTARI_NVK_SDK)" }
$build = Join-Path $root $BuildDir
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force (Join-Path $root 'out') | Out-Null
$glslang = (Join-Path $root 'out\glslang\StandAlone\glslangValidator.exe').Replace('\', '/')
$log = Join-Path $root ('out\' + (Split-Path $BuildDir -Leaf) + '_build.log')
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    $configure = @('-S', "$root\app", '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_TOOLCHAIN_FILE=$root\tools\switch\cmake\switch-devkitA64.cmake",
        "-DREXSDK_DIR=$root\sdk", "-DREXGLUE_SWITCH_NVK_SDK=$nvk", "-DFUTARI_GLSLANG_VALIDATOR=$glslang",
        '-DPYTHON_EXECUTABLE=C:/Python314/python.exe', '-DPython3_EXECUTABLE=C:/Python314/python.exe')
    # Through cmd like the steps below: with $ErrorActionPreference = 'Stop', a CMake warning on stderr stopped the
    # first configure of a new build folder (out\switch_pgo, 2026-10-08).
    $quoted = ($configure | ForEach-Object { "`"$_`"" }) -join ' '
    & $env:ComSpec /c "cmake $quoted > `"$(Join-Path $root 'out\switch_configure.log')`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $root 'out\switch_configure.log') -Tail 30; throw 'configure failed' }
}
if ($Pgo) {
    # Through cmd, as the build below: CMake's warnings on stderr must not stop this script ($ErrorActionPreference).
    $pgoValue = if ($Pgo -eq 'none') { '' } else { $Pgo }
    & $env:ComSpec /c "cmake `"$build`" -DFUTARI_PGO=$pgoValue > `"$(Join-Path $root 'out\switch_configure.log')`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $root 'out\switch_configure.log') -Tail 30; throw 'configure failed' }
}
& $env:ComSpec /c "cmake --build `"$build`" -j $Jobs > `"$log`" 2>&1"
$code = $LASTEXITCODE
Get-Content $log -Tail 25
if ($code -ne 0) { throw "build failed (exit $code), see out\switch_build.log" }
Get-ChildItem $build -Filter *.nro | Select-Object FullName, Length
