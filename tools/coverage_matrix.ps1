# Builds the real coverage matrix from campaign results and session records.
#
# Nothing here is typed by hand. The list of minigames comes from the repository
# tables, and the state of each cell comes from run.json and session.json files
# on disk. A cell moves because a run moved it, never because someone remembered
# it moving.
#
# THE SIX STATES, and the rule that governs them:
#
#   UNTESTED           never reached by anything
#   SCRIPTED-PARTIAL   reached by generated input. This is the CEILING for
#                      scripted coverage: a thousand green scripted runs still
#                      do not show that a person can play normally until they
#                      choose to stop, which is the acceptance criterion. A
#                      scripted result is NEVER promoted to PASS.
#   HUMAN-PASS         a real person played it, on one machine, and it passed
#   REAL-NETWORK-PASS  two physical machines on a real network, and it passed
#   FAIL               reached and something went wrong. Outranks every green
#                      state: one failure is not erased by later successes.
#   BLOCKED            cannot be reached, with a reason
#
#   tools\coverage_matrix.ps1                 the summary
#   tools\coverage_matrix.ps1 -Detail         every minigame, one per line
#   tools\coverage_matrix.ps1 -Markdown docs/netplay_minigame_coverage.md
#
# Exit code is always 0: this reports, it does not judge.

param(
    [string]$CampaignRoot = 'work/netplay-campaigns',
    [string]$SessionRoot = 'work/netplay-recordings',
    # Sessions actually played between two physical machines, kept as the pair
    # of diagnostics both peers wrote. Until this was read, the only level of
    # proof that says anything about the network was invisible here.
    [string]$RealNetworkRoot = 'work/real-network',
    [switch]$Detail,
    [string]$Markdown = ''
)

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
function Resolve-MatrixPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}

# ---- the minigames, enumerated from the repository ----
# src/game/objsub.c holds mgInfoTbl, which gives every minigame's id and type.
# include/ovl_table.h gives the overlay index each one loads as. Neither is
# transcribed here; both are parsed.
$objsub = Get-Content (Resolve-MatrixPath 'src/game/objsub.c') -Raw
$ovlLines = Get-Content (Resolve-MatrixPath 'include/ovl_table.h')

# Overlay index -> module name, from the TARGET_PC branch only. The two branches
# of the #ifdef both contain the same module names, and counting through both
# shifts every index; checked against anchors the matrix already carries.
# Nesting has to be tracked, not assumed. The TARGET_PC block contains its own
# `#if VERSION_JP ... #else ... #endif`, and treating the first `#else` as the
# end of the PC block stops the parse at line 146 instead of 199 - which is
# exactly what happened, and what the anchor check below caught before a single
# wrong number was printed.
$overlayOf = @{}
$index = 0
$inPcBlock = $false
$depth = 0          # nesting inside the PC block
$skipping = $false  # inside a branch this build does not compile
foreach ($line in $ovlLines) {
    if (-not $inPcBlock) {
        if ($line -match '^#ifdef TARGET_PC') { $inPcBlock = $true }
        continue
    }
    if ($line -match '^#if') {
        $depth++
        # VERSION is 0 here, so the JP branch is the one NOT compiled.
        if ($line -match 'VERSION_JP') { $skipping = $true }
        continue
    }
    if ($line -match '^#else') {
        if ($depth -eq 0) { break }      # this closes the TARGET_PC block
        if ($skipping) { $skipping = $false } else { $skipping = $true }
        continue
    }
    if ($line -match '^#endif') {
        if ($depth -gt 0) { $depth--; $skipping = $false }
        continue
    }
    if ($skipping) { continue }
    if ($line -match '^DLL\((\w+)\)') {
        $overlayOf[$index] = $Matches[1]
        $index++
    }
}
# Anchors the validation matrix already states. If these ever stop holding, the
# parse above is wrong and every number below it is wrong with it.
$anchors = @{ 'instDll' = 3; 'resultDll' = 84; 'w01Dll' = 89; 'w04Dll' = 92 }
foreach ($name in $anchors.Keys) {
    $found = ($overlayOf.GetEnumerator() | Where-Object { $_.Value -eq $name } | Select-Object -First 1).Key
    if ($found -ne $anchors[$name]) {
        Write-Output "REFUSING TO REPORT: $name parsed as overlay $found, the matrix says $($anchors[$name])."
        Write-Output 'include/ovl_table.h is not being read the way the engine numbers it.'
        exit 2
    }
}

