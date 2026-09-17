# Checks that the launcher's UDP bridge still recognises the engine's packets.
#
# Why this exists, and what it caught. Two programs have to agree on the shape
# of one datagram. src/port/netplay_transport.cpp emits it; the bridge in
# tools/online/Connection.cs forwards it between the two PCs, and forwards it
# only if it recognises it - by exact byte length, and by the protocol version
# in bytes 4 and 5. Both numbers were transcribed into the C# by hand.
#
# On 2026-09-10, "netplay: complete canonical deterministic state hashing"
# raised the packet from 88 to 152 bytes and the protocol from 6 to 7. The C#
# copy stayed at 88 and 6. From that commit on, the bridge dropped 100 % of
# game traffic while showing no error at all: both lobbies connected, the disc
# hashes matched, the host pressed start, both games launched - and each one
# waited two minutes and died with "Aucun joueur compatible apres 2 minutes".
# Every local test kept passing, because a local session never crosses the
# bridge.
#
# The numbers are now generated into WireFormat.generated.cs at build time, so
# they cannot be transcribed wrongly. This test guards the remaining ways to
# lose that: deleting the generator, or writing a fresh literal next to it.
#
#   tools\test_wire_format.ps1
#
# Exit codes: 0 the two sides agree, 1 they do not, 2 a file is missing.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectPath = Split-Path $PSScriptRoot -Parent

$failures = New-Object Collections.Generic.List[string]
function Check([bool]$condition, [string]$name) {
    if ($condition) { Write-Output "  PASS  $name" }
    else { Write-Output "  FAIL  $name"; $failures.Add($name) }
}

function Need([string]$relative) {
    $path = Join-Path $projectPath $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-Output "wire format: fichier manquant, $relative"
        exit 2
    }
    return $path
}

$header = Need 'include/port/netplay_transport.hpp'
$connection = Need 'tools/online/Connection.cs'
$builder = Need 'tools/build_online.ps1'

# ------------------------------------------------------- the engine's numbers

$packetSize = $null
$protocolVersion = $null
foreach ($line in Get-Content $header) {
    if ($line -match 'kNetplayPacketSize\s*=\s*(\d+)') { $packetSize = [int]$Matches[1] }
    if ($line -match 'kProtocolVersion\s*=\s*(\d+)') { $protocolVersion = [int]$Matches[1] }
}
if (-not $packetSize -or -not $protocolVersion) {
    Write-Output 'wire format: impossible de lire les constantes du moteur.'
    exit 2
}
Write-Output "moteur : paquet $packetSize octets, protocole v$protocolVersion"
Write-Output ''

# ------------------------------------------------- the launcher must not guess

$connectionText = Get-Content $connection -Raw
$builderText = Get-Content $builder -Raw

Check ($connectionText -match 'Payload\s*=\s*WireFormat\.PacketSize') `
    'la taille du paquet vient du moteur, pas d une copie'
Check ($connectionText -match 'b\[4\]\s*==\s*WireFormat\.VersionHigh') `
    'la version de protocole vient du moteur, pas d une copie'
Check (-not ($connectionText -match 'Payload\s*=\s*\d')) `
    'aucune taille de paquet ecrite en dur dans Connection.cs'
Check (-not ($connectionText -match 'b\[5\]\s*==\s*\d')) `
    'aucune version de protocole ecrite en dur dans Connection.cs'
Check ($builderText -match 'kNetplayPacketSize' -and $builderText -match 'kProtocolVersion') `
    'build_online.ps1 lit bien l en-tete du moteur'
Check ($builderText -match 'WireFormat\.generated\.cs') `
    'build_online.ps1 genere toujours WireFormat.generated.cs'

# ------------------------------------- and what it generated must still agree

$generated = @(Get-ChildItem (Join-Path $projectPath 'build') -Recurse -File `
    -Filter 'WireFormat.generated.cs' -ErrorAction SilentlyContinue)
if ($generated.Count -eq 0) {
    Write-Output '  SKIP  aucun WireFormat.generated.cs construit ; lancez tools\build_online.ps1'
} else {
    foreach ($file in $generated) {
        $text = Get-Content $file.FullName -Raw
        $builtSize = if ($text -match 'PacketSize\s*=\s*(\d+)') { [int]$Matches[1] } else { -1 }
        $builtVersion = if ($text -match 'ProtocolVersion\s*=\s*(\d+)') { [int]$Matches[1] } else { -1 }
        $relative = $file.FullName.Substring($projectPath.Length + 1)
        Check ($builtSize -eq $packetSize -and $builtVersion -eq $protocolVersion) `
            "$relative porte $builtSize octets / v$builtVersion"
    }
}

Write-Output ''
if ($failures.Count -gt 0) {
    Write-Output ('wire format: FAIL ({0})' -f ($failures.Count))
    Write-Output 'Le pont du lanceur ne reconnaitrait pas les paquets du jeu :'
    Write-Output 'les deux PC se verraient dans le salon et ne se verraient jamais en partie.'
    exit 1
}
Write-Output 'wire format: PASS'
exit 0
