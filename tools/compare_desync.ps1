# Names the field that actually diverged, by comparing the two peers' reports.
#
# WHY. A desync report says "category=OBJECTS" and dumps eight hundred named
# fields - but only the values this peer held. Reading one side tells you where
# the divergence surfaced, never what differs, so every occurrence is written up
# as "OBJECTS diverged" and the real cause stays unnamed. A minigame sweep would
# produce fifty of those.
#
# Both halves already exist. A local campaign runs its two peers under
# profile-0 and profile-1 and each writes its own report, so the comparison is
# free and automatic. Between two machines the second file has to be fetched
# from the other PC: %APPDATA%\MarioPartyRD\Party Board\netplay\.
#
# WHAT IT DOES NOT DO. It never decides which peer is right - that question has
# no answer from the reports alone - and it never merges anything. It prints
# what differs and stops.
#
#   tools\compare_desync.ps1 -Reports a_player1.log,b_player2.log
#   tools\compare_desync.ps1 -RunDirectory work\netplay-campaigns\...\001
#
# Exit codes: 0 a difference was named, 1 the two agree everywhere, 2 the
# reports could not be read or paired.

param(
    [string[]]$Reports,
    [string]$RunDirectory = '',
    [int]$MaxLines = 40
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $Reports -and $RunDirectory) {
    if (-not (Test-Path -LiteralPath $RunDirectory)) {
        Write-Output "Introuvable : $RunDirectory"; exit 2
    }
    $found = @(Get-ChildItem $RunDirectory -Recurse -Filter 'netplay_desync_*.log' -ErrorAction SilentlyContinue |
        Sort-Object Name)
    # One run can desync once; if it somehow wrote several, the newest pair is
    # the one that matches the reported failure.
    $one = @($found | Where-Object { $_.Name -like '*player1*' } | Select-Object -Last 1)
    $two = @($found | Where-Object { $_.Name -like '*player2*' } | Select-Object -Last 1)
    if ($one.Count -eq 0 -or $two.Count -eq 0) {
        Write-Output "Pas de paire player1/player2 sous $RunDirectory."
        Write-Output "  trouve : $($found.Count) rapport(s)."
        exit 2
    }
    $Reports = @($one[0].FullName, $two[0].FullName)
}
if (-not $Reports -or $Reports.Count -ne 2) {
    Write-Output 'Donnez deux rapports (-Reports a,b) ou un dossier de run (-RunDirectory).'
    exit 2
}

function Read-Report([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { Write-Output "Introuvable : $path"; exit 2 }
    $header = New-Object 'Collections.Generic.List[string]'
    $names = New-Object 'Collections.Generic.List[string]'
    $values = New-Object 'Collections.Generic.List[string]'
    $subsystems = New-Object 'Collections.Generic.List[string]'
    $digests = @{}
    foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $path))) {
        if ($line.StartsWith('FIELD ')) {
            # FIELD <index> <SUBSYSTEM> <name possibly with spaces> <hex>
            $parts = $line.Split(' ')
            if ($parts.Count -lt 4) { continue }
            $subsystems.Add($parts[2])
            $values.Add($parts[$parts.Count - 1])
            $names.Add(($parts[3..($parts.Count - 2)] -join ' '))
        } elseif ($line.StartsWith('SUBSYSTEM ')) {
            if ($line -match '^SUBSYSTEM\s+(\w+)\s+local=(\w+)\s+remote=(\w+)\s+(\w+)') {
                $digests[$Matches[1]] = @{ Local = $Matches[2]; Remote = $Matches[3]; Verdict = $Matches[4] }
            }
        } elseif (-not $line.StartsWith('DIGEST') -and -not $line.StartsWith('INPUT') -and $line.Trim()) {
            if ($header.Count -lt 20) { $header.Add($line) }
        }
    }
    return @{ Path = $path; Header = $header; Names = $names; Values = $values
              Subsystems = $subsystems; Digests = $digests }
}

$a = Read-Report $Reports[0]
$b = Read-Report $Reports[1]

function Field([hashtable]$r, [string]$key) {
    foreach ($line in $r.Header) { if ($line -match "$key=(\S+)") { return $Matches[1] } }
    return '?'
}