# The validation matrix is the single place where the overlay / board-id / name
# pairing is written down. It is parsed, not duplicated.
$matrixPath = Resolve-MatrixPath 'docs/netplay_validation_matrix.md'
$matrixLines = if (Test-Path -LiteralPath $matrixPath) { Get-Content $matrixPath } else { @() }

# Boards are wNNDll. Their GWSystem.board id is NOT their overlay index, which
# is why the pairing has to be read rather than computed.
$boards = @{}
foreach ($pair in $overlayOf.GetEnumerator()) {
    if ($pair.Value -match '^w(\d{2})[Dd]ll$') {
        $boards[$pair.Key] = @{ Overlay = $pair.Key; Module = $pair.Value; Name = '(non nomme)'; BoardId = -1 }
    }
}
foreach ($line in $matrixLines) {
    if ($line -match '^\|\s*(\d{2})\s*\|\s*`(w\d{2}dll)`\s*\|\s*(\d+)\s*\|\s*([^|]+?)\s*\|') {
        $overlay = [int]$Matches[1]
        if ($boards.ContainsKey($overlay)) {
            $boards[$overlay].BoardId = [int]$Matches[3]
            $boards[$overlay].Name = $Matches[4].Trim()
        }
    }
}

# Minigame modules are mNNNDll / mNNNdll; the number is the minigame id.
$minigames = @{}
foreach ($pair in $overlayOf.GetEnumerator()) {
    if ($pair.Value -match '^m(\d{3})[Dd]ll$') {
        $id = [int]$Matches[1]
        if ($id -lt 400) { continue }   # m3xx are not board minigames
        $minigames[$pair.Key] = @{ Overlay = $pair.Key; Module = $pair.Value; Id = $id }
    }
}

# Minigame names, from the same table. A missing name is reported as missing,
# never invented.
$names = @{}
foreach ($line in $matrixLines) {
    if ($line -match '^\|\s*(\d{3})\s*\|\s*(\d+)\s*\|\s*`(m\d{3}[Dd]ll)`\s*\|\s*([^|]+?)\s*\|') {
        $names[[int]$Matches[1]] = $Matches[4].Trim()
    }
}

# ---- what the results say ----
$state = @{}
foreach ($key in $minigames.Keys) { $state[$key] = @{ State = 'UNTESTED'; Why = ''; Runs = 0 } }
$boardState = @{}
foreach ($key in $boards.Keys) { $boardState[$key] = @{ State = 'UNTESTED'; Why = ''; Runs = 0; Depth = -1 } }
# REAL-NETWORK-PARTIAL is not in the original vocabulary, and it is here because
# the original vocabulary could not describe what happened on 2026-09-12: a board
# played for four and a half minutes between two houses' worth of hardware, which
# is far more than any script can claim, and which nevertheless did not finish.
# Calling that REAL-NETWORK-PASS would overstate it and UNTESTED would erase it.
# It answers the fourth question asked of this matrix - how many were really
# played across two machines, as opposed to validated there.
$rank = @{ 'UNTESTED' = 0; 'SCRIPTED-PARTIAL' = 1; 'HUMAN-PASS' = 2;
           'REAL-NETWORK-PARTIAL' = 3; 'REAL-NETWORK-PASS' = 4; 'BLOCKED' = 5; 'FAIL' = 6 }

function PromoteBoard([int]$overlay, [string]$candidate, [string]$why, [int]$depth) {
    if (-not $boardState.ContainsKey($overlay)) { return }
    $cell = $boardState[$overlay]
    if ($rank[$candidate] -gt $rank[$cell.State]) {
        $cell.State = $candidate
        $cell.Why = $why
        $cell.Depth = $depth
    } elseif ($rank[$candidate] -eq $rank[$cell.State] -and $depth -gt $cell.Depth) {
        # Same verdict, better evidence. "Reached the board" and "played five
        # turns on it" are both SCRIPTED-PARTIAL, and reporting whichever ran
        # first would throw away the one that says more.
        $cell.Why = $why
        $cell.Depth = $depth
    }
    $cell.Runs++
}

