# D33 regression: a lockstep session under network delay must not diverge.
#
# A delayed peer waits for its inputs and keeps rendering frames that carry no
# tick. Before the fix, Hu3DExec reran the motion passes on those frames and the
# first application of an attribute track allocated in HEAP_DATA a tick early:
# HEAPS diverged at frame 5899 on w01 in three runs out of five. The fix passed
# twenty out of twenty with this exact shape; seed 105 is one of the unfixed
# runs that diverged.
#
# The run only means something if it went past 5899. A slow machine that never
# reaches it would pass by not getting there, so the frame reached is checked.
#
# Exit codes: 0 pass, 1 fail, 2 this environment cannot run it (no python).
#
# SCRIPTED, one machine: this shapes packets, it is not a real network.

param(
    [Parameter(Mandatory=$true)][string]$DiscPath,
    [string]$BinaryDirectory = 'build/aexp/RelWithDebInfo',
    [int]$DurationSeconds = 360,
    [int]$Seed = 105,
    # The divergence point the run must get past to prove anything.
    [int]$RequiredFrame = 6000
)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Output 'not applicable: python is needed for tools/netem_relay.py'
    exit 2
}

$boot = Join-Path $PSScriptRoot 'test_netplay_boot.ps1'
# Windows PowerShell turns each stderr line of a native command into an error
# record under 2>&1; with Stop, the first one would end this script. The child's
# exit code is the verdict, read below.
$ErrorActionPreference = 'Continue'
$output = & powershell -NoProfile -ExecutionPolicy Bypass -File $boot -DiscPath $DiscPath `
    -BinaryDirectory $BinaryDirectory -Walk -DurationSeconds $DurationSeconds `
    -Relay "80,20,0,0,$Seed" -OutputDirectory 'work/netplay-latency' 2>&1
$code = $LASTEXITCODE
$output | ForEach-Object { Write-Output "  $_" }
if ($code -ne 0) {
    Write-Output "FAIL: the delayed session stopped (exit $code)."
    exit 1
}

$logs = ($output | Where-Object { "$_" -match '^Real boot logs: (.+)$' } | Select-Object -First 1)
if (-not $logs -or "$logs" -notmatch '^Real boot logs: (.+)$') {
    Write-Output 'FAIL: test_netplay_boot did not say where its logs are.'
    exit 1
}
$native = Join-Path $Matches[1].Trim() 'peer-0-native.log'
$reached = 0
foreach ($line in Get-Content -LiteralPath $native) {
    if ($line -match ' event=\w+ .*? frame=(\d+) ') { $reached = [Math]::Max($reached, [int]$Matches[1]) }
}
if ($reached -lt $RequiredFrame) {
    Write-Output "FAIL: no divergence, but the walk only reached frame $reached; it must pass $RequiredFrame to prove anything."
    exit 1
}
Write-Output "PASS: no divergence under 80 ms of delay, walk reached frame $reached (past $RequiredFrame)."
exit 0
