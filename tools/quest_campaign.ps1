# One headset session for many A/B switches: the player stays on the same
# view (the Toad board, say) while this script flips one switch at a time,
# keeps the game's log and the GPU's counters of each phase apart, and
# prints one row per phase.
#
#   ./tools/quest_campaign.ps1                  # live phases, about 16 minutes
#   ./tools/quest_campaign.ps1 -RestartPhases   # then the startup switches:
#                                               # the game restarts, the player
#                                               # goes back to the board each time
#
# Live phases use switches the game reads while running; the eyes'
# resolution is pinned (debug.partyboard.eye_scale) so every phase draws the
# same pixels, and the game is frozen on one image (debug.partyboard.freeze)
# so every phase draws the same scene; a reference phase runs before each
# tested one and the tested phase is compared with it (delta_* columns), so
# heat and clocks drifting over the session cancel out (-NoAlternate: one
# reference only, -NoFreeze: the game keeps playing). Per phase, the first
# -SettleSeconds are left out. Every
# switch is restored afterwards. Output in build/quest-campaign/<time>: the
# whole log and GPU counters, one folder per phase (phase.log, gpu.log,
# summary.json from analyze_quest_performance.py) and campaign.csv.
param(
    [string]$Serial,
    [int]$PhaseSeconds = 40,
    [int]$SettleSeconds = 10,
    [int]$EyeScale = 95,
    [switch]$RestartPhases,
    [switch]$NoFreeze,
    [switch]$NoAlternate,
    # The scene the restart phases wait for (89: the Toad board), and how long.
    [int]$Scene = 89,
    [int]$SceneWaitSeconds = 300,
    [string]$OutputDirectory,
    # Analyze a campaign already recorded (no headset): its folder.
    [string]$AnalyzeDirectory,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
if ($PhaseSeconds -lt 20 -or $PhaseSeconds -gt 600) { throw 'PhaseSeconds must be 20 to 600.' }
if ($SettleSeconds -lt 0 -or $SettleSeconds -ge $PhaseSeconds - 10) { throw 'SettleSeconds must leave at least 10 s per phase.' }
$repo = Split-Path $PSScriptRoot -Parent
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$package = 'com.mariopartyrd.partyboard'
$eye = "$EyeScale"

# Every switch a phase may set; each phase sets all of them (unset = default).
$switches = @('debug.partyboard.sort_opaque', 'debug.partyboard.stereo_crossing', 'debug.partyboard.layer_filter',
              'debug.partyboard.gpu_level', 'debug.partyboard.eye_scale', 'debug.partyboard.tev_overflow',
              'debug.partyboard.xr_priority', 'debug.partyboard.stereo_msaa', 'debug.partyboard.opaque_blend',
              'debug.partyboard.hud_rate', 'debug.partyboard.xr_pacing', 'debug.partyboard.anisotropy',
              'debug.partyboard.shader_f16', 'debug.partyboard.freeze', 'debug.partyboard.visibility_mask')
$freeze = if ($NoFreeze) { '' } else { '1' }
$live = @(
    @{ name = 'reference';     props = @{ 'debug.partyboard.eye_scale' = $eye } },
    @{ name = 'sort-off';      props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.sort_opaque' = '0' } },
    @{ name = 'blend-on';      props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.opaque_blend' = 'on' } },
    @{ name = 'crossing-off';  props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.stereo_crossing' = '0' } },
    @{ name = 'legacy';        props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.sort_opaque' = '0'; 'debug.partyboard.opaque_blend' = 'on'; 'debug.partyboard.stereo_crossing' = '0' } },
    @{ name = 'hud-every';     props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.hud_rate' = 'full' } },
    @{ name = 'pacing-off';    props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.xr_pacing' = '0' } },
    @{ name = 'filter-normal'; props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.layer_filter' = 'normal' } },
    @{ name = 'aniso-4';       props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.anisotropy' = '4' } },
    @{ name = 'aniso-1';       props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.anisotropy' = '1' } },
    @{ name = 'res-80';        props = @{ 'debug.partyboard.eye_scale' = '80' } },
    @{ name = 'res-110';       props = @{ 'debug.partyboard.eye_scale' = '110' } },
    @{ name = 'mask-off';      props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.visibility_mask' = '0' } },
    @{ name = 'reference-end'; props = @{ 'debug.partyboard.eye_scale' = $eye } }
)
foreach ($phase in $live) { $phase.props['debug.partyboard.freeze'] = $freeze }
if (-not $NoAlternate) {
    # ref-<name> before each tested phase: A B A C A D ... A.
    $tested = @($live | Where-Object { $_.name -notlike 'reference*' })
    $live = @(foreach ($phase in $tested) {
        @{ name = "ref-$($phase.name)"; props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.freeze' = $freeze } }
        $phase
    }) + @(@{ name = 'reference-end'; props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.freeze' = $freeze } })
}
$restart = @(
    @{ name = 'restart-reference';   props = @{ 'debug.partyboard.eye_scale' = $eye } },
    @{ name = 'tev-overflow-all';    props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.tev_overflow' = 'all' } },
    @{ name = 'xr-priority-off';     props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.xr_priority' = 'off' } },
    @{ name = 'msaa-1';              props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.stereo_msaa' = '1' } },
    @{ name = 'shader-f16';          props = @{ 'debug.partyboard.eye_scale' = $eye; 'debug.partyboard.shader_f16' = '1' } }
)

function Adb([string[]]$arguments) { & $adb -s $Serial @arguments }
function Set-Switch([string]$name, [string]$value) {
    if ($value) { Adb @('shell', 'setprop', $name, $value) } else { Adb @('shell', "setprop $name ''") }
    if ($LASTEXITCODE -ne 0) { throw "Cannot set $name on this headset." }
}
function Set-Phase($phase) {
    foreach ($name in $switches) { Set-Switch $name ($(if ($phase.props.ContainsKey($name)) { $phase.props[$name] } else { '' })) }
}
function Mark([string]$text) { Adb @('shell', 'log', '-t', 'PartyBoardCampaign', $text) | Out-Null }
function Start-Recording([string]$dir) {
    $log = Start-Process -FilePath $adb -ArgumentList @('-s', $Serial, 'logcat', '-v', 'threadtime', '-T', '1') `
        -RedirectStandardOutput (Join-Path $dir 'campaign-logcat.txt') -RedirectStandardError (Join-Path $dir 'logcat-err.txt') `
        -WindowStyle Hidden -PassThru
    # The GPU's counters once a second, each line stamped with the headset's clock.
    $gpu = Start-Process -FilePath $adb -ArgumentList @('-s', $Serial, 'shell',
        'ovrgpuprofiler --realtime=2,3,4,7,8,11,12,13,17,25,26,32,33,40 | while read l; do echo "$(date +%T) $l"; done') `
        -RedirectStandardOutput (Join-Path $dir 'campaign-gpu.txt') -RedirectStandardError (Join-Path $dir 'gpu-err.txt') `
        -WindowStyle Hidden -PassThru
    return @($log, $gpu)
}
function Stop-Recording($processes) {
    Adb @('shell', 'pkill -f ovrgpuprofiler') 2>$null | Out-Null
    Start-Sleep -Seconds 2
    foreach ($p in $processes) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue } }
}
function Wait-Scene([string]$logPath, [datetime]$since) {
    $deadline = (Get-Date).AddSeconds($SceneWaitSeconds)
    while ((Get-Date) -lt $deadline) {
        $hit = Select-String -Path $logPath -Pattern "PartyBoardQuest: Scene $Scene`: spatial rendering" -ErrorAction SilentlyContinue |
            Select-Object -Last 1
        if ($hit -and (Get-Item $logPath).LastWriteTime -gt $since -and $hit.LineNumber -gt $script:sceneLine) {
            $script:sceneLine = $hit.LineNumber
            return $true
        }
        Start-Sleep -Seconds 3
    }
    return $false
}

if ($AnalyzeDirectory) {
    $OutputDirectory = $AnalyzeDirectory
} else {
    if (-not $Serial) {
        $devices = @(& $adb devices | Where-Object { $_ -match '^([^\s]+)\s+device$' })
        if ($devices.Count -ne 1) { throw 'Connect one headset or specify -Serial.' }
        $Serial = ($devices[0] -split '\s+')[0]
    }
    if (-not (Adb @('shell', 'pidof', $package) | Out-String).Trim()) { throw 'Start the game and go to the view to measure first.' }
    if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo "build/quest-campaign/$(Get-Date -Format yyyyMMdd-HHmmss)" }
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    $previous = @{}
    foreach ($name in $switches) { $previous[$name] = (Adb @('shell', 'getprop', $name) | Out-String).Trim() }
    $recording = Start-Recording $OutputDirectory
    $logPath = Join-Path $OutputDirectory 'campaign-logcat.txt'
    $script:sceneLine = 0
    try {
        foreach ($phase in $live) {
            Set-Phase $phase
            Mark "phase=$($phase.name) start"
            Write-Output ("{0:HH:mm:ss} {1,-18} {2} s (stay still, same view)" -f (Get-Date), $phase.name, $PhaseSeconds)
            Start-Sleep -Seconds $PhaseSeconds
            Mark "phase=$($phase.name) end"
        }
        if ($RestartPhases) {
            foreach ($phase in $restart) {
                Set-Phase $phase
                # Wait for an arrival after this restart, not the one already in the log.
                $last = Select-String -Path $logPath -Pattern "PartyBoardQuest: Scene $Scene`: spatial rendering" -ErrorAction SilentlyContinue | Select-Object -Last 1
                $script:sceneLine = if ($last) { $last.LineNumber } else { 0 }
                Adb @('shell', 'am', 'force-stop', $package) | Out-Null
                $since = Get-Date
                Adb @('shell', 'am', 'start', '-n', "$package/.PartyBoardActivity") | Out-Null
                Write-Output ("{0:HH:mm:ss} {1,-18} game restarted: go back to scene {2}, then stay still" -f (Get-Date), $phase.name, $Scene)
                if (-not (Wait-Scene $logPath $since)) { Write-Warning "Scene $Scene not reached for $($phase.name); skipped."; continue }
                Start-Sleep -Seconds 20 # the board's intro camera
                if ($freeze) { Set-Switch 'debug.partyboard.freeze' $freeze; Start-Sleep -Seconds 2 }
                Mark "phase=$($phase.name) start"
                Write-Output ("{0:HH:mm:ss} {1,-18} {2} s (stay still, same view)" -f (Get-Date), $phase.name, $PhaseSeconds)
                Start-Sleep -Seconds $PhaseSeconds
                Mark "phase=$($phase.name) end"
            }
        }
    } finally {
        foreach ($name in $switches) { Set-Switch $name $previous[$name] }
        Stop-Recording $recording
    }
}

# Each phase: the game's lines and the GPU's counters between its marks, less
# the settling seconds; then one row of medians and averages.
$logPath = Join-Path $OutputDirectory 'campaign-logcat.txt'
$gpuLines = @(Get-Content (Join-Path $OutputDirectory 'campaign-gpu.txt') -Encoding UTF8 -ErrorAction SilentlyContinue)
$lines = Get-Content $logPath -Encoding UTF8
$marks = @($lines | ForEach-Object { if ($_ -match 'PartyBoardCampaign: phase=(\S+) start') { $Matches[1] } })
function Time-Of([string]$line) { [datetime]::ParseExact($line.Substring(6, 12), 'HH:mm:ss.fff', $null) }
function Average($values) { if (@($values).Count) { [math]::Round((@($values) | Measure-Object -Average).Average, 2) } else { $null } }
$rows = @()
foreach ($name in $marks) {
    $start = -1; $end = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match "PartyBoardCampaign: phase=$([regex]::Escape($name)) start") { $start = $i }
        elseif ($start -ge 0 -and $lines[$i] -match "PartyBoardCampaign: phase=$([regex]::Escape($name)) end") { $end = $i; break }
    }
    if ($start -lt 0 -or $end -lt 0) { Write-Warning "No marks for $name"; continue }
    $t0 = (Time-Of $lines[$start]).AddSeconds($SettleSeconds)
    $t1 = Time-Of $lines[$end]
    $phaseLines = @($lines[($start + 1)..($end - 1)] | Where-Object {
        $_.Length -gt 30 -and $_.Substring(0, 2) -match '\d\d' -and (Time-Of $_) -ge $t0 })
    $gamePid = ($phaseLines | Where-Object { $_ -match 'PartyBoardQuest: ' } | Select-Object -First 1)
    $gamePid = if ($gamePid) { ($gamePid.Substring(19).TrimStart() -split '\s+')[0] } else { '' }
    $game = @($phaseLines | Where-Object { $_.Substring(19).TrimStart().StartsWith("$gamePid ") })
    $gpu = @($gpuLines | Where-Object {
        $_.Length -gt 9 -and ($_.Substring(0, 8) -ge $t0.ToString('HH:mm:ss')) -and ($_.Substring(0, 8) -lt $t1.ToString('HH:mm:ss')) })
    $dir = Join-Path $OutputDirectory ($name -replace '[^\w-]', '_')
    New-Item -ItemType Directory -Force $dir | Out-Null
    Set-Content -Path (Join-Path $dir 'phase.log') -Value $game -Encoding UTF8
    Set-Content -Path (Join-Path $dir 'gpu.log') -Value $gpu -Encoding UTF8
    py -3 (Join-Path $repo 'tools/analyze_quest_performance.py') (Join-Path $dir 'phase.log') --output $dir | Out-Null
    $summary = Get-Content (Join-Path $dir 'summary.json') -Raw | ConvertFrom-Json
    $median = { param($block, $key) if ($block -and $block.$key) { $block.$key.median } else { $null } }
    $counter = { param($label) @($gpu | Where-Object { $_ -match [regex]::Escape($label) } | ForEach-Object { [double](($_ -split ':')[-1].Trim()) }) }
    $number = { param($pattern) @($game | ForEach-Object { if ($_ -match $pattern) { [double]$Matches[1] } }) }
    $images = & $median $summary.metrics 'world_new_hz'
    $res = & $median $summary.metrics 'resolution_percent'
    $fragments = Average (& $counter 'Fragments Shaded / Second')
    $rows += [pscustomobject]@{
        phase            = $name
        images_s         = $images
        stutters         = & $median $summary.game 'stutters'
        off_cadence_pct  = if ($null -ne (& $median $summary.metrics 'off_cadence_percent')) { [math]::Round((& $median $summary.metrics 'off_cadence_percent'), 1) } else { $null }
        res_pct          = $res
        # Fragments per new image, for the pinned resolution: what a switch saves.
        frag_M_image     = if ($fragments -and $images) { [math]::Round($fragments / $images / 1e6, 1) } else { $null }
        gpu_util_pct     = Average (& $counter 'GPU % Utilization')
        gpu_MHz          = if ($f = Average (& $counter 'GPU Frequency')) { [math]::Round($f / 1e6) } else { $null }
        tex_stall_pct    = Average (& $counter '% Texture Fetch Stall')
        mem_stall_pct    = Average (& $counter '% Stalled on System Memory')
        read_GB_s        = if ($r = Average (& $counter 'Read Total')) { [math]::Round($r / 1e9, 2) } else { $null }
        compositor_ms    = Average (& $number 'compositor/gpu_frametime=([\d.]+)ms')
        motion_photon_ms = Average (& $number 'app/motion_to_photon_latency=([\d.]+)ms')
        predicted_ms     = Average (& $number 'VrApi\s*: FPS=.*?Prd=(\d+)ms')
        draws_eye        = & $median $summary.draws 'world_draws_avg'
    }
}
# Each tested phase against the reference just before it (ref-<name>), else
# against the first reference: relative change of the costs.
$byName = @{}; foreach ($row in $rows) { $byName[$row.phase] = $row }
$firstRef = $rows | Where-Object { $_.phase -like 'ref*' -or $_.phase -like 'reference*' -or $_.phase -like 'restart-reference' } | Select-Object -First 1
function Delta($value, $base) { if ($null -ne $value -and $base) { [math]::Round(100.0 * ($value - $base) / $base, 1) } else { $null } }
foreach ($row in $rows) {
    $ref = if ($byName.ContainsKey("ref-$($row.phase)")) { $byName["ref-$($row.phase)"] } elseif ($row.phase -notlike 'ref*') { $firstRef } else { $null }
    $row | Add-Member -NotePropertyName delta_frag_pct -NotePropertyValue $(if ($ref) { Delta $row.frag_M_image $ref.frag_M_image } else { $null })
    $row | Add-Member -NotePropertyName delta_gpu_pct -NotePropertyValue $(if ($ref) { Delta $row.gpu_util_pct $ref.gpu_util_pct } else { $null })
    $row | Add-Member -NotePropertyName delta_m2p_pct -NotePropertyValue $(if ($ref) { Delta $row.motion_photon_ms $ref.motion_photon_ms } else { $null })
}
$rows | Export-Csv -Path (Join-Path $OutputDirectory 'campaign.csv') -NoTypeInformation -Encoding UTF8
$rows | Format-Table phase, images_s, stutters, off_cadence_pct, res_pct, frag_M_image, gpu_util_pct, gpu_MHz -AutoSize | Out-String
$rows | Format-Table phase, tex_stall_pct, mem_stall_pct, read_GB_s, compositor_ms, motion_photon_ms, predicted_ms, draws_eye -AutoSize | Out-String
$rows | Where-Object { $_.phase -notlike 'ref-*' } | Format-Table phase, delta_frag_pct, delta_gpu_pct, delta_m2p_pct -AutoSize | Out-String
Write-Output "Output: $OutputDirectory"
