# Starts the PC build for a few seconds, stops it, and prints the end of its newest log plus what it reached.
# With -VideoAt N it also records a video from N seconds on (futari_video_s) and makes the MP4 (tools/make_video.sh).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 [-Seconds 45] [-Exe out\pc\futari.exe]
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -VideoAt 10 -VideoFrames 600
param(
    [int]$Seconds = 45,
    [string]$Exe = 'out\pc\futari.exe',
    [int]$Tail = 40,
    [int]$VideoAt = 0,
    [int]$VideoFrames = 300,
    # Scripted pad (futari_input_script): e.g. '10:start,13:a,15:a'. See app/src/futari_input_script.h.
    [string]$Pad = ''
)
if ($Pad) { $env:REX_FUTARI_INPUT_SCRIPT = $Pad } else { Remove-Item env:REX_FUTARI_INPUT_SCRIPT -ErrorAction SilentlyContinue }
if ($VideoAt -gt 0) {
    $env:REX_FUTARI_VIDEO_S = "$VideoAt"
    $env:REX_FUTARI_VIDEO_FRAMES = "$VideoFrames"
    # Room for the recording (60 fps) and for writing it out before the run is stopped.
    $Seconds = [math]::Max($Seconds, $VideoAt + [int][math]::Ceiling($VideoFrames / 60.0) + 30)
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$exePath = (Resolve-Path (Join-Path $root $Exe)).Path
$dir = Split-Path $exePath
# A test instance left over from an interrupted run keeps the exe locked (the next link fails with "permission
# denied") and competes for the GPU: stop instances of this very executable only (other trees are left alone).
Get-CimInstance Win32_Process -Filter "Name='$(Split-Path $exePath -Leaf)'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -eq $exePath } | ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $_.ProcessId -Timeout 20 -ErrorAction SilentlyContinue
    }
$started = Get-Date
# Test runs are always muted and minimized (environment beats futari.toml), so they never take over the desktop.
$env:REX_AUDIO_MUTE = 'true'
$env:REX_WINDOW_START_MINIMIZED = 'true'
$p = Start-Process -FilePath $exePath -WorkingDirectory $dir -PassThru -WindowStyle Minimized
$p.WaitForExit($Seconds * 1000) | Out-Null
if ($p.HasExited) {
    '{0} exited after {1:n1} s with code 0x{2:X8}' -f (Split-Path $exePath -Leaf), ((Get-Date) - $started).TotalSeconds, $p.ExitCode
} else {
    $p.Refresh()
    $status = 'still running after {0} s: cpu {1:n1} s, working set {2} MB; stopped' -f $Seconds, $p.TotalProcessorTime.TotalSeconds, [math]::Round($p.WorkingSet64 / 1MB)
    # Stop it BEFORE printing anything: a caller that pipes this script into Select-Object -First N closes the pipe
    # after its first line, which ends this script there and would leave the game running.
    Stop-Process -Id $p.Id -Force
    # Wait until it is really gone: a process still tearing down keeps futari.exe locked and the next link fails.
    if (-not $p.WaitForExit(20000)) { $status += " (warning: process $($p.Id) did not exit within 20 s)" }
    $status
}
$log = Get-ChildItem (Join-Path $dir 'logs') -Filter *.log -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-2) } | Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) {
    "== $($log.FullName) ($($log.Length) bytes)"
    Get-Content $log.FullName -Tail $Tail
} else {
    'no log written'
}
if ($VideoAt -gt 0) {
    Remove-Item env:REX_FUTARI_VIDEO_S, env:REX_FUTARI_VIDEO_FRAMES -ErrorAction SilentlyContinue
    $raw = Join-Path $dir "captures\video_${VideoAt}s.rgba"
    if ((Test-Path $raw) -and (Get-Item $raw).LastWriteTime -ge $started) {
        $bash = 'C:\devkitPro\msys2\usr\bin\bash.exe'
        if (-not (Test-Path $bash)) { $bash = 'bash' }
        & $bash (Join-Path $root 'tools/make_video.sh').Replace('\', '/') $raw.Replace('\', '/')
    } else {
        "no video written to $raw (see the log for [capture] lines)"
    }
}
