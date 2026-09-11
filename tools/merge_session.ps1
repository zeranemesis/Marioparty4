# Merges the two halves of a two-machine session into one verdict.
#
# `record_board_session.ps1 -Role Host` and `-Role Join` each supervise their own
# process on their own machine, so neither can decide DETERMINISM: that question
# is "did the two machines compute the same thing", and it needs both machines'
# evidence in one place. Each side therefore reports `DETERMINISM: DEFERRED` and
# `OVERALL: PENDING_MERGE`, and this script is what resolves them.
#
# Copy both session folders onto one machine, then:
#
#   tools\merge_session.ps1 -HostSession <folder-from-A> -JoinSession <folder-from-B>
#
# The two folders are named by role rather than given as a list, so a merge can
# never quietly compare a folder against itself, and so the report can say which
# machine a problem is on.
#
# What it compares:
#
#   * the two input recordings, row by row over the prefix they share. Both peers
#     record BOTH seats, so every row either machine reached must be identical.
#     This is the strongest available evidence and does not depend on either
#     machine trusting the other's summary.
#   * the overlay paths, context by context, including the frame each transition
#     happened on. Two machines that agree on inputs but take different modules
#     have diverged somewhere the recording does not show.
#   * the last state hash each side reached, when both reached the same frame.
#   * the build commit. Two machines on different commits are not a session.
#
# What it does NOT do: it never repairs, reconciles or picks a winner. If the two
# sides disagree, that is the result, and the session is a FAIL with the frame of
# first divergence named.
#
# Exit codes: 0 merged PASS, 1 merged FAIL, 2 the inputs could not be read.
param(
    # The folder written by the machine that ran -Role Host.
    [Parameter(Mandatory = $true)][string]$HostSession,
    # The folder written by the machine that ran -Role Join.
    [Parameter(Mandatory = $true)][string]$JoinSession,
    # Where to write the merged recording when the session passes.
    [string]$Output = '',
    # Emit a scenarios.json fragment for this board, so a passing human session
    # becomes a permanent regression test instead of a story about an evening.
    [string]$ScenarioId = ''
)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
function Resolve-MergePath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}

$Session = @($HostSession, $JoinSession)
if ((Resolve-MergePath $HostSession) -eq (Resolve-MergePath $JoinSession)) {
    Write-Output "-HostSession and -JoinSession are the same folder. A folder compared against itself always agrees and proves nothing."
    exit 2
}

$halves = @()
foreach ($path in $Session) {
    $folder = Resolve-MergePath $path
    $recordPath = Join-Path $folder 'session.json'
    if (-not (Test-Path -LiteralPath $recordPath)) {
        Write-Output "No session.json in $folder. That folder was not produced by record_board_session.ps1, or the session did not finish."
        exit 2
    }
    $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
    $halves += @{ Folder = $folder; Record = $record }
}

$lines = New-Object Collections.Generic.List[string]
function Emit([string]$text) { $lines.Add($text); Write-Output $text }

$problems = New-Object Collections.Generic.List[string]

Emit 'PARTYBOARD_MERGED_SESSION version=1'
foreach ($half in $halves) {
    Emit ("  {0,-6} {1}" -f $half.Record.role, $half.Folder)
    Emit ("         commit {0}{1}, {2} seat(s), determinism {3}, stability {4}, playability {5}, overall {6}" -f
        $half.Record.build_commit, $(if ($half.Record.build_tree_dirty) { ' (DIRTY TREE)' } else { '' }),
        @($half.Record.seats).Count, $half.Record.determinism, $half.Record.stability,
        $half.Record.playability, $half.Record.overall)
}
Emit ''

