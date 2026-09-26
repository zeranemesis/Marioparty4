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

# Submodule directory -> the patches applied to it. Mirrors the "Apply
# PartyBoard dependency patches" step of .github/workflows/cubeshelf-windows.yml
# and of build.yml, in the order those apply them.
#
# A submodule can carry more than one patch. Aurora carries the PartyBoard
# integration work in one, and renderer fixes that are candidates for upstream
# in another, so the second can be sent to encounter/aurora without dragging the
# first along. Patches for one submodule must touch disjoint files; two patches
# editing the same file are reported below rather than quietly merged.
$pairs = @(
    @{ Submodule = 'extern/musyx';  Patches = @('patches/musyx-partyboard.patch') },
    @{ Submodule = 'extern/aurora'; Patches = @('patches/aurora-partyboard.patch',
                                                'patches/aurora-render-fixes.patch',
                                                'patches/aurora-android-surface-deadlock.patch',
                                                'patches/aurora-mobile-one-local-player.patch') }
)

$failures = New-Object Collections.Generic.List[string]
function Say([string]$text) { Write-Output $text }

# The change a submodule carries, including files it does not track yet.
#
# `git diff` alone only reports tracked files, and that is how this guard was
# blind to the one drift it could not afford to miss: the MusyX patch creates
# src/musyx/runtime/synth_wait.h, synthmacros.c includes it, and regenerating
# the patch from a working tree where that file was untracked dropped its
# creation while keeping the include. The patch still applied, so nothing here
# complained, and every CI build failed to compile.
#
# Staging into a throwaway index keeps the submodule's own index untouched and
# still honours .gitignore, so build output stays out of the comparison.
function Get-SubmoduleChange([string]$repo, [string[]]$diffArgs) {
    $index = Join-Path ([IO.Path]::GetTempPath()) ("cubeshelf-index-" + [Guid]::NewGuid().ToString('N'))
    $previous = $env:GIT_INDEX_FILE
    try {
        $env:GIT_INDEX_FILE = $index
        & git -C $repo read-tree HEAD
        & git -C $repo add --all
        return @(& git -C $repo -c core.safecrlf=false diff --cached @diffArgs)
    }
    finally {
        if ($null -eq $previous) { Remove-Item env:GIT_INDEX_FILE -ErrorAction SilentlyContinue }
        else { $env:GIT_INDEX_FILE = $previous }
        Remove-Item -LiteralPath $index -Force -ErrorAction SilentlyContinue
    }
}

# Split a unified diff into one section per file, keyed by its b/ path.
#
# Comparing whole patch files stopped working once a submodule carried two of
# them: git orders a diff by path, so two patches whose files interleave
# (lib/dolphin/... and lib/webgpu/... around lib/gfx/...) never concatenate into
# the order git produces. Per file, order stops mattering, and a mismatch can
# name the file it is in.
function Split-DiffByFile([string]$diff) {
    $map = New-Object Collections.Specialized.OrderedDictionary
    $current = $null
    $buffer = New-Object Collections.Generic.List[string]
    foreach ($line in ($diff -split "`n")) {
        if ($line -match '^diff --git a/(.+?) b/(.+)$') {
            if ($null -ne $current) { $map[$current] = ($buffer -join "`n").TrimEnd("`n") }
            $current = $Matches[2].Trim()
            $buffer = New-Object Collections.Generic.List[string]
        }
        if ($null -ne $current) { $buffer.Add($line) }
    }
    if ($null -ne $current) { $map[$current] = ($buffer -join "`n").TrimEnd("`n") }
    return $map
}

function Normalise([string]$text) { return ($text -replace "`r`n", "`n").TrimEnd("`n") }

# Windows PowerShell 5.1 wraps a native command's redirected stderr in an error
# record, so with $ErrorActionPreference = 'Stop' a purely informational line -
# `git apply` warning about trailing whitespace in musyx-partyboard.patch - was
# enough to abort this script before it checked anything. Run git with the
# preference relaxed and judge it on its exit code, which is what actually says
# whether the command worked.
function Invoke-Git([string[]]$gitArgs) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & git @gitArgs 2>&1
        return [pscustomobject]@{ Code = $LASTEXITCODE; Output = @($out) }
    }
    finally { $ErrorActionPreference = $saved }
}

