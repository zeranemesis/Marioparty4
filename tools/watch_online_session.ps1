# Watches a real two-machine session as it happens, and says what it saw.
#
# WHY. Every campaign so far ran two peers on one machine over 127.0.0.1. The
# numbers that only a real link can produce - ping, stalled ticks, dropped
# datagrams, whether the control channel survives - have never been measured.
# The launcher already writes them, every five seconds, into
# %LOCALAPPDATA%\PartyBoard\Diagnostics\<stamp>\session.txt. Nothing read them
# live, so a session that failed left a folder nobody looked at until later.
#
# WHAT IT DOES NOT DO. It never touches the session, never writes into the
# diagnostic, never restarts anything. It reads, and it condenses: the phase
# line repeats every five seconds and is nearly always identical, so only
# transitions and moving counters are printed.
#
#   tools\watch_online_session.ps1
#   tools\watch_online_session.ps1 -Seconds 7200 -Since '2026-09-12 10:05'
#
# Exit codes: 0 the watch ended normally, 2 nothing to watch.

param(
    [int]$Seconds = 3600,
    [string]$Since = '',
    [int]$PollMs = 2000
)

$ErrorActionPreference = 'Stop'
$root = Join-Path $env:LOCALAPPDATA 'PartyBoard'
$diagnostics = Join-Path $root 'Diagnostics'
$crashes = Join-Path $root 'crashes'
if (-not (Test-Path -LiteralPath $diagnostics)) {
    Write-Output "Aucun dossier $diagnostics. Ouvrez le salon une fois."
    exit 2
}

$cutoff = if ($Since) { [datetime]::Parse($Since) } else { Get-Date }
function Say([string]$text) {
    Write-Output ((Get-Date -Format 'HH:mm:ss') + '  ' + $text)
}
Say "surveillance demarree, sessions creees apres $($cutoff.ToString('HH:mm:ss'))"

$knownCrashes = @{}
if (Test-Path -LiteralPath $crashes) {
    foreach ($f in Get-ChildItem $crashes -File) { $knownCrashes[$f.Name] = $true }
}

$current = $null
$offset = 0
$lastPhase = ''
$pings = New-Object 'Collections.Generic.List[int]'
$lastLocal = -1
$lastPeer = -1
$lastNative = 0
$sawGameTraffic = $false
$deadline = (Get-Date).AddSeconds($Seconds)