# ---- 1. same build ----
# Checked first, because every comparison below is meaningless across builds: a
# determinism difference between two commits says nothing about either.
$commits = @($halves | ForEach-Object { $_.Record.build_commit } | Sort-Object -Unique)
if ($commits.Count -ne 1) {
    $problems.Add("The two machines ran different commits: $($commits -join ' vs '). These two halves are not one session.")
} else {
    Emit "build commit : $($commits[0])  (identical on both machines)"
}
foreach ($half in $halves) {
    if ($half.Record.build_tree_dirty) {
        $problems.Add("$($half.Record.role) ran with uncommitted changes, so its commit does not identify what it ran.")
    }
}

# ---- 2. both roles present ----
$roles = @($halves | ForEach-Object { $_.Record.role })
if ($roles -contains 'Local') {
    $problems.Add("A Local session already decided determinism on one machine; it has nothing to merge. Use -Role Host and -Role Join for a two-machine session.")
} elseif ($halves[0].Record.role -eq 'Host' -and $halves[1].Record.role -eq 'Join') {
    Emit "roles        : Host + Join"
} elseif ($halves[0].Record.role -eq 'Join' -and $halves[1].Record.role -eq 'Host') {
    $problems.Add("The folders were passed the wrong way round: -HostSession holds a Join session and -JoinSession holds a Host session. Swap them.")
} else {
    $problems.Add("Expected one Host folder and one Join folder, got: $($roles -join ', ').")
}

# ---- 3. the recordings ----
# The real test. Both peers record both seats, so the shared prefix must match
# exactly, and the first differing row names the frame where the two machines
# stopped computing the same game.
$recordings = @()
foreach ($half in $halves) {
    $seat = @($half.Record.seats)[0]
    $file = $seat.recording
    if (-not (Test-Path -LiteralPath $file)) {
        # A folder copied from another machine carries a path that no longer
        # resolves; fall back to the copy inside the folder itself.
        $file = Join-Path $half.Folder ("seat-{0}-recording.txt" -f $seat.side)
    }
    if (-not (Test-Path -LiteralPath $file)) {
        $file = Join-Path $half.Folder ("peer-{0}-input.txt" -f $seat.side)
    }
    if (-not (Test-Path -LiteralPath $file)) {
        $problems.Add("$($half.Record.role) has no readable input recording; determinism cannot be decided.")
        $recordings += , @()
        continue
    }
    $recordings += , @(Get-Content -LiteralPath $file)
}

$shared = 0
if ($recordings[0].Count -gt 0 -and $recordings[1].Count -gt 0) {
    $shared = [Math]::Min($recordings[0].Count, $recordings[1].Count)
    Emit ("recordings   : {0} and {1} rows, {2} shared (about {3} s of play)" -f
        $recordings[0].Count, $recordings[1].Count, $shared, [int]($shared / 120))
    $firstDifference = -1
    for ($i = 0; $i -lt $shared; ++$i) {
        if ($recordings[0][$i] -ne $recordings[1][$i]) { $firstDifference = $i; break }
    }
    if ($firstDifference -ge 0) {
        $problems.Add(("The two machines disagree from recorded row $firstDifference onwards " +
            "(about $([int]($firstDifference / 120)) s in). " +
            "$($halves[0].Record.role): '$($recordings[0][$firstDifference])' | " +
            "$($halves[1].Record.role): '$($recordings[1][$firstDifference])'"))
    } else {
        Emit "             all $shared shared rows identical"
    }
    if ($shared -lt 600) {
        $problems.Add("Session too short to be useful: only $shared shared rows (about $([int]($shared / 120)) s).")
    }
}

