[CmdletBinding()]
param([Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$destinationRoot = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destinationRoot) { throw 'Destination must not exist; snapshots never overwrite files.' }
if ($destinationRoot.StartsWith($sourceRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Destination must be outside the source checkout.'
}
$parts = @('', 'extern/aurora', 'extern/musyx', 'extern/libco')
$excluded = '(^|/)(\.git|\.github|\.vscode|\.idea|\.codex|\.agents|build|work|orig|outputs|__pycache__)(/|$)|(^|/)\.gitmodules$|\.(iso|rvz|gcm|wbfs|dol|rel|elf|exe|dll|obj|o|pdb|log|pcm|wav|mp4|dmp|raw|sav|gci)$|(^|/)\.env($|\.)'
$provenance = @()
$copied = 0
foreach ($part in $parts) {
    $checkout = if ($part) { Join-Path $sourceRoot $part } else { $sourceRoot }
    $safe = $checkout.Replace('\', '/')
    $revision = & git -c "safe.directory=$safe" -C $checkout rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw "Not a valid checkout: $part" }
    $provenance += [pscustomobject]@{ path = $part; revision = "$revision" }
    $files = & git -c "safe.directory=$safe" -c core.quotepath=false -C $checkout ls-files --cached --others --exclude-standard
    if ($LASTEXITCODE -ne 0) { throw "Cannot enumerate sources: $part" }
    foreach ($file in ($files | Sort-Object -Unique)) {
        $relative = if ($part) { "$part/$file" } else { $file }
        if ($relative -match $excluded) { continue }
        $source = [IO.Path]::GetFullPath((Join-Path $checkout $file))
        if (!$source.StartsWith($sourceRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Source outside checkout.' }
        if (!(Test-Path -LiteralPath $source -PathType Leaf)) { continue } # Submodule directories handled above.
        $item = Get-Item -LiteralPath $source
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Link requires manual review: $relative" }
        if ($item.Length -gt 10MB) { throw "Large file requires manual review: $relative" }
        $target = [IO.Path]::GetFullPath((Join-Path $destinationRoot $relative))
        if (!$target.StartsWith($destinationRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Target outside snapshot.' }
        New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath $source -Destination $target
        $copied++
    }
}
$provenance | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destinationRoot 'SOURCE_REVISIONS.json') -Encoding utf8
Write-Output "Copied $copied source/resource files; original images, builds, logs and local work excluded."
Write-Output 'Submodule sources are flattened with their current modifications; review snapshot before publishing.'