foreach ($pair in $pairs) {
    $submodule = Join-Path $projectPath $pair.Submodule
    Say ("--- {0}" -f $pair.Submodule)

    if (-not (Test-Path -LiteralPath $submodule)) {
        Say "  MISSING submodule; run git submodule update --init --recursive"
        $failures.Add($pair.Submodule); continue
    }

    $absent = @($pair.Patches | Where-Object { -not (Test-Path -LiteralPath (Join-Path $projectPath $_)) })
    if ($absent.Count -gt 0) {
        foreach ($a in $absent) { Say ("  MISSING patch: {0}" -f $a) }
        $failures.Add(($absent -join ', ')); continue
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

    # Which patch owns which file, and what each stores for it. Built before the
    # comparison so an overlap between two patches is named for what it is: this
    # guard has no way to decide whose version of a shared file is the right one.
    $stored = New-Object Collections.Specialized.OrderedDictionary
    $owner = @{}
    $overlap = $false
    foreach ($rel in $pair.Patches) {
        $sections = Split-DiffByFile (Normalise (Get-Content -LiteralPath (Join-Path $projectPath $rel) -Raw))
        foreach ($file in @($sections.Keys)) {
            if ($owner.ContainsKey($file)) {
                Say ("  FAIL  {0} is edited by both {1} and {2}" -f $file, $owner[$file], $rel)
                Say      "        one file, two patches: split them before this guard can check either"
                $overlap = $true
                continue
            }
            $owner[$file] = $rel
            $stored[$file] = $sections[$file]
        }
    }
    if ($overlap) { $failures.Add($pair.Submodule + ' overlapping patches'); continue }

    # Does the stored set still apply to the recorded commit? The old guard
    # compared text and never asked this, so a patch could describe the work
    # faithfully and still fail every CI build at the apply step.
    $scratch = Join-Path ([IO.Path]::GetTempPath()) ("cubeshelf-patchcheck-" + [Guid]::NewGuid().ToString('N'))
    $applyFailed = $null
    $applyOutput = @()
    try {
        $added = Invoke-Git @('-C', $submodule, 'worktree', 'add', '--detach', '--quiet', $scratch, 'HEAD')
        if ($added.Code -ne 0 -or -not (Test-Path -LiteralPath $scratch)) {
            Say ("  FAIL  could not create a scratch worktree of {0}" -f $pair.Submodule)
            foreach ($l in $added.Output) { Say ("        {0}" -f $l) }
            $failures.Add($pair.Submodule + ' scratch worktree')
            continue
        }
        foreach ($rel in $pair.Patches) {
            # --whitespace=nowarn silences a cosmetic note without changing what
            # is applied; the patches are compared byte for byte further down.
            $applied = Invoke-Git @('-C', $scratch, 'apply', '--whitespace=nowarn', (Join-Path $projectPath $rel))
            if ($applied.Code -ne 0) { $applyFailed = $rel; $applyOutput = $applied.Output; break }
        }
        if ($applyFailed) {
            Say ("  FAIL  {0} does not apply to {1}" -f $applyFailed, $actual.Substring(0, 12))
            Say      "        CI applies these in order at every checkout, so this fails every build"
            foreach ($l in $applyOutput) { Say ("        {0}" -f $l) }
            $failures.Add($applyFailed + ' does not apply')
            continue
        }
        Say ("  the {0} patch(es) apply cleanly to that commit, in order" -f $pair.Patches.Count)
        $expected = Split-DiffByFile ((Get-SubmoduleChange $scratch @()) -join "`n")
    }
    finally {
        if (Test-Path -LiteralPath $scratch) {
            Invoke-Git @('-C', $submodule, 'worktree', 'remove', '--force', $scratch) | Out-Null
            Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    $live = Split-DiffByFile ((Get-SubmoduleChange $submodule @()) -join "`n")

    $liveFiles = @($live.Keys)
    $expectedFiles = @($expected.Keys)
    $onlyLive = @($liveFiles | Where-Object { $expectedFiles -notcontains $_ })
    $onlyStored = @($expectedFiles | Where-Object { $liveFiles -notcontains $_ })
    # Compare content, not line endings: git's autocrlf rewrites files on
    # checkout and that is not a difference in the change being described.
    $differing = @($liveFiles | Where-Object {
        ($expectedFiles -contains $_) -and (Normalise $live[$_]) -ne (Normalise $expected[$_])
    })

    if ($onlyLive.Count -eq 0 -and $onlyStored.Count -eq 0 -and $differing.Count -eq 0) {
        $bytes = (($stored.Values | ForEach-Object { $_.Length }) | Measure-Object -Sum).Sum
        Say ("  ok    they describe the submodule exactly ({0} files, {1:n0} bytes)" -f $liveFiles.Count, $bytes)
        continue
    }

    if ($Update) {
        # Rewrite each patch from the live tree, but only over the files it
        # already owns. A file no patch claims cannot be placed by guessing, so
        # -Update says so instead of inventing an owner for it.
        $orphans = @($onlyLive | Where-Object { -not $owner.ContainsKey($_) })
        if ($orphans.Count -gt 0) {
            foreach ($f in $orphans) { Say ("  FAIL  {0} is in the submodule but in no patch; add it to one by hand first" -f $f) }
            $failures.Add($pair.Submodule + ' unassigned files')
            continue
        }
        foreach ($rel in $pair.Patches) {
            $mine = @($owner.Keys | Where-Object { $owner[$_] -eq $rel } | Sort-Object)
            $text = (($mine | Where-Object { $live.Contains($_) } | ForEach-Object { Normalise $live[$_] }) -join "`n")
            [IO.File]::WriteAllText((Join-Path $projectPath $rel), $text + "`n", (New-Object Text.UTF8Encoding $false))
            Say ("  UPDATED {0} ({1} files)" -f $rel, $mine.Count)
        }
        continue
    }

    Say      "  FAIL  the patches and the submodule disagree"
    Say      "        CI applies the patches, not your working tree. Whatever is missing from"
    Say      "        them is missing from every build CI produces."
    foreach ($f in $onlyLive)   { Say ("        in the submodule but NOT in any patch: {0}" -f $f) }
    foreach ($f in $onlyStored) { Say ("        in a patch but NOT in the submodule: {0} ({1})" -f $f, $owner[$f]) }
    foreach ($f in $differing)  { Say ("        same file, different content: {0} ({1})" -f $f, $owner[$f]) }
    Say      "        Re-run with -Update once you have checked that is what you mean."
    $failures.Add(($pair.Patches -join ', '))
}

Write-Output ''
if ($failures.Count -gt 0) {
    Write-Output ('submodule patches: FAIL ({0})' -f ($failures -join ', '))
    exit 1
}
Write-Output 'submodule patches: PASS'
exit 0