# ---- 4. the overlay paths ----
# The unary comma matters: ForEach-Object unrolls an array it emits, so the
# obvious spelling collapses two paths of three entries into one flat list of
# six, and the comparison then pits the first machine's first transition against
# the second machine's second one. Every session would have reported a spurious
# divergence. The fixture in tools\test_merge_session.ps1 is what caught it.
$paths = @()
foreach ($half in $halves) { $paths += , @(@($half.Record.seats)[0].overlay_path) }
if ($paths[0].Count -gt 0 -and $paths[1].Count -gt 0) {
    $difference = Compare-Object $paths[0] $paths[1] -SyncWindow 0
    if ($difference) {
        $problems.Add(("The two machines took different overlay paths. " +
            "$($halves[0].Record.role): $($paths[0] -join ' ') | " +
            "$($halves[1].Record.role): $($paths[1] -join ' ')"))
    } else {
        Emit ("overlay path : {0} transitions, frame-exact on both machines" -f $paths[0].Count)
        Emit ("             {0}" -f ($paths[0] -join ' -> '))
    }
} else {
    $problems.Add('At least one machine recorded no overlay transitions, so the paths cannot be compared.')
}

# ---- 5. the last state hash ----
# Only comparable when both sides stopped on the same frame; otherwise their
# hashes SHOULD differ and saying so would be a false alarm.
$seats = @($halves | ForEach-Object { @($_.Record.seats)[0] })
$hashes = @($seats | ForEach-Object { $_.last_state_hash })
if ($hashes[0] -and $hashes[1] -and $hashes[0] -ne 'unknown' -and $hashes[1] -ne 'unknown') {
    $frames = @($hashes | ForEach-Object { ($_ -split '@')[1] })
    if ($frames[0] -eq $frames[1]) {
        if ($hashes[0] -eq $hashes[1]) {
            Emit "state hash   : $($hashes[0]) - identical at the same frame"
        } else {
            $problems.Add("Both machines stopped at frame $($frames[0]) with different canonical hashes: $($hashes[0]) vs $($hashes[1]).")
        }
    } else {
        Emit ("state hash   : last frames differ ({0} vs {1}), so the hashes are not comparable and are not compared" -f $frames[0], $frames[1])
    }
}

# ---- 6. each side's own verdicts ----
foreach ($half in $halves) {
    if ($half.Record.determinism -eq 'FAIL') {
        $problems.Add("$($half.Record.role) already failed determinism on its own machine: $(@($half.Record.determinism_problems) -join '; ')")
    }
    if ($half.Record.stability -ne 'PASS') {
        $problems.Add("$($half.Record.role) failed stability; see its session folder.")
    }
    if ($half.Record.user_terminated -ne 'YES') {
        $problems.Add("$($half.Record.role) did not end on a user-requested exit, so the acceptance criterion was not met on that machine.")
    }
    if ($half.Record.playability -eq 'FAIL') {
        $problems.Add("$($half.Record.role) failed playability; the session was spent waiting for the other machine.")
    }
}

# ---- playability, both machines together ----
Emit ''
Emit '[PLAYABILITY]'
$worstRate = 0.0
$worstStall = 0
foreach ($half in $halves) {
    $seat = @($half.Record.seats)[0]
    Emit ("  {0,-6} {1} stalled ticks, {2}/min, longest wait {3} frames ({4} s)" -f
        $half.Record.role, $seat.stalled_ticks, $seat.stalls_per_minute, $seat.longest_stall_frames,
        [math]::Round([double]$seat.longest_stall_frames / 60.0, 2))
    if ($null -ne $seat.stalls_per_minute) { $worstRate = [Math]::Max($worstRate, [double]$seat.stalls_per_minute) }
    if ($null -ne $seat.longest_stall_frames) { $worstStall = [Math]::Max($worstStall, [int]$seat.longest_stall_frames) }
}
# Read from the recording rather than restated, so the two never drift apart.
$thresholds = $halves[0].Record.playability_thresholds
if ($worstRate -gt [double]$thresholds.max_stalls_per_minute -or $worstStall -gt [int]$thresholds.max_longest_stall_frames) {
    $playability = 'FAIL'
} elseif ($worstRate -gt [double]$thresholds.good_stalls_per_minute -or $worstStall -gt [int]$thresholds.good_longest_stall_frames) {
    $playability = 'MARGINAL'
} else {
    $playability = 'PASS'
}
Emit ("  worst of the two: {0}/min, {1} frames -> {2}" -f $worstRate, $worstStall, $playability)
Emit ("  thresholds (docs/playability_thresholds.md, fixed 2026-09-11): pass <= {0}/min and <= {1} frames; fail above {2}/min or {3} frames" -f
    $thresholds.good_stalls_per_minute, $thresholds.good_longest_stall_frames,
    $thresholds.max_stalls_per_minute, $thresholds.max_longest_stall_frames)

