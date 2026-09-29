# One GPU trace of the game on the Toad board, for Meta's ovrgpuprofiler:
# render stages (time per stage, fragments and vertices shaded, bytes read and
# written), the same without metrics, and per draw call. It tells where the
# GPU's milliseconds go before a lever (half precision, compressed textures,
# fewer draws) is chosen (docs/quest-optimization.md).
#
#   ./tools/quest_gpu_trace.ps1                 # restarts the game, waits for the board
#   ./tools/quest_gpu_trace.ps1 -NoRestart      # the game already runs on the board
#
# Detailed profiling is switched off again at the end, even if the script
# fails: it slows the game down and stays on until it is disabled.
# Output in build/quest-gpu-trace/<time>: renderstages.txt, renderstages-plain.txt,
# drawcalls.txt and logcat.txt. Nothing here parses them yet: their format is
# read from the first real capture.
param(
    [string]$Serial,
    [switch]$NoRestart,
    [int]$WaitMinutes = 9,
    [int]$SettleSeconds = 20,
    [string]$OutputDirectory,
    # Render-stage counters (ovrgpuprofiler --renderstage-metrics): clocks,
    # bytes read/written, vertices and fragments shaded, time shading each.
    [string]$StageMetrics = '1,7,8,21,28,20,27,39',
    [string]$Package = 'com.mariopartyrd.partyboard',
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
if ($WaitMinutes -lt 1 -or $WaitMinutes -gt 30) { throw 'WaitMinutes must be 1 to 30.' }
if ($SettleSeconds -lt 0 -or $SettleSeconds -gt 300) { throw 'SettleSeconds must be 0 to 300.' }
if ($StageMetrics -notmatch '^\d+(,\d+)*$') { throw 'StageMetrics is a comma-separated list of counter numbers.' }
$repo = Split-Path $PSScriptRoot -Parent
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
if (-not $Serial) {
    $devices = @(& $adb devices | Where-Object { $_ -match '^([^\s]+)\s+device$' })
    if ($devices.Count -ne 1) { throw 'Connect one headset or specify -Serial.' }
    $Serial = ($devices[0] -split '\s+')[0]
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo "build/quest-gpu-trace/$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null

function Invoke-Adb { & $adb -s $Serial @args }
$model = (Invoke-Adb shell getprop ro.product.model | Out-String).Trim()
"Headset $Serial ($model). Output: $OutputDirectory"
$enabled = $false
try {
    if (-not $NoRestart) { Invoke-Adb shell am force-stop $Package | Out-Null }
    (Invoke-Adb shell ovrgpuprofiler -e $Package 2>&1 | Select-Object -First 2) -join ' '
    $enabled = $true
    Invoke-Adb logcat -c
    if ($NoRestart) {
        if (-not (Invoke-Adb shell pidof $Package | Out-String).Trim()) { throw 'The game is not running; start it or drop -NoRestart.' }
    } else {
        Invoke-Adb shell am start -n "$Package/.PartyBoardActivity" | Out-Null
        "Launched at $(Get-Date -Format HH:mm:ss); waiting for the Toad board (scene 89)"
    }
    $deadline = (Get-Date).AddMinutes($WaitMinutes)
    $onBoard = $false
    while ((Get-Date) -lt $deadline) {
        $last = Invoke-Adb logcat -d | Select-String 'PartyBoardQuest.*Scene \d+:' | Select-Object -Last 1
        if ($last -and $last.Line -match 'Scene 89:') { $onBoard = $true; break }
        if ($NoRestart -and -not $last) { $onBoard = $true; break } # the scene line is already out of the log: trust the caller
        Start-Sleep -Seconds 5
    }
    if (-not $onBoard) { throw "Not on the board after $WaitMinutes minutes." }
    "On the board; settling $SettleSeconds s (keep the view still)"
    Start-Sleep -Seconds $SettleSeconds
    # Out-File, not `>`: Windows PowerShell 5.1 would write UTF-16.
    Invoke-Adb shell "ovrgpuprofiler -t 0.25 --renderstage-metrics=$StageMetrics" 2>&1 | Out-File -Encoding utf8 (Join-Path $OutputDirectory 'renderstages.txt')
    Start-Sleep -Seconds 2
    Invoke-Adb shell 'ovrgpuprofiler -t 0.25' 2>&1 | Out-File -Encoding utf8 (Join-Path $OutputDirectory 'renderstages-plain.txt')
    Start-Sleep -Seconds 2
    Invoke-Adb shell 'ovrgpuprofiler -x -t 0.1' 2>&1 | Out-File -Encoding utf8 (Join-Path $OutputDirectory 'drawcalls.txt')
    Start-Sleep -Seconds 2
    Invoke-Adb logcat -d | Out-File -Encoding utf8 (Join-Path $OutputDirectory 'logcat.txt')
    'Traces: ' + ((Get-ChildItem $OutputDirectory | ForEach-Object { "$($_.Name)=$($_.Length)" }) -join ' ')
} finally {
    if ($enabled) {
        (Invoke-Adb shell ovrgpuprofiler -d 2>&1 | Select-Object -First 1) -join ' '
        (Invoke-Adb shell ovrgpuprofiler -i 2>&1 | Select-Object -First 1) -join ' '
    }
}