function Promote([int]$overlay, [string]$candidate, [string]$why) {
    if (-not $state.ContainsKey($overlay)) { return }
    # FAIL outranks everything: one failure is not erased by later successes.
    $current = $state[$overlay].State
    if ($rank[$candidate] -gt $rank[$current]) {
        $state[$overlay].State = $candidate
        $state[$overlay].Why = $why
    }
    $state[$overlay].Runs++
}

$runFiles = @(Get-ChildItem (Resolve-MatrixPath $CampaignRoot) -Recurse -Filter 'run.json' -ErrorAction SilentlyContinue)
foreach ($file in $runFiles) {
    $run = Get-Content $file.FullName -Raw | ConvertFrom-Json
    # A retracted verdict is a measurement known to be wrong, kept on disk as
    # evidence of the harness defect that produced it. It stays readable and it
    # stops counting - deleting it would hide a failure, and trusting it would
    # repeat a falsehood.
    if ($run.PSObject.Properties.Name -contains 'verdict_retracted') { continue }
    $played = @()
    if ($run.PSObject.Properties.Name -contains 'minigames_played') { $played = @($run.minigames_played) }
    if ($played.Count -eq 0) { continue }
    $source = if ($run.PSObject.Properties.Name -contains 'coverage_source') { [string]$run.coverage_source } else { 'RECORDED' }
    foreach ($overlay in $played) {
        $id = [int]$overlay
        # FAIL is for something going wrong WHERE THE MINIGAME IS, not for the
        # run failing elsewhere. A monkey that entered a minigame cleanly and
        # then stalled on the board two minutes later has told us something true
        # about that minigame - it was reached and it did not break - and
        # condemning it would both lose that and cry wolf.
        #
        # A crash or a desync still marks FAIL, because attributing them is hard
        # and hiding them would be worse. TIMEOUT and ABNORMAL_EXIT are progress
        # verdicts about the run, not about the minigame.
        if ($run.result -eq 'CRASH' -or $run.result -eq 'DESYNC') {
            Promote $id 'FAIL' "$($run.result) in $($run.scenario) run $($run.run_index)"
        } elseif ($run.result -eq 'HARNESS_FAILURE') {
            # The harness could not deliver the scenario, so the run says nothing
            # about anything. Credit nothing.
            continue
        } else {
            # RECORDED replays a human session; it is still not a human session,
            # so it is capped exactly where SCRIPTED is.
            Promote $id 'SCRIPTED-PARTIAL' "$source, $($run.scenario)"
        }
    }
}

# Boards, from the same runs. The overlay path names which board a run entered,
# and board_coverage says which of its mechanics fired - a board reached with
# two mechanics is not the same claim as a board played through, so the count
# travels with the verdict.
foreach ($file in $runFiles) {
    $run = Get-Content $file.FullName -Raw | ConvertFrom-Json
    if ($run.PSObject.Properties.Name -contains 'verdict_retracted') { continue }
    if ($run.result -eq 'HARNESS_FAILURE') { continue }
    $path = @()
    if ($run.PSObject.Properties.Name -contains 'overlays') { $path = @($run.overlays) }
    $source = if ($run.PSObject.Properties.Name -contains 'coverage_source') { [string]$run.coverage_source } else { 'RECORDED' }
    $mechanics = 0
    if ($run.PSObject.Properties.Name -contains 'board_coverage') { $mechanics = @($run.board_coverage).Count }
    $turn = if ($run.PSObject.Properties.Name -contains 'board_turn') { [int]$run.board_turn } else { -1 }
    foreach ($entryText in $path) {
        $id = [int](($entryText -split '@')[0])
        if (-not $boardState.ContainsKey($id)) { continue }
        # Depth: how much the run actually did on the board. Turns dominate,
        # mechanics break the tie - a run on turn 5 says more than one on turn 1
        # whatever else it touched.
        $depth = ($turn * 100) + $mechanics
        if ($run.result -eq 'CRASH' -or $run.result -eq 'DESYNC') {
            PromoteBoard $id 'FAIL' "$($run.result) in $($run.scenario)" $depth
        } else {
            PromoteBoard $id 'SCRIPTED-PARTIAL' "$source, $mechanics mecaniques, tour $turn" $depth
        }
    }
}