while ((Get-Date) -lt $deadline) {
    # A retry creates a new folder; follow the newest one that is ours.
    $candidate = Get-ChildItem $diagnostics -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.CreationTime -ge $cutoff } |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($candidate -and (-not $current -or $candidate.FullName -ne $current.FullName)) {
        if ($current) { Say '' }
        $current = $candidate
        $offset = 0; $lastPhase = ''; $lastLocal = -1; $lastPeer = -1; $lastNative = 0
        $sawGameTraffic = $false
        $pings.Clear()
        Say "=== nouvelle session $($current.Name) ==="
    }

    if ($current) {
        $file = Join-Path $current.FullName 'session.txt'
        if (Test-Path -LiteralPath $file) {
            $lines = @(Get-Content $file -ErrorAction SilentlyContinue)
            for ($i = $offset; $i -lt $lines.Count; $i++) {
                $line = $lines[$i]
                $body = ($line -split ' ', 2)[1]
                if (-not $body) { continue }

                if ($body -match '^phase=(\w+)') {
                    $phase = $Matches[1]
                    $match = if ($body -match 'disk_match=(\w+)') { $Matches[1] } else { '?' }
                    $ping = if ($body -match 'ping_ms=(\d+)') { [int]$Matches[1] } else { -1 }
                    $localPk = if ($body -match 'local_packets=(\d+)') { [int]$Matches[1] } else { -1 }
                    $peerPk = if ($body -match 'peer_packets=(\d+)') { [int]$Matches[1] } else { -1 }
                    $dropped = if ($body -match 'udp_dropped=(\d+)') { [int]$Matches[1] } else { 0 }
                    $sockErr = if ($body -match 'udp_error=(\d+)') { [int]$Matches[1] } else { 0 }
                    if ($ping -ge 0) { $pings.Add($ping) }

                    if ($phase -ne $lastPhase) {
                        Say "phase -> $phase   disque=$match"
                        if ($phase -eq 'Preparing') { Say '   >>> l hote a lance la partie, les deux jeux chargent' }
                        if ($phase -eq 'Running') { Say '   >>> SIMULATION DEMARREE sur les deux PC' }
                        $lastPhase = $phase
                    }
                    if (($localPk -ne $lastLocal -or $peerPk -ne $lastPeer) -and ($localPk -gt 0 -or $peerPk -gt 0)) {
                        if (-not $sawGameTraffic) {
                            Say '   >>> PREMIER TRAFIC DE JEU A TRAVERS LE PONT'
                            $sawGameTraffic = $true
                        }
                        Say ("   jeu->reseau {0}  reseau->jeu {1}  ping {2} ms  perdus {3}  erreur {4}" -f
                            $localPk, $peerPk, $ping, $dropped, $sockErr)
                        $lastLocal = $localPk; $lastPeer = $peerPk
                    }
                    if ($sockErr -ne 0) { Say "   ATTENTION erreur de socket UDP $sockErr" }
                }
                elseif ($body -match 'build=([0-9A-F]{64})') {
                    $hash = $Matches[1].ToLowerInvariant()
                    $same = $hash -eq 'ff2a80402d42fce13911da9abeef09fe9f0584845ce1ce0c4262b169e6b1651c'
                    Say ("handshake accepte : {0}" -f ($body -replace 'build=[0-9A-F]{64}', 'build=...'))
                    Say ("   empreinte {0}" -f $(if ($same) { 'identique au paquet livre' } else { "INATTENDUE $hash" }))
                }
                else {
                    Say $body
                    if ($body -match 'socket_code=10054') { Say "   (10054 = l autre PC a coupe la connexion)" }
                    if ($body -match 'socket_code=10060') { Say "   (10060 = delai depasse, l autre PC ne repond plus)" }
                    if ($body -match 'control_lost') { Say '   (TLS perdu ; le jeu continue en UDP si la partie tourne)' }
                }
            }
            $offset = $lines.Count
        }

        # The engine's own diagnostic only exists once a game process starts.
        $native = Join-Path $current.FullName 'native.txt'
        foreach ($name in 'native.txt', 'native.txt.desync') {
            $path = Join-Path $current.FullName $name
            if (Test-Path -LiteralPath $path) {
                $size = (Get-Item -LiteralPath $path).Length
                if ($name -eq 'native.txt' -and $size -ne $lastNative) {
                    if ($lastNative -eq 0) { Say "   >>> le jeu ecrit son diagnostic ($name)" }
                    $lastNative = $size
                }
                if ($name -eq 'native.txt.desync') {
                    Say "   >>> DESYNC ENREGISTREE : $(Get-Content $path -Raw)"
                }
            }
        }
    }

    if (Test-Path -LiteralPath $crashes) {
        foreach ($f in Get-ChildItem $crashes -File -ErrorAction SilentlyContinue) {
            if (-not $knownCrashes.ContainsKey($f.Name)) {
                $knownCrashes[$f.Name] = $true
                Say "   >>> NOUVEAU RAPPORT DE PLANTAGE : $($f.Name)"
            }
        }
    }

    Start-Sleep -Milliseconds $PollMs
}

Say ''
Say '=== fin de la surveillance ==='
if ($pings.Count) {
    $sorted = $pings | Sort-Object
    Say ("ping : {0} mesures, min {1} ms, median {2} ms, max {3} ms" -f
        $pings.Count, $sorted[0], $sorted[[int]($sorted.Count / 2)], $sorted[-1])
}
if ($current) { Say "dernier dossier : $($current.FullName)" }
exit 0