Write-Output ''
Write-Output '================ les deux rapports ================'
foreach ($r in $a, $b) {
    Write-Output ("  {0}" -f (Split-Path $r.Path -Leaf))
    Write-Output ("     joueur {0}  frame {1}  categorie {2}  overlay {3}  {4} champs" -f
        (Field $r 'player'), (Field $r 'first_desync_frame'), (Field $r 'category'),
        (Field $r 'overlay'), $r.Names.Count)
}

$frameA = Field $a 'first_desync_frame'; $frameB = Field $b 'first_desync_frame'
if ($frameA -ne $frameB) {
    Write-Output ''
    Write-Output "ATTENTION : les deux pairs ne signalent pas la meme frame ($frameA contre $frameB)."
    Write-Output 'La comparaison ci-dessous porte sur deux instants differents.'
}

# A difference in the number of fields is itself the finding: the two peers do
# not hold the same number of objects, processes or models, so nothing lines up
# after that point and naming "the field that differs" would be meaningless.
Write-Output ''
if ($a.Names.Count -ne $b.Names.Count) {
    Write-Output '================ structures differentes ================'
    Write-Output ("  {0} champs d'un cote, {1} de l'autre." -f $a.Names.Count, $b.Names.Count)
    $limit = [Math]::Min($a.Names.Count, $b.Names.Count)
    $split = -1
    for ($i = 0; $i -lt $limit; $i++) { if ($a.Names[$i] -ne $b.Names[$i]) { $split = $i; break } }
    if ($split -ge 0) {
        Write-Output ("  Elles divergent au champ {0}, dans {1} :" -f $split, $a.Subsystems[$split])
        $from = [Math]::Max(0, $split - 3)
        for ($i = $from; $i -lt [Math]::Min($limit, $split + 6); $i++) {
            $mark = if ($a.Names[$i] -ne $b.Names[$i]) { '  <<<' } else { '' }
            Write-Output ("    {0,5}  {1,-12} {2,-42} | {3}{4}" -f $i, $a.Subsystems[$i], $a.Names[$i], $b.Names[$i], $mark)
        }
    } else {
        Write-Output "  Le debut est identique ; l'un des deux s'arrete plus tot."
    }
    Write-Output ''
    Write-Output "Un pair porte plus d'elements que l'autre. C'est un ecart de creation,"
    Write-Output 'pas un ecart de valeur : cherchez ce qui alloue cet element de plus.'
    exit 0
}

Write-Output '================ sous-systemes ================'
foreach ($name in $a.Digests.Keys | Sort-Object) {
    $d = $a.Digests[$name]
    if ($d.Verdict -ne 'OK') { Write-Output ("  {0,-12} {1} contre {2}   DIFFERENT" -f $name, $d.Local, $d.Remote) }
}

Write-Output ''
Write-Output '================ champs qui different ================'
$differences = 0
$shown = 0
for ($i = 0; $i -lt $a.Names.Count; $i++) {
    if ($a.Values[$i] -eq $b.Values[$i]) { continue }
    $differences++
    if ($shown -lt $MaxLines) {
        Write-Output ("  {0,5}  {1,-11} {2,-44} {3}  contre  {4}" -f
            $i, $a.Subsystems[$i], $a.Names[$i], $a.Values[$i], $b.Values[$i])
        $shown++
    }
}
if ($differences -gt $shown) { Write-Output ("  ... et {0} autres (-MaxLines pour en voir plus)" -f ($differences - $shown)) }

Write-Output ''
if ($differences -eq 0) {
    Write-Output "Les deux vidages sont identiques champ pour champ."
    Write-Output "La divergence n'est donc pas dans ce qui est hache a cet instant :"
    Write-Output 'elle a ete detectee sur une frame que ces vidages ne montrent pas.'
    exit 1
}
Write-Output ("{0} champ(s) different sur {1}." -f $differences, $a.Names.Count)
$bySubsystem = @{}
for ($i = 0; $i -lt $a.Names.Count; $i++) {
    if ($a.Values[$i] -eq $b.Values[$i]) { continue }
    $key = $a.Subsystems[$i]
    if (-not $bySubsystem.ContainsKey($key)) { $bySubsystem[$key] = 0 }
    $bySubsystem[$key]++
}
foreach ($key in $bySubsystem.Keys | Sort-Object) { Write-Output ("  {0,-12} {1}" -f $key, $bySubsystem[$key]) }
exit 0
