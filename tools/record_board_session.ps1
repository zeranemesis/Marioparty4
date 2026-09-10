# Records a human-played online session so it can be replayed as a regression
# test. Launches two visible instances on this machine, both reading the same
# physical controller port by default, which lets one person confirm both seats
# on the selection screens and reach a board.
#
#   tools\record_board_session.ps1 -DiscPath "..\Mario Party 4 (USA) (Rev 1).iso"
#
# Play until you are on a board and have taken a few turns, then close either
# window. The script keeps the frames both peers agreed on and writes the
# recording plus the overlay path it produced. Replay it with:
#
#   tools\test_netplay_boot.ps1 -DiscPath <iso> -ReplayInput work\netplay-recordings\board.txt
#
# A recording is tied to the build it was made with: any change to
# determinism-affecting behaviour invalidates it and it must be re-recorded.
param(
    [Parameter(Mandatory = $true)][string]$DiscPath,
    [string]$BinaryDirectory = 'build/aexp/RelWithDebInfo',
    [string]$Output = 'work/netplay-recordings/board.txt',
    [int]$HostPad = 1,
    [int]$ClientPad = 1
)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
function Resolve-TestPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}
$binaryPath = Resolve-TestPath $BinaryDirectory
$disc = [IO.Path]::GetFullPath($DiscPath)
if (-not (Test-Path -LiteralPath $disc -PathType Leaf)) { throw "Disc file missing: $disc" }

$runPath = Join-Path (Resolve-TestPath 'work/netplay-recordings') ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runPath -Force | Out-Null

# Reserve an ephemeral port, then release it for the host to bind.
$reservation = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
$port = $reservation.Client.LocalEndPoint.Port
$reservation.Dispose()

$peers = @()
foreach ($side in 0, 1) {
    # Each instance gets its own preferences and memory card, so a recording
    # session never touches your normal offline save.
    $profile = Join-Path $runPath "profile-$side"
    New-Item -ItemType Directory -Path $profile | Out-Null
    $settings = @{
        'backend.isoPath'            = $disc
        'backend.isoVerification'    = 2
        'backend.wasPresetChosen'    = $true
        'backend.skipPreLaunchUI'    = $true
        'video.targetFrameRate'      = 60
    }
    # Windows PowerShell 5.1 has no utf8NoBOM encoding name, and the config
    # parser rejects a byte-order mark.
    [IO.File]::WriteAllText((Join-Path $profile 'config.json'),
        ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))

    # Bring the player's own input bindings across, or the isolated profile has
    # no controller mapping and the game cannot be driven. Memory cards are
    # deliberately NOT copied: an online session forces a fresh save anyway, and
    # the personal card must stay untouched.
    $realProfile = Join-Path $env:APPDATA 'MarioPartyRD\Party Board'
    if (Test-Path -LiteralPath $realProfile) {
        foreach ($pattern in '*.controller', 'keyboard_bindings.dat', 'controller_ports.dat') {
            Get-ChildItem -LiteralPath $realProfile -File -Filter $pattern -ErrorAction SilentlyContinue |
                ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $profile }
        }
    }

    $pad = if ($side -eq 0) { $HostPad } else { $ClientPad }
    $net = if ($side -eq 0) { "--netplay-host $port" } else { "--netplay-join 127.0.0.1:$port" }
    $recording = Join-Path $runPath "peer-$side-input.txt"

    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $binaryPath 'partyboard.exe'
    $start.WorkingDirectory = $binaryPath
    $start.Arguments = "$net --netplay-full --netplay-pad $pad " +
        '--netplay-record-input ' + [char]34 + $recording + [char]34
    $start.UseShellExecute = $false
    # Captured so the overlay path can be extracted with the same pattern the
    # replay test uses; without it a human recording cannot become a regression.
    $start.RedirectStandardOutput = $true
    $start.EnvironmentVariables['PARTYBOARD_NETPLAY_TEST_PROFILE'] = $profile
    $start.EnvironmentVariables['PARTYBOARD_NET_DIAGNOSTIC'] = Join-Path $runPath "peer-$side-native.log"
    $process = [Diagnostics.Process]::Start($start)
    $peers += @{ Process = $process; Side = $side; Recording = $recording
        Out = $process.StandardOutput.ReadToEndAsync() }
}

