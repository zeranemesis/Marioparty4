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
    $dir = Join-Path $root "$Target-$RayTracing-$run"
    Write-Host "=== run $run / $Runs (plateau +$boardIndex) ==="
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test_raytracing.ps1') `
        -Target $Target -RayTracing $RayTracing -OutputDirectory $dir -Attempts 2 -BoardIndex $boardIndex 2>&1
    $text = $out -join "`n"
    $scene = if ($text -match 'reached (\S+) on attempt') { $Matches[1] } else { 'none' }
    # Echo what the child said about walking the carousel: without this the run
    # looks identical whether the board moved or not.
    $out | Select-String 'plateau :' | ForEach-Object { Write-Host "  $_" }
    if ($scene -eq 'none') {
        Write-Host "  aucune scene atteinte"
        $out | Select-Object -Last 4 | ForEach-Object { Write-Host "    $_" }
        continue
    }
    $row = [ordered]@{ scene = $scene }
    if ($text -match '(\d+) triangles, BLAS (\d+) KB, build (?:reused, )?([0-9.]+) ms, (\d+x\d+) ([0-9.]+) ms GPU') {
        $row.triangles = [int]$Matches[1]; $row.buildMs = [double]$Matches[3]
        $row.res = $Matches[4]; $row.traceMs = [double]$Matches[5]
    }
    if ($text -match 'Orthographic coverage: ([0-9.]+)% .*?(\d+) full-screen 2D draws, (\d+) bounded') {
        $row.mask = [double]$Matches[1]; $row.fullscreen2D = [int]$Matches[2]; $row.bounded2D = [int]$Matches[3]
    }
    if ($text -match 'Perspective projections this frame: (\d+)') { $row.projections = [int]$Matches[1] }
    if ($text -match 'Of (\d+) captured draws: (\d+) cut-out.*?(\d+) environment mapped') {
        $row.draws = [int]$Matches[1]
        $row.cutoutPct = [math]::Round(100.0 * [int]$Matches[2] / [int]$Matches[1], 1)
        $row.envMapPct = [math]::Round(100.0 * [int]$Matches[3] / [int]$Matches[1], 1)
    }
    if ($text -match 'Scene extent (\d+) x (\d+) x (\d+).*?shadow range (\d+)') {
        $row.extent = "$($Matches[1])x$($Matches[2])x$($Matches[3])"; $row.shadowRange = [int]$Matches[4]
    }
    $row.errors = if ($text -match 'PASS:') { 0 } else { 1 }
    $rows += [pscustomobject]$row
    Write-Host "  $scene : trace $($row.traceMs) ms, masque $($row.mask)%, decoupe $($row.cutoutPct)%, envmap $($row.envMapPct)%"
}

Write-Host ''
Write-Host '===== recapitulatif ====='
$rows | Format-Table -AutoSize scene, triangles, traceMs, buildMs, mask, bounded2D, projections, cutoutPct, envMapPct, shadowRange, errors
$rows | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $root "summary-$Target-$RayTracing.json") -Encoding utf8
Write-Host "scenes distinctes : $(($rows.scene | Sort-Object -Unique) -join ', ')"
