param(
    # Aurora owns the live cache, not the port, and it resolves its directory with
    # SDL_GetPrefPath(nullptr, "Party Board") -- a NULL organisation, so the path
    # is %APPDATA%\Party Board\, one level shallower than the port's own config
    # directory (%APPDATA%\MarioPartyRD\Party Board\). Verified on a live run:
    # pipeline_cache.db and dawn_cache.db both sit in the former.
    [string]$ConfigDir = (Join-Path $env:APPDATA 'Party Board'),
    # Where the seed has to land: CMakeLists.txt installs it from the source root
    # to sit beside partyboard.exe, which is where Aurora's seed_pipeline_cache()
    # reads it from (g_config.resourcesPath) and merges it into the live cache.
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'initial_pipeline_cache.db'),
    # Refuse to overwrite a larger seed with a smaller one. A short play session
    # records fewer pipelines than a thorough one, and silently replacing a good
    # recording with a worse one is the easy mistake here.
    [switch]$AllowShrink
)

$ErrorActionPreference = 'Stop'

# Why this script exists: a pipeline that is still compiling does not delay its
# draw, it drops it (extern/aurora/lib/gx/pipeline.cpp:18). A player with an empty
# cache therefore sees missing geometry for a few seconds on every scene they
# visit for the first time. Seeding the cache is the only thing that prevents it,
# and the seed can only be produced by actually rendering the scenes.

$source = Join-Path $ConfigDir 'pipeline_cache.db'

if (-not (Test-Path -LiteralPath $source)) {
    throw "No pipeline cache at '$source'. Run the game once, visit the scenes worth seeding, then quit it normally."
}

# SQLite keeps recent writes in a side journal until the last connection closes.
# Copying the .db alone while one exists yields a cache missing its newest rows.
foreach ($sidecar in @("$source-wal", "$source-journal")) {
    if (Test-Path -LiteralPath $sidecar) {
        throw "'$sidecar' is present, so the cache is still open or was not closed cleanly. Quit PartyBoard and run this again."
    }
}

$sourceInfo = Get-Item -LiteralPath $source
if ($sourceInfo.Length -lt 8192) {
    throw "'$source' is only $($sourceInfo.Length) bytes, which is an empty database. Play further before capturing."
}

if ((Test-Path -LiteralPath $Destination) -and -not $AllowShrink) {
    $existing = (Get-Item -LiteralPath $Destination).Length
    if ($sourceInfo.Length -lt $existing) {
        throw "Refusing to replace a $existing byte seed with a smaller $($sourceInfo.Length) byte one. Pass -AllowShrink if that is deliberate."
    }
}

Copy-Item -LiteralPath $source -Destination $Destination -Force

$kib = [math]::Round($sourceInfo.Length / 1KB, 1)
Write-Output "Captured $kib KiB -> $Destination"
Write-Output ''
Write-Output 'This file is a recording, not a build product, so nothing regenerates it:'
Write-Output '  - commit it, or CI will keep shipping packages without a seed;'
Write-Output '  - recapture it whenever the shader or pipeline schema changes, since'
Write-Output '    Aurora rejects a seed whose aurora_schema value does not match'
Write-Output '    (it logs "does not use schema version" and falls back to an empty cache).'
