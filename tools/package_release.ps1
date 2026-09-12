# Builds the folder a second PC needs, and proves the zip carries it intact.
#
# WHY THIS IS A SCRIPT. The lobby refuses to start a game unless both PCs hold
# byte-identical folders: Wire.BuildHash in tools/online/Connection.cs hashes
# every *.dll in the directory plus partyboard.exe and PartyBoardOnline.exe.
# A hand-assembled copy that silently drops or adds one DLL produces a refusal
# on the other machine, hours later, with no way to tell which file moved. So
# the package is assembled from a whitelist, its hash is recorded, and the zip
# is unpacked again and re-hashed before this script reports success.
#
# WHAT IS DELIBERATELY EXCLUDED. Debug symbols (.pdb, 160 MB for dol.dll
# alone), link artefacts (.lib/.exp), and every test output that has collected
# in build/install (.pcm, .log, .txt, .dmp). None of them are read at runtime
# and none of them enter the build hash.
#
# WHAT IS DELIBERATELY INCLUDED. The four Visual C++ runtime DLLs the binaries
# import. They are part of the hashed set, so shipping them inside the zip is
# what keeps two machines equal; leaving them out would make the folder depend
# on whatever redistributable each PC happens to have installed.
#
#   tools\package_release.ps1
#   tools\package_release.ps1 -OutputDirectory D:\transfert
#
# Exit codes: 0 packaged and verified, 2 refused before building, 3 the zip
# did not verify.

param(
    [string]$BuildDirectory = 'build/install',
    [string]$OutputDirectory = 'dist',
    [string]$FolderName = 'PartyBoard',
    [switch]$NoRuntime,
    [switch]$AllowStale
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem

$projectPath = Split-Path $PSScriptRoot -Parent
function Resolve-ProjectPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}
function Fail([string]$message, [int]$code) {
    Write-Output ''
    Write-Output "REFUS: $message"
    exit $code
}

$build = Resolve-ProjectPath $BuildDirectory
if (-not (Test-Path -LiteralPath $build -PathType Container)) {
    Fail "$build est introuvable. Construisez le jeu d'abord." 2
}

# ---------------------------------------------------------------- preconditions

$required = @(
    'partyboard.exe', 'PartyBoardOnline.exe', 'PartyBoardOnline.exe.config',
    'dol.dll', 'dsp_coef.bin'
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $name) -PathType Leaf)) {
        Fail "$name manque dans $build." 2
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $build 'res') -PathType Container)) {
    Fail "le dossier res manque dans $build." 2
}