$sessionFiles = @(Get-ChildItem (Resolve-MatrixPath $SessionRoot) -Recurse -Filter 'session.json' -ErrorAction SilentlyContinue)
foreach ($file in $sessionFiles) {
    $session = Get-Content $file.FullName -Raw | ConvertFrom-Json
    if ($session.PSObject.Properties.Name -contains 'rehearsal' -and $session.rehearsal) { continue }
    $overall = [string]$session.overall
    foreach ($seat in @($session.seats)) {
        foreach ($entryText in @($seat.overlay_path)) {
            $id = [int](($entryText -split '@')[0])
            if (-not $state.ContainsKey($id)) { continue }
            if ($overall -eq 'FAIL') { Promote $id 'FAIL' 'human session failed'; continue }
            if ($session.role -eq 'Local') { Promote $id 'HUMAN-PASS' 'human session, one machine' }
            else { Promote $id 'REAL-NETWORK-PASS' 'human session, two machines' }
        }
    }
}

# ---- sessions played through the lobby, between two machines ----
# These leave no session.json: record_board_session.ps1 was never in the loop,
# the players used the lobby. What they do leave is each peer's native.txt, and
# that carries the overlay path and the desync - which is what a cell needs.
#
# They are credited REAL-NETWORK-PARTIAL and never REAL-NETWORK-PASS, however
# well they went. PASS requires the verdict machinery in session.json, which
# knows whether the session ended because the players chose to stop; a raw
# native.txt only shows a window closing, and a window closes on delight and on
# disgust alike.
$realRoots = @(Get-ChildItem (Resolve-MatrixPath $RealNetworkRoot) -Directory -ErrorAction SilentlyContinue)
foreach ($root in $realRoots) {
    $natives = @(Get-ChildItem $root.FullName -Recurse -Filter 'native.txt' -ErrorAction SilentlyContinue)
    if ($natives.Count -eq 0) { continue }
    $visited = New-Object 'Collections.Generic.HashSet[int]'
    $failedAt = -1
    $lastFrame = 0
    foreach ($native in $natives) {
        foreach ($line in [IO.File]::ReadLines($native.FullName)) {
            if ($line -match 'event=DESYNC .*?context=(-?\d+)') { $failedAt = [int]$Matches[1] }
            if ($line -match ' context=(-?\d+)') { [void]$visited.Add([int]$Matches[1]) }
            if ($line -match ' frame=(\d+)') {
                $f = [int]$Matches[1]
                if ($f -gt $lastFrame) { $lastFrame = $f }
            }
        }
    }
    $seconds = [math]::Round($lastFrame / 60.0)
    foreach ($id in $visited) {
        if ($id -lt 0) { continue }
        if ($id -eq $failedAt) {
            $why = "DESYNC en session reelle $($root.Name)"
            Promote $id 'FAIL' $why
            PromoteBoard $id 'FAIL' $why $lastFrame
        } else {
            $why = "session reelle $($root.Name), $lastFrame frames ($seconds s)"
            Promote $id 'REAL-NETWORK-PARTIAL' $why
            PromoteBoard $id 'REAL-NETWORK-PARTIAL' $why $lastFrame
        }
    }
}

# ---- report ----
$lines = New-Object Collections.Generic.List[string]
function Emit([string]$text) { $lines.Add($text); Write-Output $text }

$total = $minigames.Count
$counts = @{}
foreach ($name in 'UNTESTED', 'SCRIPTED-PARTIAL', 'HUMAN-PASS', 'REAL-NETWORK-PARTIAL', 'REAL-NETWORK-PASS', 'FAIL', 'BLOCKED') {
    $counts[$name] = @($state.Values | Where-Object { $_.State -eq $name }).Count
}

