# Exercises merge_session.ps1 against synthetic session folders.
#
# Why this exists. merge_session.ps1 decides whether a two-machine session
# counts, and the only natural way to exercise it is to hold a two-machine
# session - the most expensive evidence this project can collect: two people,
# two machines, two hours. Discovering there that the merge tool mis-handles a
# divergence would waste the evening AND produce nothing.
#
# So the merge is tested here, on fixtures, against cases that a real session
# might only produce once a year:
#
#   1. two halves that agree                     -> PASS
#   2. inputs that diverge at a known row         -> FAIL, naming that row
#   3. overlay paths that diverge                 -> FAIL
#   4. two different build commits                -> FAIL, before anything else
#   5. stalls above the playability threshold     -> FAIL on playability alone,
#                                                    with determinism green
#   6. a crash on one side                        -> FAIL
#   7. a session that is too short                -> FAIL
#
# Case 5 is the one that matters most: it is the case the four original verdicts
# could not express, and the reason PLAYABILITY was added before the first real
# session rather than after it.
#
#   tools\test_merge_session.ps1
#
# Exit codes: 0 every case behaved, 1 one did not.

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$root = Join-Path $projectPath 'work/test-merge-session'
if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
New-Item -ItemType Directory -Path $root -Force | Out-Null

$failures = New-Object Collections.Generic.List[string]
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Output ("  ok    {0}" -f $name) }
    else { Write-Output ("  FAIL  {0} - {1}" -f $name, $detail); $failures.Add($name) }
}

$commit = 'a'.PadRight(40, '0')

# A recording row is nine integers per player per frame; the merge only compares
# them as text, so the exact shape does not matter here, only that two halves
# can be made to agree or disagree at a chosen row.
function New-Rows([int]$count, [int]$divergeAt) {
    $rows = New-Object Collections.Generic.List[string]
    for ($i = 0; $i -lt $count; ++$i) {
        $value = if ($divergeAt -ge 0 -and $i -ge $divergeAt) { 1 } else { 0 }
        $rows.Add("$i 0 0 0 0 0 0 0 $value")
    }
    return $rows.ToArray()
}

function New-Half {
    param(
        [string]$Case, [string]$Role, [int]$Side, [string]$Commit = $commit,
        [int]$Rows = 900, [int]$DivergeAt = -1,
        [string[]]$Overlays = @('1@2', '74@1058', '92@5385'),
        [int]$Stalled = 0, [int]$LongestStall = 0, [double]$Minutes = 10.0,
        [string]$Classification = 'USER_REQUESTED_EXIT',
        [string]$Stability = 'PASS', [string]$UserTerminated = 'YES',
        [string]$Playability = 'PASS', [string]$Hash = 'abc123@9000'
    )
    $folder = Join-Path $root "$Case-$($Role.ToLower())"
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $recording = Join-Path $folder "seat-$Side-recording.txt"
    [IO.File]::WriteAllLines($recording, (New-Rows $Rows $DivergeAt))
    $document = [ordered]@{
        schema = 2
        role = $Role
        label = $Case
        session_directory = $folder
        udp_port = 51000
        join_address = ''
        disc = 'fixture.iso'
        build_commit = $Commit
        build_tree_dirty = $false
        started_at = (Get-Date).ToString('o')
        determinism = 'DEFERRED'
        stability = $Stability
        user_terminated = $UserTerminated
        playability = $Playability
        overall = 'PENDING_MERGE'
        determinism_problems = @()
        playability_thresholds = [ordered]@{
            good_stalls_per_minute = 180
            max_stalls_per_minute = 720
            good_longest_stall_frames = 30
            max_longest_stall_frames = 180
        }
        seats = @([ordered]@{
            side = $Side
            role = $(if ($Side -eq 0) { 'host' } else { 'client' })
            classification = $Classification
            exit_code = 0
            shutdown_intent = 'USER_REQUESTED_EXIT'
            last_frame = '9000'
            last_state_hash = $Hash
            lifetime_seconds = [math]::Round($Minutes * 60, 3)
            stalled_ticks = $Stalled
            longest_stall_frames = $LongestStall
            stalls_per_minute = [math]::Round($Stalled / $Minutes, 1)
            recorded_rows = $Rows
            recording = $recording
            overlay_path = @($Overlays)
            crash_reports = @()
            minidumps = @()
            desync_reports = @()
            mem_corruption_reports = @()
        })
    }
    [IO.File]::WriteAllText((Join-Path $folder 'session.json'),
        ($document | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))
    return $folder
}

function Run-Merge([string]$a, [string]$b) {
    $output = & "$PSScriptRoot\merge_session.ps1" -HostSession $a -JoinSession $b 2>&1 | ForEach-Object { "$_" }
    return @{ Text = ($output -join "`n"); Exit = $LASTEXITCODE }
}

Write-Output 'merge_session fixtures'

# 1. Agreement.
$a = New-Half -Case 'agree' -Role 'Host' -Side 0
$b = New-Half -Case 'agree' -Role 'Join' -Side 1
$r = Run-Merge $a $b
Check 'two agreeing halves merge to PASS' ($r.Exit -eq 0 -and $r.Text -match 'MERGED OVERALL:\s+PASS') $r.Text
Check 'the merged recording is written' ($r.Text -match 'Merged recording:') 'no merged recording line'

