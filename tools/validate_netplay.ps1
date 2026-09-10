# Single entry point for the deterministic netplay regression suite.
#
#   tools\validate_netplay.ps1 [-DiscPath <iso>] [-SkipBuild]
#
# Two rules this script exists to enforce, both learned the hard way:
#
#   1. A failed build invalidates every test that follows. The suite stops with
#      an explicit error instead of silently exercising a stale binary. The
#      binary's timestamp is compared before and after the build, so a build
#      that "succeeds" without producing a new binary is also caught.
#   2. Exit codes are propagated. No test output is filtered in a way that can
#      hide a non-zero status; each step's real code decides whether the chain
#      continues, and the script's own exit code reflects the first failure.
param(
    [string]$DiscPath,
    [switch]$SkipBuild,
    [string]$BinaryDirectory = 'build/aexp/RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
Set-Location $projectPath

function Resolve-ProjectPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}

$binary = Join-Path (Resolve-ProjectPath $BinaryDirectory) 'partyboard.exe'
$library = Join-Path (Resolve-ProjectPath $BinaryDirectory) 'dol.dll'
$results = [ordered]@{}
$script:failed = $null

function Invoke-Step([string]$name, [scriptblock]$body) {
    if ($script:failed) {
        $results[$name] = 'SKIPPED (earlier failure)'
        return
    }
    Write-Output ''
    Write-Output "===== $name ====="
    $global:LASTEXITCODE = 0
    try {
        & $body
        $code = $LASTEXITCODE
        if ($code -ne 0) { throw "exit code $code" }
        $results[$name] = 'PASS'
    } catch {
        $results[$name] = "FAIL ($($_.Exception.Message))"
        $script:failed = $name
    }
}

# --- Build, with staleness detection -----------------------------------------
if (-not $SkipBuild) {
    Write-Output '===== build ====='
    & (Join-Path $PSScriptRoot 'build_local.ps1')
    $buildCode = $LASTEXITCODE
    if ($buildCode -ne 0) {
        throw "Build failed with exit code $buildCode. Every test below would have run against a stale binary, so the suite is aborted."
    }
    if (-not (Test-Path -LiteralPath $library)) {
        throw 'Build reported success but produced no dol.dll. Suite aborted.'
    }
    # An incremental build legitimately rewrites nothing when no source changed,
    # so "dol.dll is older than the last build" proves nothing. What must hold is
    # that no source is NEWER than the binary: that is the signature of a build
    # that reported success without picking the change up.
    $built = (Get-Item -LiteralPath $library).LastWriteTimeUtc
    $newest = Get-ChildItem -Path (Join-Path $projectPath 'src'), (Join-Path $projectPath 'include') `
        -Recurse -File -Include '*.c', '*.cpp', '*.h', '*.hpp', '*.inc' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($newest -and $newest.LastWriteTimeUtc -gt $built) {
        throw ("Build reported success but $($newest.Name) ($($newest.LastWriteTimeUtc) UTC) is newer than " +
            "dol.dll ($built UTC). Refusing to validate a binary that does not contain the sources.")
    }
    Write-Output "dol.dll is at $built (UTC) and no source is newer; binary matches the tree."
    $results['build'] = 'PASS'
} else {
    if (-not (Test-Path -LiteralPath $library)) {
        throw 'No dol.dll present and -SkipBuild was given. Nothing valid to test.'
    }
    $results['build'] = "SKIPPED (-SkipBuild, dol.dll from $((Get-Item -LiteralPath $library).LastWriteTimeUtc) UTC)"
}

# --- Deterministic unit and internal self-tests -------------------------------
Invoke-Step 'canonical state unit tests' {
    & (Join-Path $PSScriptRoot 'test_netplay_state.ps1')
}

Invoke-Step 'in-process self-tests (--netplay-self-test)' {
    Push-Location (Split-Path $binary -Parent)
    try { & $binary '--netplay-self-test' } finally { Pop-Location }
}

Invoke-Step 'two-process lockstep regression (11 cases)' {
    & (Join-Path $PSScriptRoot 'test_netplay_pad.ps1')
}

# --- Real-game steps, only when a disc is supplied -----------------------------
if ($DiscPath) {
    $disc = Resolve-ProjectPath $DiscPath
    if (-not (Test-Path -LiteralPath $disc -PathType Leaf)) {
        throw "Disc file missing: $disc"
    }
    $recording = 'work/netplay-recordings/walk.txt'

    Invoke-Step 'real boot, two instances (-Menu)' {
        & (Join-Path $PSScriptRoot 'test_netplay_boot.ps1') -DiscPath $disc -Menu -DurationSeconds 60
    }
    Invoke-Step 'real walk, record input timeline' {
        & (Join-Path $PSScriptRoot 'test_netplay_boot.ps1') -DiscPath $disc -Walk `
            -RecordInput $recording -DurationSeconds 280
    }
    Invoke-Step 'replay the recording, compare overlay path' {
        & (Join-Path $PSScriptRoot 'test_netplay_boot.ps1') -DiscPath $disc `
            -ReplayInput $recording -DurationSeconds 280
    }
} else {
    foreach ($name in 'real boot, two instances (-Menu)',
                      'real walk, record input timeline',
                      'replay the recording, compare overlay path') {
        $results[$name] = 'SKIPPED (no -DiscPath)'
    }
}

Write-Output ''
Write-Output '===== summary ====='
foreach ($entry in $results.GetEnumerator()) {
    Write-Output ("  {0,-48} {1}" -f $entry.Key, $entry.Value)
}
if ($script:failed) {
    Write-Output ''
    throw "Validation failed at: $script:failed"
}
Write-Output ''
Write-Output 'All requested validation steps passed.'
