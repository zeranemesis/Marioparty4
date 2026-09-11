# Runs every test script in tools/, and says which ones ran.
#
# Why this exists. There are thirteen `test_*.ps1` scripts. `validate_netplay.ps1`
# invoked three of them. `test_audio_wait.ps1` and `test_rollback_effects.ps1`
# were referenced from nothing at all - written, committed, and then never run
# again by anything. A test nobody runs is a comment that takes time to compile.
#
#   tools\run_all_tests.ps1                       everything that needs no disc
#   tools\run_all_tests.ps1 -DiscPath "<iso>"     those, plus the ones that do
#   tools\run_all_tests.ps1 -List                 just show what would run
#
# Discovery is by glob, not by a list kept here: a new `test_*.ps1` is picked up
# the day it is added, which is the only way a runner stays complete. A script
# that needs something this invocation cannot give it - a disc, usually - is
# reported SKIPPED with the reason, never silently omitted, because "12 passed"
# means nothing without knowing that the thirteenth did not run.
#
# Exit codes: 0 every test that ran passed, 1 one did not, 2 nothing ran.

param(
    [string]$DiscPath = '',
    [string]$BinaryDirectory = 'build/aexp/RelWithDebInfo',
    # Skip these by name (without the .ps1), for a quick pass.
    [string[]]$Exclude = @(),
    [switch]$List
)

$ErrorActionPreference = 'Continue'
$projectPath = Split-Path $PSScriptRoot -Parent
function Resolve-RunnerPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}

# Scripts that need a real disc image to do anything. Everything else runs on a
# machine that has never seen the game, which is what lets CI run them.
$needsDisc = @('test_netplay_boot')

# Two scripts need more than a disc path to mean anything, and the knowledge of
# how to call them already lives in validate_netplay.ps1. Rather than guess,
# this mirrors it.
#
# test_netplay_boot is a driver with several modes, not a single test. Run with
# no mode it boots two instances, sits still for thirty seconds and fails on
# "No overlay transition observed" - which is the script correctly reporting
# that nothing happened, not a defect. -Menu is its shortest meaningful mode.
$extraArguments = @{
    'test_netplay_boot' = @{ Menu = $true; DurationSeconds = 60 }
}

# Scripts that use exit code 2 to mean "this environment cannot run me", as
# distinct from "I ran and something is wrong". Listed by name rather than
# assumed for every script, because for most of them a non-zero exit is a real
# failure and silently forgiving one would be worse than any missing coverage.
#
# test_direct_connection exits 2 when a VPN owns the priority route: it refuses
# to probe the router or open a port through someone's VPN, which is the correct
# thing to do and must not be reported as a failing test.
$notApplicableExit = @{
    'test_direct_connection' = 2
}

$scripts = @(Get-ChildItem $PSScriptRoot -Filter 'test_*.ps1' | Sort-Object Name)
if ($scripts.Count -eq 0) { Write-Output 'No test_*.ps1 found.'; exit 2 }

if ($List) {
    foreach ($script in $scripts) {
        $name = [IO.Path]::GetFileNameWithoutExtension($script.Name)
        $note = if ($needsDisc -contains $name) { 'needs a disc' } else { 'no disc' }
        Write-Output ('  {0,-32} {1}' -f $name, $note)
    }
    exit 0
}

$binary = Join-Path (Resolve-RunnerPath $BinaryDirectory) 'partyboard.exe'
if (-not (Test-Path -LiteralPath $binary)) {
    Write-Output "partyboard.exe not found at $binary. Build first, or pass -BinaryDirectory."
    exit 2
}

$results = @()
$startedAll = Get-Date
foreach ($script in $scripts) {
    $name = [IO.Path]::GetFileNameWithoutExtension($script.Name)
    if ($Exclude -contains $name) {
        $results += @{ Name = $name; Outcome = 'SKIPPED'; Seconds = 0; Reason = 'excluded on the command line' }
        continue
    }
    if (($needsDisc -contains $name) -and -not $DiscPath) {
        $results += @{ Name = $name; Outcome = 'SKIPPED'; Seconds = 0; Reason = 'needs -DiscPath' }
        continue
    }

    Write-Output ''
    Write-Output ('=== {0} ===' -f $name)
    $started = Get-Date
    $arguments = @{}
    if ($needsDisc -contains $name) { $arguments['DiscPath'] = $DiscPath }
    if ($extraArguments.ContainsKey($name)) {
        foreach ($key in $extraArguments[$name].Keys) { $arguments[$key] = $extraArguments[$name][$key] }
    }
    try {
        & $script.FullName @arguments 2>&1 | ForEach-Object { Write-Output "  $_" }
        $code = $LASTEXITCODE
    } catch {
        Write-Output ("  threw: " + $_.Exception.Message)
        $code = 99
    }
    $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    $reason = ''
    $outcome = if ($code -eq 0) {
        'PASS'
    } elseif ($notApplicableExit.ContainsKey($name) -and $code -eq $notApplicableExit[$name]) {
        $reason = "the script reported this environment cannot run it (exit $code)"
        'NOT_APPLICABLE'
    } else {
        "FAIL(exit $code)"
    }
    $results += @{ Name = $name; Outcome = $outcome; Seconds = $seconds; Reason = $reason }
    Write-Output ('--- {0}: {1} in {2} s' -f $name, $outcome, $seconds)
}

Write-Output ''
Write-Output '================ summary ================'
foreach ($result in $results) {
    $suffix = if ($result.Reason) { " ($($result.Reason))" } else { '' }
    Write-Output ('  {0,-32} {1,-16} {2,6} s{3}' -f $result.Name, $result.Outcome, $result.Seconds, $suffix)
}
$passed = @($results | Where-Object { $_.Outcome -eq 'PASS' }).Count
$failed = @($results | Where-Object { $_.Outcome -like 'FAIL*' }).Count
$skipped = @($results | Where-Object { $_.Outcome -eq 'SKIPPED' }).Count
$notApplicable = @($results | Where-Object { $_.Outcome -eq 'NOT_APPLICABLE' }).Count
Write-Output ''
Write-Output ('{0} scripts: {1} passed, {2} failed, {3} skipped, {4} not applicable, {5:n0} s total' -f
    $results.Count, $passed, $failed, $skipped, $notApplicable, ((Get-Date) - $startedAll).TotalSeconds)
if ($skipped -gt 0 -or $notApplicable -gt 0) {
    Write-Output 'Scripts that did not run prove nothing. The reasons are above.'
}

if ($failed -gt 0) { exit 1 }
if ($passed -eq 0) { exit 2 }
exit 0