# A package built from binaries older than the sources would ship code nobody
# has ever run. Refusing is cheaper than discovering it on the other machine.
#
# Only partyboard.exe and dol.dll are compared. They are what src/ and include/
# actually produce; the third-party libraries next to them (SDL3, dxcompiler,
# libpng, zlib) are prebuilt and permanently older than any source edit, so
# including them would make this check fire on every single run and teach
# everyone to pass -AllowStale.
$newestSource = Get-ChildItem (Join-Path $projectPath 'src'), (Join-Path $projectPath 'include') `
    -Recurse -File -Include '*.c', '*.cpp', '*.h', '*.hpp' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
#
# It compares against the NEWEST of the two, not both, and that limit is
# deliberate: partyboard.exe is a thin entry point that does not depend on
# src/game, so a change to src/game/process.c legitimately leaves it untouched
# while dol.dll is rebuilt. Requiring both to be newer than every source turns
# a normal build into a refusal. What survives is the failure that matters:
# somebody edited a source and never rebuilt anything at all. Which target
# needed which source is CMake's knowledge, not this script's - run the build
# itself if you want that answer.
if ($newestSource) {
    $newestBinary = @('partyboard.exe', 'dol.dll') |
        ForEach-Object { Get-Item -LiteralPath (Join-Path $build $_) } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($newestSource.LastWriteTimeUtc -gt $newestBinary.LastWriteTimeUtc) {
        $message = "$($newestSource.Name) a ete modifie apres la derniere construction " +
            "($($newestSource.LastWriteTime.ToString('MM-dd HH:mm')) contre " +
            "$($newestBinary.Name) a $($newestBinary.LastWriteTime.ToString('MM-dd HH:mm'))). " +
            "Reconstruisez, ou passez -AllowStale si c'est voulu."
        if (-not $AllowStale) { Fail $message 2 }
        Write-Output "AVERTISSEMENT: $message"
    }
}

# ------------------------------------------------------------------ build hash

function Get-BuildHash([string]$root) {
    $names = New-Object 'Collections.Generic.List[string]'
    foreach ($file in [IO.Directory]::GetFiles($root, '*.dll')) {
        $names.Add([IO.Path]::GetFileName($file))
    }
    $names.Add('partyboard.exe')
    $names.Add('PartyBoardOnline.exe')
    $ordered = $names.ToArray()
    [Array]::Sort($ordered, [StringComparer]::OrdinalIgnoreCase)

    $sha = [Security.Cryptography.SHA256]::Create()
    $buffer = New-Object IO.MemoryStream
    try {
        foreach ($name in $ordered) {
            $label = [Text.Encoding]::UTF8.GetBytes($name.ToLowerInvariant() + [char]10)
            $buffer.Write($label, 0, $label.Length)
            $stream = [IO.File]::OpenRead((Join-Path $root $name))
            try {
                $digest = $sha.ComputeHash($stream)
                $buffer.Write($digest, 0, $digest.Length)
            } finally { $stream.Dispose() }
        }
        return (($sha.ComputeHash($buffer.ToArray()) | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally { $buffer.Dispose(); $sha.Dispose() }
}

# --------------------------------------------------------------------- staging

$stagingRoot = Join-Path ([IO.Path]::GetTempPath()) ('partyboard-package-' + [Guid]::NewGuid().ToString('N'))
$staging = Join-Path $stagingRoot $FolderName
$verifyRoot = Join-Path ([IO.Path]::GetTempPath()) ('partyboard-verify-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging -Force | Out-Null

try {
    $shipped = 0
    foreach ($file in Get-ChildItem $build -File) {
        if ($file.Extension -notin '.dll', '.exe', '.config', '.bin') { continue }
        Copy-Item $file.FullName (Join-Path $staging $file.Name) -Force
        $shipped++
    }
    Copy-Item (Join-Path $build 'res') $staging -Recurse -Force
    Write-Output "$shipped fichiers binaires, plus res/."

    if (-not $NoRuntime) {
        # Only what the binaries actually import, established by scanning every
        # shipped image for its MSVC runtime imports.
        $runtime = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll')
        $sources = @()
        foreach ($root in 'C:\Program Files (x86)\Microsoft Visual Studio', 'C:\Program Files\Microsoft Visual Studio') {
            if (Test-Path -LiteralPath $root) {
                $sources += Get-ChildItem $root -Recurse -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue |
                    Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC' } |
                    Sort-Object FullName -Descending | Select-Object -ExpandProperty FullName
            }
        }
        $sources += 'C:\Windows\System32'
        foreach ($name in $runtime) {
            $picked = $null
            foreach ($source in $sources) {
                $candidate = Join-Path $source $name
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { $picked = $candidate; break }
            }
            if (-not $picked) { Fail "$name est introuvable sur cette machine." 2 }
            Copy-Item $picked (Join-Path $staging $name) -Force
        }
        Write-Output "$($runtime.Count) bibliotheques Visual C++ ajoutees."
    }

    foreach ($name in 'Lancer PartyBoard.cmd', 'Jouer en ligne.cmd', 'Verifier le dossier.cmd') {
        $source = Join-Path $PSScriptRoot (Join-Path 'package' $name)
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Fail "tools/package/$name manque." 2 }
        Copy-Item $source (Join-Path $staging $name) -Force
    }

    $buildHash = Get-BuildHash $staging
    Write-Output "empreinte du dossier : $buildHash"

    # --------------------------------------------------------------- documents

    $commit = (& git -C $projectPath rev-parse HEAD).Trim()
    $describe = (& git -C $projectPath rev-parse --short HEAD).Trim()
    $dirty = (& git -C $projectPath status --porcelain) -join ''
    if ($dirty) { $commit = "$commit (arbre modifie)" }
    $version = 'dev-' + $describe
    foreach ($line in Get-Content (Join-Path $projectPath 'tools/online/UpdateService.cs')) {
        if ($line -match 'CurrentVersion\s*=\s*"([^"]+)"') { $version = $Matches[1] + ' / dev-' + $describe }
    }
    $stamp = (Get-Date).ToString('yyyy-MM-dd HH:mm')

    $template = Join-Path $PSScriptRoot 'package/LISEZ-MOI.txt'
    if (-not (Test-Path -LiteralPath $template -PathType Leaf)) { Fail 'tools/package/LISEZ-MOI.txt manque.' 2 }
    $readme = [IO.File]::ReadAllText($template, (New-Object Text.UTF8Encoding $false))
    $readme = $readme.Replace('@VERSION@', $version).Replace('@COMMIT@', $commit)
    $readme = $readme.Replace('@BUILD_HASH@', $buildHash).Replace('@DATE@', $stamp)
    $leftover = ([regex]::Matches($readme, '@[A-Z_]+@') | ForEach-Object { $_.Value }) -join ' '
    if ($leftover) { Fail "le modele LISEZ-MOI garde des champs non remplis : $leftover" 2 }
    # A BOM, because Windows tools read a headerless file as ANSI and turn every
    # accent into mojibake.
    [IO.File]::WriteAllText((Join-Path $staging 'LISEZ-MOI.txt'), $readme, (New-Object Text.UTF8Encoding $true))

    $lines = New-Object 'Collections.Generic.List[string]'
    $lines.Add('# Empreintes SHA-256 de chaque fichier livre.')
    $lines.Add('# Empreinte du dossier, celle que le salon compare : ' + $buildHash)
    $lines.Add('')
    foreach ($file in Get-ChildItem $staging -Recurse -File | Sort-Object FullName) {
        $relative = $file.FullName.Substring($staging.Length + 1)
        $lines.Add(((Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $relative))
    }
    [IO.File]::WriteAllLines((Join-Path $staging 'empreintes.txt'), $lines)

    $manifest = [ordered]@{
        schema       = 1
        game         = 'Mario Party 4'
        gameId       = 'GMPE01_00'
        version      = $version
        commit       = $commit
        buildHash    = $buildHash
        builtAt      = (Get-Date).ToUniversalTime().ToString('o')
        platform     = 'win-x64'
        executable   = 'partyboard.exe'
        launcher     = 'PartyBoardOnline.exe'
        discIncluded = $false
        validation   = 'TEST. Aucune session entre deux machines physiques n a jamais ete menee.'
    } | ConvertTo-Json -Depth 4
    [IO.File]::WriteAllText((Join-Path $staging 'manifest.json'), $manifest, (New-Object Text.UTF8Encoding $false))

    # --------------------------------------------------------------------- zip

    $outDir = Resolve-ProjectPath $OutputDirectory
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    $zip = Join-Path $outDir ('PartyBoard-win-x64-' + $describe + '.zip')
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }

    Write-Output 'compression...'
    [IO.Compression.ZipFile]::CreateFromDirectory($staging, $zip,
        [IO.Compression.CompressionLevel]::Optimal, $true)

    # ------------------------------------------------------------ verification

    Write-Output 'verification du zip...'
    New-Item -ItemType Directory -Path $verifyRoot -Force | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $verifyRoot)
    $extracted = Join-Path $verifyRoot $FolderName
    if (-not (Test-Path -LiteralPath $extracted -PathType Container)) {
        Fail "le zip ne contient pas de dossier $FolderName." 3
    }

    $problems = New-Object 'Collections.Generic.List[string]'
    $roundTrip = Get-BuildHash $extracted
    if ($roundTrip -ne $buildHash) {
        $problems.Add("empreinte apres extraction $roundTrip au lieu de $buildHash")
    }
    $expected = $required + @('LISEZ-MOI.txt', 'manifest.json', 'empreintes.txt',
        'Jouer en ligne.cmd', 'Lancer PartyBoard.cmd', 'Verifier le dossier.cmd')
    foreach ($name in $expected) {
        if (-not (Test-Path -LiteralPath (Join-Path $extracted $name) -PathType Leaf)) {
            $problems.Add("$name absent du zip")
        }
    }
    foreach ($banned in '*.pdb', '*.lib', '*.exp', '*.pcm', '*.dmp') {
        $found = @(Get-ChildItem $extracted -File -Filter $banned -ErrorAction SilentlyContinue)
        if ($found.Count) { $problems.Add("$($found.Count) fichier(s) $banned n auraient pas du etre livres") }
    }
    $sourceCount = @(Get-ChildItem $staging -Recurse -File).Count
    $zipCount = @(Get-ChildItem $extracted -Recurse -File).Count
    if ($sourceCount -ne $zipCount) { $problems.Add("$zipCount fichiers extraits pour $sourceCount prepares") }

    if ($problems.Count) {
        Write-Output ''
        Write-Output 'LE ZIP NE PASSE PAS SA VERIFICATION :'
        foreach ($problem in $problems) { Write-Output "  - $problem" }
        exit 3
    }

    $size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    Write-Output ''
    Write-Output "OK  $zip"
    Write-Output "    $zipCount fichiers, $size Mo"
    Write-Output "    empreinte $buildHash"
    Write-Output "    commit    $commit"
    Write-Output ''
    Write-Output 'Le zip a ete extrait et re-hache : son contenu est celui qui a ete prepare.'
    exit 0
}
finally {
    foreach ($path in $stagingRoot, $verifyRoot) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