# 2. Diverging inputs, at a row chosen in advance.
$a = New-Half -Case 'inputs' -Role 'Host' -Side 0
$b = New-Half -Case 'inputs' -Role 'Join' -Side 1 -DivergeAt 640
$r = Run-Merge $a $b
Check 'diverging inputs fail' ($r.Exit -eq 1 -and $r.Text -match 'MERGED OVERALL:\s+FAIL') $r.Text
Check 'the divergence names row 640' ($r.Text -match 'from recorded row 640 onwards') 'the first differing row was not named'
Check 'a partial recording is still kept' ($r.Text -match 'Partial recording kept') 'the prefix was discarded'

# 3. Diverging overlay paths, identical inputs.
$a = New-Half -Case 'overlay' -Role 'Host' -Side 0
$b = New-Half -Case 'overlay' -Role 'Join' -Side 1 -Overlays @('1@2', '74@1058', '89@5385')
$r = Run-Merge $a $b
Check 'diverging overlay paths fail' ($r.Exit -eq 1 -and $r.Text -match 'different overlay paths') $r.Text

# 4. Different commits. Checked before anything else, because a determinism
#    difference between two builds says nothing about either.
$a = New-Half -Case 'commits' -Role 'Host' -Side 0
$b = New-Half -Case 'commits' -Role 'Join' -Side 1 -Commit ('b'.PadRight(40, '1'))
$r = Run-Merge $a $b
Check 'different commits fail' ($r.Exit -eq 1 -and $r.Text -match 'different commits') $r.Text

# 5. THE case the four original verdicts could not express: everything agrees,
#    nothing crashed, the user quit on purpose, and the session was unplayable.
$a = New-Half -Case 'stalls' -Role 'Host' -Side 0 -Stalled 9000 -LongestStall 40
$b = New-Half -Case 'stalls' -Role 'Join' -Side 1 -Stalled 9500 -LongestStall 45
$r = Run-Merge $a $b
Check 'stalls above threshold fail the session' ($r.Exit -eq 1) $r.Text
Check 'determinism stays green while playability fails' `
    ($r.Text -match 'MERGED DETERMINISM:\s+PASS' -and $r.Text -match 'MERGED PLAYABILITY:\s+FAIL') $r.Text

# 5b. And the marginal band is reported as marginal, not quietly rounded to
#     either side.
$a = New-Half -Case 'marginal' -Role 'Host' -Side 0 -Stalled 3000 -LongestStall 40
$b = New-Half -Case 'marginal' -Role 'Join' -Side 1 -Stalled 2000 -LongestStall 35
$r = Run-Merge $a $b
Check 'the marginal band is named and still passes' `
    ($r.Exit -eq 0 -and $r.Text -match 'MERGED PLAYABILITY:\s+MARGINAL') $r.Text

# 5c. A long freeze fails even when the average is excellent - the reason the
#     longest wait is measured separately from the rate.
$a = New-Half -Case 'freeze' -Role 'Host' -Side 0 -Stalled 300 -LongestStall 300
$b = New-Half -Case 'freeze' -Role 'Join' -Side 1 -Stalled 200 -LongestStall 10
$r = Run-Merge $a $b
Check 'a five-second freeze fails despite a good average' `
    ($r.Exit -eq 1 -and $r.Text -match 'MERGED PLAYABILITY:\s+FAIL') $r.Text

# 6. A crash on one machine.
$a = New-Half -Case 'crash' -Role 'Host' -Side 0
$b = New-Half -Case 'crash' -Role 'Join' -Side 1 -Stability 'FAIL' -Classification 'PROCESS_CRASH' -UserTerminated 'NO'
$r = Run-Merge $a $b
Check 'a crash on one side fails the merge' ($r.Exit -eq 1 -and $r.Text -match 'failed stability') $r.Text

# 7. Too short to be evidence of anything.
$a = New-Half -Case 'short' -Role 'Host' -Side 0 -Rows 200
$b = New-Half -Case 'short' -Role 'Join' -Side 1 -Rows 200
$r = Run-Merge $a $b
Check 'a session too short fails' ($r.Exit -eq 1 -and $r.Text -match 'too short') $r.Text

# 8. A scenario fragment is emitted on request, and only on a pass.
$a = New-Half -Case 'fragment' -Role 'Host' -Side 0
$b = New-Half -Case 'fragment' -Role 'Join' -Side 1
$output = & "$PSScriptRoot\merge_session.ps1" -HostSession $a -JoinSession $b -ScenarioId 'w04-human-fixture' 2>&1 |
    ForEach-Object { "$_" }
$text = $output -join "`n"
$fragment = @(Get-ChildItem (Join-Path $projectPath 'work/netplay-recordings') -Filter 'w04-human-fixture.scenario.json' -ErrorAction SilentlyContinue)
Check 'a passing merge can emit a scenario fragment' ($fragment.Count -eq 1) $text
if ($fragment.Count -eq 1) {
    $parsed = Get-Content $fragment[0].FullName -Raw | ConvertFrom-Json
    Check 'the fragment is marked HUMAN, never SCRIPTED' ($parsed.coverage_source -eq 'HUMAN') "coverage_source = $($parsed.coverage_source)"
    Check 'the fragment names the commit it came from' ($parsed.build_commit -eq $commit) "build_commit = $($parsed.build_commit)"
    Remove-Item $fragment[0].FullName -Force
}

Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem (Join-Path $projectPath 'work/netplay-recordings') -Filter 'merged-*' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-5) } | Remove-Item -Force -ErrorAction SilentlyContinue

Write-Output ''
if ($failures.Count -gt 0) {
    Write-Output ("merge_session: FAIL ({0} of the cases above)" -f $failures.Count)
    exit 1
}
Write-Output 'merge_session: PASS'
exit 0