Emit "# Couverture des mini-jeux"
Emit ''
Emit ("Etablie le {0} depuis {1} resultats de campagne et {2} sessions." -f
    (Get-Date).ToString('yyyy-MM-dd HH:mm'), $runFiles.Count, $sessionFiles.Count)
Emit ''
Emit ("**{0} mini-jeux** enumeres depuis \`include/ovl_table.h\`." -f $total)
Emit ''
Emit '| etat | nombre | sur |'
Emit '|---|---|---|'
foreach ($name in 'UNTESTED', 'SCRIPTED-PARTIAL', 'HUMAN-PASS', 'REAL-NETWORK-PARTIAL', 'REAL-NETWORK-PASS', 'FAIL', 'BLOCKED') {
    Emit ('| `{0}` | **{1}** | {2} |' -f $name, $counts[$name], $total)
}
Emit ''
Emit ('- jamais atteints : **{0} sur {1}**' -f $counts['UNTESTED'], $total)
Emit ('- exerces automatiquement : **{0}**' -f $counts['SCRIPTED-PARTIAL'])
Emit ('- reellement joues par un humain : **{0}**' -f $counts['HUMAN-PASS'])
Emit ('- joues entre deux machines sans aller au bout : **{0}**' -f $counts['REAL-NETWORK-PARTIAL'])
Emit ('- valides entre deux machines : **{0}**' -f $counts['REAL-NETWORK-PASS'])
Emit ''
Emit 'Un resultat `SCRIPTED` plafonne a `SCRIPTED-PARTIAL`. Il n''est jamais promu'
Emit 'en `PASS`, quel que soit le nombre de runs verts : le volume de succes'
Emit 'automatiques ne remplace pas la nature de la preuve.'

Emit ''
Emit '## Plateaux'
Emit ''
Emit '| overlay | module | board | nom | etat | pourquoi |'
Emit '|---|---|---|---|---|---|'
foreach ($key in ($boards.Keys | Sort-Object)) {
    $b = $boards[$key]
    Emit ('| {0} | `{1}` | {2} | {3} | `{4}` | {5} |' -f
        $b.Overlay, $b.Module, $b.BoardId, $b.Name, $boardState[$key].State, $boardState[$key].Why)
}
$boardCounts = @{}
foreach ($name in 'UNTESTED', 'SCRIPTED-PARTIAL', 'HUMAN-PASS', 'REAL-NETWORK-PARTIAL', 'REAL-NETWORK-PASS', 'FAIL', 'BLOCKED') {
    $boardCounts[$name] = @($boardState.Values | Where-Object { $_.State -eq $name }).Count
}
Emit ''
Emit ('{0} plateaux : {1} jamais atteints, {2} exerces automatiquement, {3} joues par un humain, {4} joues entre deux machines sans aller au bout, {5} valides entre deux machines.' -f
    $boards.Count, $boardCounts['UNTESTED'], $boardCounts['SCRIPTED-PARTIAL'],
    $boardCounts['HUMAN-PASS'], $boardCounts['REAL-NETWORK-PARTIAL'], $boardCounts['REAL-NETWORK-PASS'])

if ($Detail -or $Markdown) {
    Emit ''
    Emit '## Mini-jeux, un par ligne'
    Emit ''
    Emit '| overlay | module | id | nom | etat | pourquoi |'
    Emit '|---|---|---|---|---|---|'
    foreach ($key in ($minigames.Keys | Sort-Object)) {
        $info = $minigames[$key]
        $name = if ($names.ContainsKey($info.Id)) { $names[$info.Id] } else { '(non nomme dans la matrice)' }
        Emit ('| {0} | `{1}` | {2} | {3} | `{4}` | {5} |' -f
            $info.Overlay, $info.Module, $info.Id, $name, $state[$key].State, $state[$key].Why)
    }
}

if ($Markdown) {
    $target = Resolve-MatrixPath $Markdown
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    [IO.File]::WriteAllLines($target, $lines)
    Write-Output ''
    Write-Output "ecrit dans $target"
}
exit 0
