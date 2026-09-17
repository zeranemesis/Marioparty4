# Runs a night of unattended campaigns, one after another.
#
# The point of a night is coverage bought while nobody is watching, so the rules
# are different from an interactive run:
#
#   * a failing step NEVER stops the ones after it. A defect found in step 2
#     must not prevent step 5 from finding a different one. Every step runs.
#   * a failure is never retried to obtain a green. The occurrence and its
#     report stay on disk exactly as they landed, and the night continues.
#   * every step's exit code is recorded, so the morning summary says what
#     happened rather than what was hoped.
#
# Each step is a scenario from a manifest, plus the extra switches that scenario
# needs. They are listed here rather than inferred, because the knowledge of
# which probe belongs with which scenario is real knowledge.
#
#   tools\run_night.ps1 -DiscPath "<iso>"
#   tools\run_night.ps1 -DiscPath "<iso>" -Only 1-w04-monkey,4-rollback
#   tools\run_night.ps1 -List
#
# Exit code: 0 every step passed, 1 at least one did not, 2 nothing ran.

param(
    [string]$DiscPath = '',
    [string[]]$Only = @(),
    [switch]$List
)

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$campaign = Join-Path $PSScriptRoot 'netplay_campaign.ps1'

# Extra is splatted from a VARIABLE, never written inline as @( ... ) at the
# call site: `@($x)` there is an array expression that binds positionally to the
# next parameter, which silently became -BinaryDirectory and killed every step
# of the first night in under a second.
$steps = @(
    @{ Name = '1-w04-monkey'; Manifest = 'tests/scenarios/generated.json'
       Scenario = 'w04-monkey-1001'; Extra = @{}
       Why = "Boo's Haunted Bash driven by seed 1001, hunting the seven mechanics the recording never touches." },
    @{ Name = '2-w01-monkey'; Manifest = 'tests/scenarios/generated.json'
       Scenario = 'w01-monkey-2001'; Extra = @{}
       Why = "Toad's Midway Madness, reached exactly once before by a replay that walked and stopped." },
    @{ Name = '4-rollback'; Manifest = 'tests/scenarios/scenarios.json'
       Scenario = 'w04-board-replay'; Extra = @{ ForceRollback = '300:1,2,4,8' }
       Why = 'SAVE/RESTORE/REPLAY under the arming policy measured today: 9 armed, 9 compared, 9 passed.' },
    @{ Name = '5-transitions'; Manifest = 'tests/scenarios/scenarios.json'
       Scenario = 'w04-results-unload'; Extra = @{ Repeat = 6 }
       Why = 'Overlay transition stress: six passes through the results screen, where D3 lived.' },
    @{ Name = '6-regression-d3'; Manifest = 'tests/scenarios/regression-proofs.json'
       Scenario = 'd3-first-unload'; Extra = @{}
       Why = 'The D3 path, with the fix in place. Must stay at zero audio lifetime violations.' },
    @{ Name = '7-regression-d5'; Manifest = 'tests/scenarios/scenarios.json'
       Scenario = 'w04-boot-smoke'; Extra = @{ ForceRollback = '600:1,2,4' }
       Why = 'The D5 path. The probe must reach its first comparison and pass it.' }
)

if ($List) {
    foreach ($step in $steps) {
        Write-Output ('  {0,-16} {1}' -f $step.Name, $step.Scenario)
        Write-Output ('  {0,-16} {1}' -f '', $step.Why)
    }
    exit 0
}
if (-not $DiscPath) { Write-Output 'Need -DiscPath.'; exit 2 }

$selected = if ($Only.Count -gt 0) { @($steps | Where-Object { $Only -contains $_.Name }) } else { $steps }
if ($selected.Count -eq 0) { Write-Output 'No step selected.'; exit 2 }

$logPath = Join-Path $projectPath 'work/nuit-machine.log'
New-Item -ItemType Directory -Path (Split-Path $logPath -Parent) -Force | Out-Null
Set-Content -LiteralPath $logPath -Value '' -Encoding utf8
function Say([string]$text) {
    $line = (Get-Date).ToString('HH:mm:ss') + '  ' + $text
    Add-Content -LiteralPath $logPath -Value $line
    Write-Output $line
}

Say ("night starting: {0} steps" -f $selected.Count)
$results = @()
foreach ($step in $selected) {
    Say ''
    Say ("=== {0} : {1} ===" -f $step.Name, $step.Scenario)
    Say ("    {0}" -f $step.Why)
    $started = Get-Date
    $code = 99
    # A step that throws must not take the night with it.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $extra = $step.Extra
        & $campaign -DiscPath $DiscPath -Manifest $step.Manifest -Scenario $step.Scenario `
            -Label $step.Name @extra 2>&1 | ForEach-Object { Say ("    " + $_) }
        $code = $LASTEXITCODE
    } catch {
        Say ("    exception: " + $_.Exception.Message)
        $code = 99
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($null -eq $code) { $code = 99 }
    $minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
    # 0 passed, 1 a run failed, 2 nothing ran, 3 the harness itself broke.
    $verdict = switch ($code) { 0 { 'PASS' } 1 { 'FAILURES KEPT' } 2 { 'NOTHING RAN' } 3 { 'HARNESS FAILURE' } default { "UNKNOWN($code)" } }
    Say ("    -> {0} in {1} min" -f $verdict, $minutes)
    $results += @{ Name = $step.Name; Verdict = $verdict; Code = $code; Minutes = $minutes }
}

Say ''
Say '================ night summary ================'
foreach ($r in $results) { Say ('  {0,-16} {1,-16} {2,6} min' -f $r.Name, $r.Verdict, $r.Minutes) }
$bad = @($results | Where-Object { $_.Code -ne 0 })
Say ''
Say ("{0} steps, {1} clean, {2} with something kept" -f $results.Count, ($results.Count - $bad.Count), $bad.Count)
if ($bad.Count -gt 0) {
    Say 'Nothing was retried and nothing was deleted. Read the runs under work/netplay-campaigns.'
}
Say "log: $logPath"

# The morning should not have to ask. The matrix is rebuilt from what the night
# actually produced, so the first thing readable after a night is the coverage
# it bought - or did not.
Say ''
Say 'rebuilding the coverage matrix'
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & (Join-Path $PSScriptRoot 'coverage_matrix.ps1') -Markdown 'docs/couverture.md' 2>&1 |
        Where-Object { $_ -match 'jamais atteints|exerces|reellement|valides|plateaux :|ecrit dans' } |
        ForEach-Object { Say ("  " + $_) }
} catch {
    Say ("  the matrix could not be rebuilt: " + $_.Exception.Message)
} finally {
    $ErrorActionPreference = $previous
}

if ($bad.Count -gt 0) { exit 1 }
exit 0
