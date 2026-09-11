# Combines several campaign directories into one report.
#
# A long campaign is usually split across shards so it can run in parallel, and
# the answer to "did this hold up" is about all of them together, not about any
# one. This merges their runs, keeps the statistics the campaign is required to
# keep - runs, pass, crash, desync, timeout, abnormal, harness, max frames - and
# groups the crash signatures across shards so the same defect seen in two
# shards counts as one defect with two occurrences.
#
#   tools\summarize_campaigns.ps1 -Pattern "*d3stress*"
#   tools\summarize_campaigns.ps1 -Directory work\netplay-campaigns\2026-09-11_145901-d3stress-a
#
# Exit codes: 0 every run passed, 1 something did not, 2 nothing was found.

param(
    [string]$CampaignRoot = 'work/netplay-campaigns',
    [string]$Pattern = '*',
    [string[]]$Directory,
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
function Resolve-SummaryPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}

$folders = @()
if ($Directory) {
    foreach ($d in $Directory) { $folders += Get-Item -LiteralPath (Resolve-SummaryPath $d) }
} else {
    $folders = @(Get-ChildItem (Resolve-SummaryPath $CampaignRoot) -Directory -Filter $Pattern -ErrorAction SilentlyContinue)
}
if ($folders.Count -eq 0) { Write-Output 'No campaign directory matched.'; exit 2 }

$records = @()
foreach ($folder in $folders) {
    foreach ($file in Get-ChildItem $folder.FullName -Recurse -Filter 'run.json' -ErrorAction SilentlyContinue) {
        $record = Get-Content $file.FullName -Raw | ConvertFrom-Json
        $record | Add-Member -NotePropertyName campaign -NotePropertyValue $folder.Name -Force
        $records += $record
    }
}
if ($records.Count -eq 0) { Write-Output 'No run.json found in any matching campaign.'; exit 2 }

$lines = New-Object Collections.Generic.List[string]
function Emit([string]$text) { $lines.Add($text); Write-Output $text }

Emit ('campaigns : ' + ($folders.Name -join ', '))
$commits = @($records | ForEach-Object { $_.build_commit } | Sort-Object -Unique)
Emit ('commits   : ' + ($commits -join ', '))
if ($commits.Count -gt 1) {
    # Runs from two builds are not one campaign, whatever the folder names say.
    Emit 'WARNING   : these runs came from more than one commit and are not directly comparable'
}
$binaries = @($records | ForEach-Object { $_.binary_written } | Sort-Object -Unique)
Emit ('binaries  : ' + ($binaries -join ', '))
Emit ''

$counts = @{}
foreach ($name in 'PASS','CRASH','DESYNC','TIMEOUT','ABNORMAL_EXIT','HARNESS_FAILURE') {
    $counts[$name] = @($records | Where-Object { $_.result -eq $name }).Count
}
$maxFrames = (@($records | ForEach-Object { [int]$_.last_frame }) | Measure-Object -Maximum).Maximum
$totalSeconds = (@($records | ForEach-Object { [int]$_.duration_seconds }) | Measure-Object -Sum).Sum
$violations = (@($records | ForEach-Object { [int]$_.audio_lifetime_violations }) | Measure-Object -Sum).Sum

Emit ('runs           : ' + $records.Count)
foreach ($name in 'PASS','CRASH','DESYNC','TIMEOUT','ABNORMAL_EXIT','HARNESS_FAILURE') {
    Emit ('{0,-15}: {1}' -f $name.ToLower(), $counts[$name])
}
Emit ('max frames     : ' + $maxFrames)
Emit ('audio lifetime violations : ' + $violations)
Emit ('total runtime  : {0:n0} s ({1:n1} h)' -f $totalSeconds, ($totalSeconds / 3600))
Emit ''

$fingerprints = @($records | Where-Object { $_.crash_fingerprint } | Group-Object crash_fingerprint)
if ($fingerprints.Count -gt 0) {
    Emit 'crash signatures'
    foreach ($group in $fingerprints | Sort-Object Count -Descending) {
        Emit ('  {0,3} x {1}' -f $group.Count, $group.Name)
        foreach ($r in $group.Group) { Emit ('        {0} {1}/{2} frame {3}' -f $r.campaign, $r.scenario, $r.run_index, $r.last_frame) }
    }
} else {
    Emit 'crash signatures : none'
}
$desyncs = @($records | Where-Object { $_.desync_fingerprint } | Group-Object desync_fingerprint)
if ($desyncs.Count -gt 0) {
    Emit 'desync signatures'
    foreach ($group in $desyncs | Sort-Object Count -Descending) {
        Emit ('  {0,3} x {1}' -f $group.Count, $group.Name)
    }
} else {
    Emit 'desync signatures : none'
}
Emit ''

# Every run that is not a PASS is listed individually. A count can be skimmed
# past; a list has to be read.
$failures = @($records | Where-Object { $_.result -ne 'PASS' })
if ($failures.Count -gt 0) {
    Emit 'runs that did not pass'
    foreach ($r in $failures) {
        Emit ('  {0} {1}/{2}  {3}  frame {4}  {5}' -f $r.campaign, $r.scenario, $r.run_index, $r.result, $r.last_frame, ($r.notes -join '; '))
    }
} else {
    Emit 'every run passed'
}

if ($OutFile) {
    [IO.File]::WriteAllLines((Resolve-SummaryPath $OutFile), $lines)
    Write-Output ''
    Write-Output ('written to ' + (Resolve-SummaryPath $OutFile))
}

if ($failures.Count -gt 0) { exit 1 }
exit 0
