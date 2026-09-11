# Checks the recordings against tests/replays/manifest.json.
#
# A recording is evidence. It is large, so it is not versioned; its hash is.
# A campaign run against a recording whose hash has changed is a campaign about
# a different recording, and calling the two by the same name would make every
# past result unreadable.
#
#   tools\verify_replays.ps1              check every entry
#   tools\verify_replays.ps1 -Update      rewrite the manifest from what is on disk
#
# Exit codes: 0 every entry matches, 1 something differs, 2 something is missing.

param(
    [string]$Manifest = 'tests/replays/manifest.json',
    [switch]$Update
)

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$manifestPath = if ([IO.Path]::IsPathRooted($Manifest)) { $Manifest } else { Join-Path $projectPath $Manifest }
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Manifest missing: $manifestPath" }
# Deliberately NOT $manifest: PowerShell variable names are case-insensitive, so
# that would be the [string]$Manifest parameter, and assigning a parsed object to
# a string-typed variable silently stores its ToString(). The loop then iterates
# over nothing and the script reports that every entry matched.
$document = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

$missing = 0
$different = 0
$matched = 0
$rows = @()

foreach ($entry in $document.replays) {
    $path = Join-Path $projectPath $entry.path
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Output ("MISSING   {0}" -f $entry.path)
        $missing++
        continue
    }
    $actualBytes = (Get-Item -LiteralPath $path).Length
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
    if ($actualHash -eq $entry.sha256) {
        Write-Output ("OK        {0}  {1} bytes" -f $entry.path, $actualBytes)
        $matched++
    } else {
        Write-Output ("DIFFERENT {0}" -f $entry.path)
        Write-Output ("          expected {0}" -f $entry.sha256)
        Write-Output ("          actual   {0}" -f $actualHash)
        Write-Output ("          {0} bytes on disk, {1} in the manifest" -f $actualBytes, $entry.bytes)
        $different++
    }
    $rows += [pscustomobject]@{ path = $entry.path; bytes = $actualBytes; hash = $actualHash }
}

if ($Update) {
    foreach ($entry in $document.replays) {
        $row = $rows | Where-Object { $_.path -eq $entry.path } | Select-Object -First 1
        if ($row) { $entry.sha256 = $row.hash; $entry.bytes = $row.bytes }
    }
    $document.generated_at = (Get-Date).ToString('o')
    $document | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    Write-Output "Manifest rewritten from what is on disk."
    exit 0
}

Write-Output ""
Write-Output ("matched={0} different={1} missing={2}" -f $matched, $different, $missing)
# Checking nothing is not the same as checking everything and finding it correct.
if (($matched + $different + $missing) -eq 0) {
    Write-Output "The manifest listed no recording to check."
    exit 2
}
if ($missing -gt 0) { exit 2 }
if ($different -gt 0) { exit 1 }
exit 0
