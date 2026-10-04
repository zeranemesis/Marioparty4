# One headset profiling session with Meta's and Android's tools, while the
# player stays still on a board: where the GPU's and the game thread's time
# goes, per render pass, per draw and per function.
#
#   ./tools/quest_profile.ps1              # the game already on the board
#   ./tools/quest_profile.ps1 -Detailed    # first restarts the game with the
#                                          # GPU driver's detailed mode (needed
#                                          # for the per-pass and per-draw
#                                          # traces); go back to the board
#
# Output in build/quest-profile/<time>:
#   render-stages.txt  ovrgpuprofiler -t: each pass's bins, binning, rendering,
#                      GMEM loads and stores, with per-stage metrics
#   drawcalls.txt      ovrgpuprofiler -x -t: each draw's GPU time
#   metrics.txt        ovrgpuprofiler -r: the whole GPU, once a second
#   trace.perfetto     Perfetto: CPU scheduling and frequencies, GPU frequency,
#                      graphics atrace (open in https://ui.perfetto.dev)
#   perf.data, report.txt  simpleperf: the game's hottest functions (symbols
#                      from -Symbols, the unstripped libraries' build tree)
param(
    [switch]$Detailed,
    [int]$Seconds = 10,
    [string]$Symbols,
    [string]$Serial,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$Ndk = "$env:LOCALAPPDATA/Android/Sdk/ndk/29.0.14206865"
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path $PSScriptRoot -Parent
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$package = 'com.mariopartyrd.partyboard'
if (-not $Symbols) {
    $Symbols = if (Test-Path "$repo/build/android-arm64-quest-fdm/libdol.so") { "$repo/build/android-arm64-quest-fdm" }
               else { "$repo/build/android-arm64-quest" }
}
if (-not $Serial) {
    $devices = @(& $adb devices | Where-Object { $_ -match '^([^\s]+)\s+device$' })
    if ($devices.Count -ne 1) { throw 'Connect one headset or specify -Serial.' }
    $Serial = ($devices[0] -split '\s+')[0]
}
function Adb([string[]]$arguments) { & $adb -s $Serial @arguments }
$out = Join-Path $repo "build/quest-profile/$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Force $out | Out-Null

if ($Detailed) {
    Adb @('shell', "ovrgpuprofiler -e $package") | Out-File "$out/detailed-on.txt"
    Adb @('shell', 'am', 'force-stop', $package) | Out-Null
    Adb @('shell', 'monkey', '-p', $package, '-c', 'android.intent.category.LAUNCHER', '1') | Out-Null
    Read-Host 'The game restarted in detailed mode: go back to the board, stay still, then press Enter here'
}
$gamePid = (Adb @('shell', 'pidof', $package) | Out-String).Trim()
if (-not $gamePid) { throw 'Start the game and go to the board first.' }

Write-Output 'GPU metrics, 10 s...'
$metrics = Start-Process -FilePath $adb -ArgumentList @('-s', $Serial, 'shell',
    'ovrgpuprofiler -r="1,2,3,6,7,8,12,14,20,21,25,27,28,31,36" | head -12') `
    -RedirectStandardOutput "$out/metrics.txt" -RedirectStandardError "$out/metrics-err.txt" -WindowStyle Hidden -PassThru
$metrics.WaitForExit(20000) | Out-Null

Write-Output 'Render stages (1 s) and draw calls (0.5 s)...'
Adb @('shell', 'ovrgpuprofiler -t 1 -s="1,6,7,8,12,20,21,27,28,36" -v') | Out-File "$out/render-stages.txt" -Encoding utf8
Adb @('shell', 'ovrgpuprofiler -x -t 0.5 -v') | Out-File "$out/drawcalls.txt" -Encoding utf8

Write-Output "Perfetto, $Seconds s..."
$config = @"
buffers { size_kb: 65536 fill_policy: RING_BUFFER }
data_sources { config { name: "linux.ftrace" ftrace_config {
  ftrace_events: "sched/sched_switch" ftrace_events: "sched/sched_wakeup" ftrace_events: "power/cpu_frequency"
  ftrace_events: "power/gpu_frequency" ftrace_events: "power/cpu_idle"
  atrace_categories: "gfx" atrace_categories: "view" atrace_categories: "sched" atrace_categories: "freq"
  atrace_apps: "$package" } } }
data_sources { config { name: "linux.process_stats" } }
duration_ms: $($Seconds * 1000)
"@
$config | Out-File -Encoding ascii "$out/perfetto.cfg"
Adb @('push', "$out/perfetto.cfg", '/data/local/tmp/partyboard-perfetto.cfg') | Out-Null
Adb @('shell', 'cat /data/local/tmp/partyboard-perfetto.cfg | perfetto --txt -c - -o /data/misc/perfetto-traces/partyboard.perfetto') | Out-File "$out/perfetto.log"
Adb @('pull', '/data/misc/perfetto-traces/partyboard.perfetto', "$out/trace.perfetto") | Out-Null

Write-Output "simpleperf, $Seconds s..."
Adb @('shell', "simpleperf record --app $package -g --duration $Seconds -o /data/local/tmp/partyboard-perf.data") |
    Out-File "$out/simpleperf.log"
Adb @('shell', "run-as $package cat /data/local/tmp/partyboard-perf.data > /dev/null 2>&1; cp /data/local/tmp/partyboard-perf.data /sdcard/partyboard-perf.data") | Out-Null
Adb @('pull', '/data/local/tmp/partyboard-perf.data', "$out/perf.data") 2>$null | Out-Null
if (-not (Test-Path "$out/perf.data")) { Adb @('pull', '/sdcard/partyboard-perf.data', "$out/perf.data") | Out-Null }
$scripts = Join-Path $Ndk 'simpleperf'
if ((Test-Path "$out/perf.data") -and (Test-Path "$scripts/report.py")) {
    Push-Location $out
    py -3 "$scripts/binary_cache_builder.py" -i perf.data -lib $Symbols | Out-File binary-cache.log
    py -3 "$scripts/report.py" -i perf.data --symfs binary_cache --sort dso,symbol --percent-limit 0.5 2>$null |
        Out-File report.txt -Encoding utf8
    Pop-Location
}

if ($Detailed) { Adb @('shell', 'ovrgpuprofiler -d') | Out-File "$out/detailed-off.txt" }
Write-Output "Output: $out"
