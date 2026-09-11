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

# Minigame modules are mNNNDll / mNNNdll; the number is the minigame id.
$minigames = @{}
foreach ($pair in $overlayOf.GetEnumerator()) {
    if ($pair.Value -match '^m(\d{3})[Dd]ll$') {
        $id = [int]$Matches[1]
        if ($id -lt 400) { continue }   # m3xx are not board minigames
        $minigames[$pair.Key] = @{ Overlay = $pair.Key; Module = $pair.Value; Id = $id }
    }
}

# Names, from the validation matrix table which was itself built from
# selmenuDll/main.c. Optional: a missing name is reported as such, never
# invented.
$names = @{}
$matrixPath = Resolve-MatrixPath 'docs/netplay_validation_matrix.md'
if (Test-Path -LiteralPath $matrixPath) {
    foreach ($line in Get-Content $matrixPath) {
        if ($line -match '^\|\s*(\d{3})\s*\|\s*(\d+)\s*\|\s*`(m\d{3}[Dd]ll)`\s*\|\s*([^|]+?)\s*\|') {
            $names[[int]$Matches[1]] = $Matches[4].Trim()
        }
    }
}

# ---- what the results say ----
$state = @{}
foreach ($key in $minigames.Keys) { $state[$key] = @{ State = 'UNTESTED'; Why = ''; Runs = 0 } }

function Promote([int]$overlay, [string]$candidate, [string]$why) {
    if (-not $state.ContainsKey($overlay)) { return }
    # FAIL outranks everything: one failure is not erased by later successes.
    $rank = @{ 'UNTESTED' = 0; 'SCRIPTED-PARTIAL' = 1; 'HUMAN-PASS' = 2; 'REAL-NETWORK-PASS' = 3; 'BLOCKED' = 4; 'FAIL' = 5 }
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

# ---- report ----
$lines = New-Object Collections.Generic.List[string]
function Emit([string]$text) { $lines.Add($text); Write-Output $text }

$total = $minigames.Count
$counts = @{}
foreach ($name in 'UNTESTED', 'SCRIPTED-PARTIAL', 'HUMAN-PASS', 'REAL-NETWORK-PASS', 'FAIL', 'BLOCKED') {
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
foreach ($name in 'UNTESTED', 'SCRIPTED-PARTIAL', 'HUMAN-PASS', 'REAL-NETWORK-PASS', 'FAIL', 'BLOCKED') {
    Emit ('| `{0}` | **{1}** | {2} |' -f $name, $counts[$name], $total)
}
Emit ''
Emit ('- jamais atteints : **{0} sur {1}**' -f $counts['UNTESTED'], $total)
Emit ('- exerces automatiquement : **{0}**' -f $counts['SCRIPTED-PARTIAL'])
Emit ('- reellement joues par un humain : **{0}**' -f $counts['HUMAN-PASS'])
Emit ('- valides entre deux machines : **{0}**' -f $counts['REAL-NETWORK-PASS'])
Emit ''
Emit 'Un resultat `SCRIPTED` plafonne a `SCRIPTED-PARTIAL`. Il n''est jamais promu'
Emit 'en `PASS`, quel que soit le nombre de runs verts : le volume de succes'
Emit 'automatiques ne remplace pas la nature de la preuve.'

if ($Detail -or $Markdown) {
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
