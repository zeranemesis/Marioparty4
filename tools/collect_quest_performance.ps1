param(
    [string]$Serial,
    [int]$DurationSeconds = 120,
    [string]$OutputDirectory,
    [switch]$GpuTiming,
    [switch]$RestartGame,
    # A/B switches, restored after the capture. RenderHz: new images per
    # second at most (0: the display rate); the game reads it within 2 s.
    [int]$RenderHz = -1,
    # Eyes: 0 one draw per eye, 1 one draw cut by clip distances, 2 one draw
    # cut by a fragment test (the default when unset). Read at startup, so
    # with -RestartGame.
    [ValidateSet('', '0', '1', '2')] [string]$InstancedStereo = '',
    # The eyes' MSAA: 1 (none) or 4 (the default when unset); WebGPU has no
    # 2x. Read at startup.
    [ValidateSet('', '1', '4')] [string]$StereoMsaa = '',
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
if ($DurationSeconds -lt 10 -or $DurationSeconds -gt 1800) { throw 'Duration must be between 10 and 1800 seconds.' }
if ($GpuTiming -and -not $RestartGame) { throw 'GPU timing is enabled at startup; specify -RestartGame with -GpuTiming.' }
if ($StereoMsaa -ne '' -and -not $RestartGame) { throw 'MSAA is chosen at startup; specify -RestartGame with -StereoMsaa.' }
if ($InstancedStereo -ne '' -and -not $RestartGame) { throw 'Instanced stereo is chosen at startup; specify -RestartGame with -InstancedStereo.' }
if ($RenderHz -gt 240) { throw 'RenderHz must be 0 (display rate) to 240.' }
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
$taskPreviousRenderHz = (& $taskAdb -s $Serial shell getprop debug.partyboard.render_hz).Trim()
$taskPreviousInstanced = (& $taskAdb -s $Serial shell getprop debug.partyboard.instanced_stereo).Trim()
$taskPreviousMsaa = (& $taskAdb -s $Serial shell getprop debug.partyboard.stereo_msaa).Trim()
function Restore-Property([string]$name, [string]$value) {
    if ($value) { & $taskAdb -s $Serial shell setprop $name $value }
    else { & $taskAdb -s $Serial shell "setprop $name ''" }
}
$taskCapture = $null
try {
    if ($GpuTiming) {
        & $taskAdb -s $Serial shell setprop debug.partyboard.gpu_timing 1
        if ($LASTEXITCODE -ne 0) { throw 'Cannot enable GPU timing on this headset.' }
    }
    if ($RenderHz -ge 0) {
        & $taskAdb -s $Serial shell setprop debug.partyboard.render_hz $RenderHz
        if ($LASTEXITCODE -ne 0) { throw 'Cannot set the render rate cap on this headset.' }
    }
    if ($StereoMsaa -ne '') {
        & $taskAdb -s $Serial shell setprop debug.partyboard.stereo_msaa $StereoMsaa
        if ($LASTEXITCODE -ne 0) { throw 'Cannot set the eyes MSAA on this headset.' }
    }
    if ($InstancedStereo -ne '') {
        & $taskAdb -s $Serial shell setprop debug.partyboard.instanced_stereo $InstancedStereo
        if ($LASTEXITCODE -ne 0) { throw 'Cannot enable instanced stereo on this headset.' }
    }
    # What the comparison protocol asks to record with every capture.
    [ordered]@{
        commit = (git -C (Split-Path $PSScriptRoot -Parent) rev-parse --short HEAD 2>$null)
        headset = (& $taskAdb -s $Serial shell getprop ro.product.model).Trim()
        durationSeconds = $DurationSeconds; gpuTiming = [bool]$GpuTiming
        renderHz = $(if ($RenderHz -ge 0) { $RenderHz } else { $taskPreviousRenderHz })
        stereoMsaa = $(if ($StereoMsaa -ne '') { $StereoMsaa } elseif ($taskPreviousMsaa) { $taskPreviousMsaa } else { 'unset (4)' })
        instancedStereo = $(if ($InstancedStereo -ne '') { $InstancedStereo } elseif ($taskPreviousInstanced) { $taskPreviousInstanced } else { 'unset (2)' })
    } | ConvertTo-Json | Out-File -Encoding utf8 (Join-Path $OutputDirectory 'settings.json')
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
    if ($RenderHz -ge 0) { Restore-Property debug.partyboard.render_hz $taskPreviousRenderHz }
    if ($StereoMsaa -ne '') { Restore-Property debug.partyboard.stereo_msaa $taskPreviousMsaa }
    if ($InstancedStereo -ne '') {
        Restore-Property debug.partyboard.instanced_stereo $taskPreviousInstanced
        Write-Output 'Previous instanced stereo property restored; the running game keeps its startup setting until restarted.'
    }
}
& py -3 (Join-Path $PSScriptRoot 'analyze_quest_performance.py') $taskLog --output $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Performance analysis failed.' }