Write-Output "Host on UDP port $port. Two windows are open."
foreach ($peer in $peers) {
    $seat = if ($peer.Side -eq 0) { 'host, player 1' } else { 'client, player 2' }
    Write-Output ("  instance $($peer.Side) ($seat): PID $($peer.Process.Id), " +
        "profile-$($peer.Side), reports under profile-$($peer.Side)\netplay")
}
Write-Output 'Play normally. Nothing in this script touches the menus.'
Write-Output 'When you are done, close either window; the other is closed with it.'
Write-Output "Session files: $runPath"

# One window closing ends the session; stop the other so both recordings close.
while ($true) {
    $exited = $peers | Where-Object { $_.Process.HasExited }
    if ($exited) { break }
    Start-Sleep -Milliseconds 250
}
foreach ($peer in $peers) {
    if (-not $peer.Process.HasExited) {
        $peer.Process.CloseMainWindow() | Out-Null
        if (-not $peer.Process.WaitForExit(10000)) { $peer.Process.Kill() }
    }
    $peer.Process.WaitForExit()
    # One log file per instance, kept beside its own diagnostic and reports.
    [IO.File]::WriteAllText((Join-Path $runPath "peer-$($peer.Side)-stdout.log"), $peer.Out.Result)
}

$recorded = @()
foreach ($peer in $peers) {
    if (-not (Test-Path -LiteralPath $peer.Recording)) {
        throw "Peer $($peer.Side) wrote no input recording."
    }
    $recorded += , @(Get-Content -LiteralPath $peer.Recording)
}
$shared = [Math]::Min($recorded[0].Count, $recorded[1].Count)
if ($shared -lt 600) {
    throw "Session too short to be useful: only $shared shared rows (about $([int]($shared / 120)) seconds)."
}
# Both peers record both seats, so everything they both reached must match.
if (Compare-Object $recorded[0][0..($shared - 1)] $recorded[1][0..($shared - 1)] -SyncWindow 0) {
    throw "The two peers disagreed within their first $shared recorded rows; the session desynced."
}

$target = Resolve-TestPath $Output
New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
[IO.File]::WriteAllLines($target, $recorded[0][0..($shared - 1)])

# The overlay path this session produced becomes the expectation for replays.
# Same pattern the replay check uses, so a human recording is a regression test
# and not just a saved session. Each transition is kept with its frame: the
# frame is the part that a timing regression would move.
$overlays = @()
foreach ($side in 0, 1) {
    $log = Get-Content -LiteralPath (Join-Path $runPath "peer-$side-stdout.log") -Raw
    $seen = [regex]::Matches($log, 'game context (-?\d+) at network frame (\d+)') |
        ForEach-Object { "$($_.Groups[1].Value)@$($_.Groups[2].Value)" }
    $overlays += , @($seen)
}
if (Compare-Object $overlays[0] $overlays[1] -SyncWindow 0) {
    throw "The two instances took different overlay paths. instance 0: $($overlays[0] -join ' ') | instance 1: $($overlays[1] -join ' ')"
}
[IO.File]::WriteAllLines("$target.overlays", $overlays[0])

Write-Output ''
Write-Output "PASS: both instances agreed on $shared recorded rows (about $([int]($shared / 120)) seconds of play)."
Write-Output "PASS: both instances crossed $($overlays[0].Count) overlay transitions on identical frames."
Write-Output ("Overlay path: " + ($overlays[0] -join " -> "))
Write-Output "Recording saved to $target"
Write-Output ''
Write-Output 'Replay it with:'
Write-Output "  tools\test_netplay_boot.ps1 -DiscPath ""$disc"" -ReplayInput ""$Output"""
