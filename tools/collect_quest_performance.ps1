param(
    [string]$Serial,
    [int]$DurationSeconds = 120,
    [string]$OutputDirectory,
    [switch]$GpuTiming,
    [switch]$RestartGame,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
if ($DurationSeconds -lt 10 -or $DurationSeconds -gt 1800) { throw 'Duration must be between 10 and 1800 seconds.' }
if ($GpuTiming -and -not $RestartGame) { throw 'GPU timing is enabled at startup; specify -RestartGame with -GpuTiming.' }
$taskAdb = Join-Path $Sdk 'platform-tools/adb.exe'
if (-not $Serial) {
    $taskDevices = @(& $taskAdb devices | Where-Object { $_ -match '^([^\s]+)\s+device$' })
    if ($taskDevices.Count -ne 1) { throw 'Connect one headset or specify -Serial.' }
    $Serial = ($taskDevices[0] -split '\s+')[0]
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) "build/quest-performance/$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$taskPreviousTiming = (& $taskAdb -s $Serial shell getprop debug.partyboard.gpu_timing).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Headset property query failed.' }
if ($GpuTiming -and $taskPreviousTiming -notin @('', '0', '1')) { throw 'Unexpected existing GPU timing property; preserve it unchanged.' }
$taskCapture = $null
try {
    if ($GpuTiming) {
        & $taskAdb -s $Serial shell setprop debug.partyboard.gpu_timing 1
        if ($LASTEXITCODE -ne 0) { throw 'Cannot enable GPU timing on this headset.' }
    }
    if ($RestartGame) {
        & $taskAdb -s $Serial shell am force-stop com.mariopartyrd.partyboard
        & $taskAdb -s $Serial shell am start -n com.mariopartyrd.partyboard/.PartyBoardActivity
        if ($LASTEXITCODE -ne 0) { throw 'Game launch failed.' }
    }
    $taskGamePid = (& $taskAdb -s $Serial shell pidof com.mariopartyrd.partyboard).Trim()
    if (-not $taskGamePid -or $LASTEXITCODE -ne 0) { throw 'Start Party Board before recording.' }
    $taskLog = Join-Path $OutputDirectory 'quest.log'
    $taskCapture = Start-Process -FilePath $taskAdb -ArgumentList @('-s', $Serial, 'logcat', "--pid=$taskGamePid", '-v', 'threadtime', '-T', '1') `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $taskLog -RedirectStandardError (Join-Path $OutputDirectory 'adb-errors.log')
    $taskUntil = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
    Write-Output "Recording $DurationSeconds seconds to $taskLog. Play the same board route for each comparison."
    while ([DateTime]::UtcNow -lt $taskUntil -and -not $taskCapture.HasExited) { Start-Sleep -Seconds 1 }
} finally {
    if ($taskCapture -and -not $taskCapture.HasExited) { Stop-Process -Id $taskCapture.Id; $taskCapture.WaitForExit() }
    if ($GpuTiming) {
        if ($taskPreviousTiming) { & $taskAdb -s $Serial shell setprop debug.partyboard.gpu_timing $taskPreviousTiming }
        else { & $taskAdb -s $Serial shell "setprop debug.partyboard.gpu_timing ''" }
        Write-Output 'Previous GPU timing property restored; the running game keeps its startup setting until restarted.'
    }
}
& py -3 (Join-Path $PSScriptRoot 'analyze_quest_performance.py') $taskLog --output $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Performance analysis failed.' }