$determinism = if ($problems.Count -eq 0) { 'PASS' } else { 'FAIL' }
$overall = if ($determinism -eq 'PASS' -and $playability -ne 'FAIL') { 'PASS' } else { 'FAIL' }

Emit ''
foreach ($problem in $problems) { Emit "PROBLEM: $problem" }
Emit ''
Emit "MERGED DETERMINISM: $determinism"
Emit "MERGED PLAYABILITY: $playability"
Emit "MERGED OVERALL:     $overall"

# The merged recording is the artifact the whole exercise exists to produce.
if ($overall -eq 'PASS' -and $shared -ge 600) {
    $target = if ($Output) { Resolve-MergePath $Output } else {
        Resolve-MergePath ("work/netplay-recordings/merged-" + (Get-Date).ToString('yyyy-MM-dd_HHmmss') + ".txt")
    }
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    [IO.File]::WriteAllLines($target, $recordings[0][0..($shared - 1)])
    [IO.File]::WriteAllLines("$target.overlays", $paths[0])
    Emit ''
    Emit "Merged recording: $target"

    if ($ScenarioId) {
        # A scenario fragment, not an edit to the shipped manifest: adding it is
        # a deliberate act with a review, and a tool that writes into the
        # manifest by itself will eventually write something nobody chose.
        $fragment = [ordered]@{
            id = $ScenarioId
            description = "Human two-machine session, merged $((Get-Date).ToString('yyyy-MM-dd'))"
            kind = 'replay'
            replay = ($target -replace [regex]::Escape($projectPath + [IO.Path]::DirectorySeparatorChar), '') -replace '\\', '/'
            durationSeconds = [int]([Math]::Ceiling($shared / 120.0 * 1.4))
            minFrames = [int]($shared / 2)
            repeats = 1
            netplayDelay = 3
            seed = 'recorded'
            coverage_source = 'HUMAN'
            overlay_path = @($paths[0])
            merged_from = @($halves | ForEach-Object { Split-Path $_.Folder -Leaf })
            build_commit = $commits[0]
        }
        $fragmentPath = Join-Path (Split-Path $target -Parent) "$ScenarioId.scenario.json"
        [IO.File]::WriteAllText($fragmentPath,
            ($fragment | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding $false))
        Emit "Scenario fragment: $fragmentPath"
        Emit "  Review it, then add it to tests/scenarios/scenarios.json and record its hash with tools\verify_replays.ps1."
    }
} elseif ($shared -gt 0) {
    # A session that broke at 1 h 50 is still 1 h 50 of coverage, and the prefix
    # is exactly what reproduces the failure.
    $target = Resolve-MergePath ("work/netplay-recordings/merged-partial-" + (Get-Date).ToString('yyyy-MM-dd_HHmmss') + ".txt")
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    [IO.File]::WriteAllLines($target, $recordings[0][0..($shared - 1)])
    Emit ''
    Emit "Partial recording kept (the session failed, the prefix is still valid): $target"
}

$reportPath = Join-Path (Resolve-MergePath (Split-Path $HostSession -Parent)) `
    ("merged-session-" + (Get-Date).ToString('yyyy-MM-dd_HHmmss') + ".txt")
[IO.File]::WriteAllLines($reportPath, $lines)
Write-Output ''
Write-Output "Merge report: $reportPath"

if ($overall -ne 'PASS') { exit 1 }
exit 0
