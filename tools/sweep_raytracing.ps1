<#
.SYNOPSIS
Runs test_raytracing.ps1 repeatedly and aggregates what each scene reported.

.DESCRIPTION
Which board or mini-game the menu sequence lands on depends on timing, so
breadth comes from repetition rather than from targeting. Each run gets its own
output directory; the summary at the end is the point.
#>
param(
    [ValidateSet('board', 'minigame')][string]$Target = 'board',
    [ValidateSet('on', 'off')][string]$RayTracing = 'on',
    [int]$Runs = 6,
    # A comma separated list, not [int[]]: powershell.exe -File passes arguments
    # as literal strings, and "0,1,2,3,4,5" binds to an int array as the single
    # value 12345 without complaining. That silently gave every run the same
    # board, which is exactly the failure this sweep exists to avoid.
    [string]$BoardIndices = '0',
    # Same idea for mini-games: how far down the list each run goes.
    [string]$MinigameIndices = '0',
    # And which category of the list each run takes first, walked in step with
    # the indices -- run n uses the n-th entry of both. The list stops at the
    # bottom of a category, so the indices alone never leave the first one.
    [string]$MinigameCategories = '0',
    [string]$OutputDirectory = 'work/raytracing-sweep'
)
$ErrorActionPreference = 'Continue'
$projectPath = Split-Path $PSScriptRoot -Parent
$root = Join-Path $projectPath $OutputDirectory
New-Item -ItemType Directory -Force -Path $root | Out-Null

$rows = @()
foreach ($run in 1..$Runs) {
    $indices = @($BoardIndices -split ',' | ForEach-Object { [int]$_.Trim() })
    $boardIndex = $indices[($run - 1) % $indices.Count]
    $mgIndices = @($MinigameIndices -split ',' | ForEach-Object { [int]$_.Trim() })
    $mgIndex = $mgIndices[($run - 1) % $mgIndices.Count]
    $mgCategories = @($MinigameCategories -split ',' | ForEach-Object { [int]$_.Trim() })
    $mgCategory = $mgCategories[($run - 1) % $mgCategories.Count]
    $dir = Join-Path $root "$Target-$RayTracing-$run"
    Write-Host "=== run $run / $Runs (plateau +$boardIndex, mini-jeu categorie $mgCategory +$mgIndex) ==="
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test_raytracing.ps1') `
        -Target $Target -RayTracing $RayTracing -OutputDirectory $dir -Attempts 2 -BoardIndex $boardIndex -MinigameIndex $mgIndex -MinigameCategory $mgCategory 2>&1
    $text = $out -join "`n"
    $scene = if ($text -match 'reached (\S+) on attempt') { $Matches[1] } else { 'none' }
    # Echo what the child said about walking the carousel: without this the run
    # looks identical whether the board moved or not.
    $out | Select-String 'plateau :|mini-jeu :' | ForEach-Object { Write-Host "  $_" }
    if ($scene -eq 'none') {
        Write-Host "  aucune scene atteinte"
        $out | Select-Object -Last 4 | ForEach-Object { Write-Host "    $_" }
        continue
    }
    $row = [ordered]@{ scene = $scene; category = $mgCategory; index = $mgIndex }
    if ($text -match '(\d+) triangles, BLAS (\d+) KB, build (?:reused, )?([0-9.]+) ms, (\d+x\d+) ([0-9.]+) ms GPU') {
        $row.triangles = [int]$Matches[1]; $row.buildMs = [double]$Matches[3]
        $row.res = $Matches[4]; $row.traceMs = [double]$Matches[5]
    }
    # The 2D mask is gone; what matters now is that the bounds stay finite and
    # the composition runs once a frame. Both are judged in the child script.
    if ($text -match 'bornes de scene : (\d+) non finies et (\d+) absurdes sur (\d+) rapports') {
        $row.badBounds = [int]$Matches[1] + [int]$Matches[2]; $row.boundReports = [int]$Matches[3]
    }
    if ($text -match 'composition : au plus (\d+) par rapport de 300 frames, (\d+) passes') {
        $row.compositeMax = [int]$Matches[1]; $row.compositeExtra = [int]$Matches[2]
    }
    if ($text -match 'Positions rejected as non-finite or out of range: (\d+)') { $row.rejectedTris = [int]$Matches[1] }
    if ($text -match 'Perspective projections this frame: (\d+)') { $row.projections = [int]$Matches[1] }
    if ($text -match 'Of (\d+) captured draws: (\d+) cut-out.*?(\d+) environment mapped') {
        $row.draws = [int]$Matches[1]
        $row.cutoutPct = [math]::Round(100.0 * [int]$Matches[2] / [int]$Matches[1], 1)
        $row.envMapPct = [math]::Round(100.0 * [int]$Matches[3] / [int]$Matches[1], 1)
    }
    # (\S+), not (\d+): the integer form failed silently on "-nan" and left
    # the column blank, which read as "no light" rather than as a defect.
    if ($text -match 'Scene extent (\S+) x (\S+) x (\S+);.*?shadow range (\S+)') {
        $row.extent = "$($Matches[1])x$($Matches[2])x$($Matches[3])"; $row.shadowRange = $Matches[4]
    }
    $row.errors = if ($text -match 'PASS:') { 0 } else { 1 }
    $rows += [pscustomobject]$row
    Write-Host "  $scene : trace $($row.traceMs) ms, bornes invalides $($row.badBounds)/$($row.boundReports), composition max $($row.compositeMax), decoupe $($row.cutoutPct)%"
}

Write-Host ''
Write-Host '===== recapitulatif ====='
$rows | Format-Table -AutoSize scene, category, index, triangles, traceMs, buildMs, badBounds, rejectedTris, compositeMax, projections, cutoutPct, envMapPct, shadowRange, errors
$rows | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $root "summary-$Target-$RayTracing.json") -Encoding utf8
Write-Host "scenes distinctes : $(($rows.scene | Sort-Object -Unique) -join ', ')"
