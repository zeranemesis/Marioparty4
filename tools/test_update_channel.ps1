# Guards the live update channel.
#
# Why this exists. `update.json` is served from the branch and read at every
# launcher start by tools/online/UpdateService.cs. The moment its `version`
# rises above the launcher's own `CurrentVersion`, every installed copy in the
# world offers to download and unpack whatever `downloadUrl` points at, over the
# top of its own installation. That is a publication, and it is one bumped
# string away at all times.
#
# So the bump is made to cost something. Two rules:
#
#   1. While `version` equals the launcher's `CurrentVersion`, the channel is
#      dormant and nothing else is checked. This is the normal state during
#      development and it is where the channel sits today.
#
#   2. The moment `version` is raised, `downloadUrl` must point at an immutable
#      release asset - github.com/<owner>/<repo>/releases/download/<tag>/... -
#      and not at a branch path. A branch path is mutable: the same URL serves
#      different bytes tomorrow, so an artifact a tester crashed on cannot be
#      retrieved to reproduce the crash.
#
# The SHA-256 pin is checked in both cases, because it is the only thing that
# makes the download safe at all, and its absence would be silent: the launcher
# refuses a manifest without one, so a broken manifest would look like "GitHub
# is unreachable" to every user at once.
#
#   tools\test_update_channel.ps1
#
# Exit codes: 0 pass, 1 fail.

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent

$failures = New-Object Collections.Generic.List[string]
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Output ("  ok    {0}" -f $name) }
    else { Write-Output ("  FAIL  {0} - {1}" -f $name, $detail); $failures.Add($name) }
}

$manifestPath = Join-Path $projectPath 'update.json'
$servicePath = Join-Path $projectPath 'tools/online/UpdateService.cs'
Check 'update.json exists' (Test-Path -LiteralPath $manifestPath) $manifestPath
Check 'UpdateService.cs exists' (Test-Path -LiteralPath $servicePath) $servicePath
if ($failures.Count -gt 0) { Write-Output 'update channel: FAIL'; exit 1 }

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$serviceText = Get-Content -LiteralPath $servicePath -Raw

$match = [regex]::Match($serviceText, 'CurrentVersion\s*=\s*"([^"]+)"')
Check 'launcher declares a CurrentVersion' $match.Success 'CurrentVersion not found in UpdateService.cs'
if (-not $match.Success) { Write-Output 'update channel: FAIL'; exit 1 }

$current = [version]$match.Groups[1].Value
$published = $null
$parsed = [version]::TryParse([string]$manifest.version, [ref]$published)
Check 'manifest version parses' $parsed ("version = '{0}'" -f $manifest.version)
if (-not $parsed) { Write-Output 'update channel: FAIL'; exit 1 }

Write-Output ("  launcher {0}, manifest {1}" -f $current, $published)

# Always required: without a valid pin every launcher gets an error dialog, and
# with a wrong one every launcher installs something nobody chose.
Check 'manifest carries a SHA-256 pin' `
    ([regex]::IsMatch([string]$manifest.sha256, '^[0-9a-fA-F]{64}$')) `
    ("sha256 = '{0}'" -f $manifest.sha256)

$url = [string]$manifest.downloadUrl
Check 'manifest carries a download URL' (-not [string]::IsNullOrWhiteSpace($url)) 'downloadUrl is empty'

if ($published -le $current) {
    Write-Output '  channel is dormant: no installed launcher will be offered an update'
    # The URL still has to be syntactically usable, but it is allowed to point at
    # a branch path while nothing is being served from it.
} else {
    Write-Output '  channel is LIVE: every installed launcher will offer this update'
    $immutable = $url -match '^https://github\.com/[^/]+/[^/]+/releases/download/[^/]+/'
    Check 'live update points at an immutable release asset' $immutable `
        ("downloadUrl = '{0}' - a branch path serves different bytes tomorrow, so a crash reported against this build could not be reproduced" -f $url)

    # A release asset implies a tag; the tag is what the workflow now creates,
    # and what makes the artifact bisectable.
    $tagMatch = [regex]::Match($url, '/releases/download/([^/]+)/')
    if ($tagMatch.Success) { Write-Output ("  release tag: {0}" -f $tagMatch.Groups[1].Value) }
}

# A 32 MB zip committed into the tree is carried by every clone forever and is
# exactly the mutable-URL problem in file form.
$distZips = @(Get-ChildItem (Join-Path $projectPath 'dist') -Filter '*.zip' -ErrorAction SilentlyContinue)
foreach ($zip in $distZips) {
    $tracked = (& git -C $projectPath ls-files --error-unmatch ("dist/" + $zip.Name) 2>$null)
    if ($LASTEXITCODE -eq 0 -and $tracked) {
        Write-Output ("  note  dist/{0} is tracked in git ({1:n0} MB); new zips are ignored, this one is kept because the manifest still names it" -f $zip.Name, ($zip.Length / 1MB))
    }
}

if ($failures.Count -gt 0) {
    Write-Output ('update channel: FAIL ({0})' -f ($failures -join ', '))
    exit 1
}
Write-Output 'update channel: PASS'
exit 0
