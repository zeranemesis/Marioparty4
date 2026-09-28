# One headset session for every live A/B switch: the player stays on the
# same view (the Toad board, say) while this script flips one switch at a
# time, keeps the game's log of each phase apart, and prints a table.
#
#   ./tools/quest_campaign.ps1                    # 7 phases of 45 s, about 6 minutes
#   ./tools/quest_campaign.ps1 -PhaseSeconds 60
#
# Only switches the game reads while running are used, so the game is never
# restarted. Every switch is restored afterwards. Per phase, the first
# -SettleSeconds are left out (a resolution change, pipelines being built).
# Output in build/quest-campaign/<time>: the whole log, one log and
# summary.json per phase (analyze_quest_performance.py), and campaign.csv.
param(
    [string]$Serial,
    [int]$PhaseSeconds = 45,
    [int]$SettleSeconds = 10,
    [string]$OutputDirectory,
    # Analyze a campaign log already recorded (no headset): its path, and the game's pid in it.
    [string]$AnalyzeLog,
    [string]$GamePid,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
if ($PhaseSeconds -lt 20 -or $PhaseSeconds -gt 600) { throw 'PhaseSeconds must be 20 to 600.' }
if ($SettleSeconds -lt 0 -or $SettleSeconds -ge $PhaseSeconds - 10) { throw 'SettleSeconds must leave at least 10 s per phase.' }
$repo = Split-Path $PSScriptRoot -Parent
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
if ($AnalyzeLog) {
    if (-not $GamePid) { throw 'With -AnalyzeLog, give the game''s -GamePid.' }
    if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo "build/quest-campaign/analysis-$(Get-Date -Format yyyyMMdd-HHmmss)" }
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    $gamePid = $GamePid
    $logPath = $AnalyzeLog
    $marks = @(Get-Content $logPath -Encoding UTF8 | ForEach-Object { if ($_ -match 'PartyBoardCampaign: phase=(.+) start') { $Matches[1] } })
} else {
if (-not $Serial) {
    $devices = @(& $adb devices | Where-Object { $_ -match '^([^\s]+)\s+device$' })
    if ($devices.Count -ne 1) { throw 'Connect one headset or specify -Serial.' }
    $Serial = ($devices[0] -split '\s+')[0]
}
$package = 'com.mariopartyrd.partyboard'
$gamePid = (& $adb -s $Serial shell pidof $package).Trim()
if (-not $gamePid) { throw 'Start the game and go to the view to measure first.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo "build/quest-campaign/$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null

# The live switches (read again every 2 s by the game) and the phases.
$switches = @('debug.partyboard.sort_opaque', 'debug.partyboard.stereo_crossing', 'debug.partyboard.layer_filter')
$phases = @(
    @{ name = 'reference';      props = @{} },
    @{ name = 'sort';           props = @{ 'debug.partyboard.sort_opaque' = '1' } },
    @{ name = 'crossing';       props = @{ 'debug.partyboard.stereo_crossing' = '1' } },
    @{ name = 'sort+crossing';  props = @{ 'debug.partyboard.sort_opaque' = '1'; 'debug.partyboard.stereo_crossing' = '1' } },
    @{ name = 'filter-normal';  props = @{ 'debug.partyboard.layer_filter' = 'normal' } },
    @{ name = 'filter-none';    props = @{ 'debug.partyboard.layer_filter' = 'none' } },
    @{ name = 'reference-end';  props = @{} }  # the same as the first: heat and drift
)

function Set-Switch([string]$name, [string]$value) {
    if ($value) { & $adb -s $Serial shell setprop $name $value } else { & $adb -s $Serial shell "setprop $name ''" }
    if ($LASTEXITCODE -ne 0) { throw "Cannot set $name on this headset." }
}
$previous = @{}
foreach ($name in $switches) { $previous[$name] = (& $adb -s $Serial shell getprop $name).Trim() }

$logPath = Join-Path $OutputDirectory 'campaign-logcat.txt'
$logcat = Start-Process -FilePath $adb -ArgumentList @('-s', $Serial, 'logcat', '-v', 'threadtime', '-T', '1') `
    -RedirectStandardOutput $logPath -NoNewWindow -PassThru
$marks = @()
try {
    foreach ($phase in $phases) {
        foreach ($name in $switches) {
            Set-Switch $name ($(if ($phase.props.ContainsKey($name)) { $phase.props[$name] } else { '' }))
        }
        & $adb -s $Serial shell log -t PartyBoardCampaign "phase=$($phase.name) start" | Out-Null
        Write-Output ("{0:HH:mm:ss} {1,-14} {2} s (stay still, same view)" -f (Get-Date), $phase.name, $PhaseSeconds)
        Start-Sleep -Seconds $PhaseSeconds
        & $adb -s $Serial shell log -t PartyBoardCampaign "phase=$($phase.name) end" | Out-Null
        $marks += $phase.name
    }
} finally {
    foreach ($name in $switches) { Set-Switch $name $previous[$name] }
    Start-Sleep -Seconds 2
    if (-not $logcat.HasExited) { Stop-Process -Id $logcat.Id }
}
}

# Each phase: the game's lines between its marks, less the settling seconds.
$lines = Get-Content $logPath -Encoding UTF8
$rows = @()
foreach ($name in $marks) {
    $start = -1; $end = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match "PartyBoardCampaign: phase=$([regex]::Escape($name)) start") { $start = $i }
        elseif ($start -ge 0 -and $lines[$i] -match "PartyBoardCampaign: phase=$([regex]::Escape($name)) end") { $end = $i; break }
    }
    if ($start -lt 0 -or $end -lt 0) { Write-Warning "No marks for $name"; continue }
    $t0 = [datetime]::ParseExact($lines[$start].Substring(6, 12), 'HH:mm:ss.fff', $null)
    $phaseLines = $lines[($start + 1)..($end - 1)] | Where-Object {
        $_.Length -gt 30 -and $_.Substring(19).TrimStart().StartsWith("$gamePid ") -and
        ([datetime]::ParseExact($_.Substring(6, 12), 'HH:mm:ss.fff', $null) - $t0).TotalSeconds -ge $SettleSeconds
    }
    $dir = Join-Path $OutputDirectory ($name -replace '[^\w-]', '_')
    New-Item -ItemType Directory -Force $dir | Out-Null
    $phaseLog = Join-Path $dir 'phase.log'
    Set-Content -Path $phaseLog -Value $phaseLines -Encoding UTF8
    py -3 (Join-Path $repo 'tools/analyze_quest_performance.py') $phaseLog --output $dir | Out-Null
    $summary = Get-Content (Join-Path $dir 'summary.json') -Raw | ConvertFrom-Json
    $median = { param($block, $key) if ($block -and $block.$key) { $block.$key.median } else { $null } }
    $gpu = @($phaseLines | ForEach-Object { if ($_ -match 'device/gpu_utilization=([\d.]+)') { [double]$Matches[1] } })
    $rows += [pscustomobject]@{
        phase             = $name
        game_fps          = & $median $summary.game 'frames_per_s'
        stutters          = & $median $summary.game 'stutters'
        world_new_hz      = & $median $summary.metrics 'world_new_hz'
        off_cadence_pct   = & $median $summary.metrics 'off_cadence_percent'
        resolution_pct    = & $median $summary.metrics 'resolution_percent'
        latency_ms        = & $median $summary.metrics 'latency_ms'
        gpu_util_pct      = if ($gpu.Count) { [math]::Round(($gpu | Measure-Object -Average).Average, 1) } else { $null }
        draws_per_eye     = & $median $summary.draws 'world_draws_avg'
    }
}
$rows | Export-Csv -Path (Join-Path $OutputDirectory 'campaign.csv') -NoTypeInformation -Encoding UTF8
$rows | Format-Table -AutoSize | Out-String -Width 200
Write-Output "Output: $OutputDirectory"
