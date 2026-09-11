# Checks that the stored submodule patches still describe the submodules.
#
# Why this exists, and what it caught. PartyBoard keeps its changes to MusyX and
# Aurora as working-tree modifications in the submodules, and CI does not have
# those: it checks out the recorded submodule commits and applies
# patches/*.patch on top. The patch file IS the shipped version of that work.
#
# On 2026-09-11 the two had been out of step for a day. The whole D3 fix - the
# barrier in salDetachVoicesFromSample that stops a bank being freed under a
# live voice, plus every audio lifetime detector call in MusyX - existed only in
# the local submodule. patches/musyx-partyboard.patch predated it. A release
# built by CI would have applied the old patch, passed every self-test (they
# test the detector module in src/port/, not its integration into MusyX), and
# shipped the use-after-free that an entire session had been spent proving and
# fixing.
#
# Nothing would have said so. So this says so.
#
#   tools\test_submodule_patches.ps1            check
#   tools\test_submodule_patches.ps1 -Update    rewrite the patches from the
#                                               submodules, deliberately
#
# Exit codes: 0 the patches match, 1 they do not, 2 something is missing.

param(
    [switch]$Update
)

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent

# Submodule directory -> patch file. Mirrors the "Apply PartyBoard dependency
# patches" step of .github/workflows/cubeshelf-windows.yml.
$pairs = @(
    @{ Submodule = 'extern/musyx';  Patch = 'patches/musyx-partyboard.patch' },
    @{ Submodule = 'extern/aurora'; Patch = 'patches/aurora-partyboard.patch' }
)

$failures = New-Object Collections.Generic.List[string]
function Say([string]$text) { Write-Output $text }

foreach ($pair in $pairs) {
    $submodule = Join-Path $projectPath $pair.Submodule
    $patch = Join-Path $projectPath $pair.Patch
    Say ("--- {0}" -f $pair.Submodule)

    if (-not (Test-Path -LiteralPath $submodule)) {
        Say "  MISSING submodule; run git submodule update --init --recursive"
        $failures.Add($pair.Submodule); continue
    }
    if (-not (Test-Path -LiteralPath $patch)) {
        Say ("  MISSING patch: {0}" -f $pair.Patch)
        $failures.Add($pair.Patch); continue
    }

    # The submodule commit CI will check out. A patch generated against a
    # different base will not apply there, however well it applies here.
    $recorded = (& git -C $projectPath ls-tree HEAD $pair.Submodule) -split '\s+' | Select-Object -Index 2
    $actual = (& git -C $submodule rev-parse HEAD).Trim()
    if ($recorded -and $actual -and $recorded -ne $actual) {
        Say ("  FAIL  the submodule is at {0} but the superproject records {1};" -f $actual.Substring(0,12), $recorded.Substring(0,12))
        Say      "        a patch generated here would not apply to what CI checks out"
        $failures.Add($pair.Submodule + ' base commit')
        continue
    }
    Say ("  base commit {0}, matching the superproject" -f $actual.Substring(0, 12))

    $live = (& git -C $submodule -c core.safecrlf=false diff) -join "`n"
    $stored = (Get-Content -LiteralPath $patch -Raw)

    # Compare content, not line endings: git's autocrlf rewrites the file on
    # checkout and that is not a difference in the change being described.
    $liveNormal = ($live -replace "`r`n", "`n").TrimEnd("`n")
    $storedNormal = ($stored -replace "`r`n", "`n").TrimEnd("`n")

    if ($liveNormal -eq $storedNormal) {
        $files = @(& git -C $submodule -c core.safecrlf=false diff --name-only).Count
        Say ("  ok    the patch describes the submodule exactly ({0} files, {1:n0} bytes)" -f $files, $storedNormal.Length)
        continue
    }

    if ($Update) {
        [IO.File]::WriteAllText($patch, $liveNormal + "`n", (New-Object Text.UTF8Encoding $false))
        Say ("  UPDATED {0} from the submodule" -f $pair.Patch)
        continue
    }

    Say ("  FAIL  the patch and the submodule disagree ({0:n0} vs {1:n0} bytes)" -f $storedNormal.Length, $liveNormal.Length)
    Say      "        CI applies the patch, not your working tree. Whatever is missing from"
    Say      "        the patch is missing from every build CI produces."
    # Naming the files is what turns this from a puzzle into a two-minute fix.
    $liveFiles = @(& git -C $submodule -c core.safecrlf=false diff --name-only)
    $storedFiles = @([regex]::Matches($storedNormal, '(?m)^\+\+\+ b/(.+)$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    $onlyLive = @($liveFiles | Where-Object { $storedFiles -notcontains $_ })
    $onlyStored = @($storedFiles | Where-Object { $liveFiles -notcontains $_ })
    foreach ($f in $onlyLive) { Say ("        in the submodule but NOT in the patch: {0}" -f $f) }
    foreach ($f in $onlyStored) { Say ("        in the patch but NOT in the submodule: {0}" -f $f) }
    if ($onlyLive.Count -eq 0 -and $onlyStored.Count -eq 0) {
        Say "        the same files on both sides, so the difference is inside one of them"
    }
    Say      "        Re-run with -Update once you have checked that is what you mean."
    $failures.Add($pair.Patch)
}

Write-Output ''
if ($failures.Count -gt 0) {
    Write-Output ('submodule patches: FAIL ({0})' -f ($failures -join ', '))
    exit 1
}
Write-Output 'submodule patches: PASS'
exit 0
