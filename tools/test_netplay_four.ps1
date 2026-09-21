# Four real instances of the game on one machine, seated 0..3, talking directly.
#
# Everything else about four players has been checked a piece at a time: the
# transport seats four, the readiness predicate waits for all of them, the salon
# hands out seats. None of that says a four-player session runs. This does, by
# running one -- and it is possible at all because loopback has no NAT, so the
# star that exists to survive consumer routers is not needed to find out whether
# the seats themselves work.
#
# What it does not cover: the relay, the TLS salon, and the Internet. A pass here
# means the engine seats four and agrees on every frame, not that four people in
# four houses can play.
#
# Where it stands, 2026-09-18: -Players 2, 3 and 4 all pass. 2400 frames, every
# seat verified on every frame, every peer's canonical state stream agreeing in
# both directions at the end.
#
# Every piece of four-player support passed its own test before this script
# existed, and the whole still did not: it found an InvalidAck at frame 3 from
# a shared state stream, then a seed authority that only seat 1 honoured. That
# gap is the reason it exists, and the reason it stays.

param(
    [string]$Exe = "",
    [int]$Frames = 900,
    [int]$Players = 4
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (-not $Exe) {
    # The build output, not build/install: install is written by a separate step
    # and has been a day stale before now, which turned two earlier measurements
    # in this area into measurements of yesterday's code.
    $Exe = Join-Path $root 'build\windows-msvc-relwithdebinfo\partyboard.exe'
}
if (-not (Test-Path -LiteralPath $Exe)) { throw "No partyboard.exe at '$Exe'. Build first." }
if ($Players -lt 2 -or $Players -gt 4) { throw "Players must be between 2 and 4." }

# One UDP port per seat, reserved then released so each instance can bind it.
$ports = @()
$held = @()
try {
    for ($i = 0; $i -lt $Players; $i++) {
        $socket = New-Object System.Net.Sockets.UdpClient([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
        $held += $socket
        $ports += ([System.Net.IPEndPoint]$socket.Client.LocalEndPoint).Port
    }
} finally {
    foreach ($socket in $held) { $socket.Close() }
}

Write-Output ("Seats {0}, ports {1}" -f $Players, ($ports -join ', '))

$processes = @()
$logs = @()
try {
    for ($seat = 0; $seat -lt $Players; $seat++) {
        # Every instance hosts its own socket and is handed the others directly.
        # No discovery, no relay: the point is to test the seats, not the finding.
        $arguments = @(
            '--netplay-host', $ports[$seat],
            '--netplay-loopback',
            '--netplay-players', $Players,
            '--netplay-seat', $seat,
            # The pad probe runs the netplay loop and returns without booting the
            # game, so this needs no disc and no start barrier: it exercises the
            # seats, which is the only thing in question.
            '--netplay-pad-probe',
            # The probe refuses to run without it (netplay_runtime.cpp:2974): it
            # drives the full-game input path, which is the path under test.
            '--netplay-full',
            '--netplay-delay', '3'
        )
        for ($other = 0; $other -lt $Players; $other++) {
            if ($other -eq $seat) { continue }
            $arguments += @('--netplay-peer', ("{0}:127.0.0.1:{1}" -f $other, $ports[$other]))
        }

        $log = Join-Path ([IO.Path]::GetTempPath()) ("partyboard-four-{0}-{1}.log" -f $seat, [Guid]::NewGuid().ToString('N'))
        $logs += $log
        # Not Start-Process: with redirection it hands back a process object whose
        # ExitCode stays empty, which read as a failure on a run that had passed.
        $info = New-Object Diagnostics.ProcessStartInfo
        $info.FileName = $Exe
        # ArgumentList is .NET Core only, and this runs under Windows PowerShell.
        # No argument here contains a space, so joining is exact.
        $info.Arguments = (($arguments | ForEach-Object { [string]$_ }) -join ' ')
        $info.UseShellExecute = $false
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.WorkingDirectory = Split-Path $Exe -Parent
        $process = [Diagnostics.Process]::Start($info)
        # Drained on background tasks: a full pipe buffer deadlocks the child.
        $process.StandardOutput.ReadToEndAsync() | Out-Null
        $process.StandardError.ReadToEndAsync() | Out-Null
        $processes += $process
    }

    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.Elapsed.TotalSeconds -lt 180) {
        if (-not ($processes | Where-Object { -not $_.HasExited })) { break }
        Start-Sleep -Milliseconds 250
    }

    $failed = $false
    for ($seat = 0; $seat -lt $Players; $seat++) {
        $process = $processes[$seat]
        if (-not $process.HasExited) {
            Write-Output ("Seat {0}: still running after 180 s" -f $seat)
            try { $process.Kill() } catch { }
            $failed = $true
            continue
        }
        # ExitCode stays empty on a PassThru process until the handle is waited on,
        # which made a passing run look like a failing one.
        $process.WaitForExit()
        Write-Output ("Seat {0}: exit {1}" -f $seat, $process.ExitCode)
        if ($process.ExitCode -ne 0) { $failed = $true }
    }

    if ($failed) { throw "A seat did not finish cleanly; see the output above." }
    Write-Output ("Four-seat loopback session: PASS ({0} seats, direct peers, no relay)." -f $Players)
} finally {
    foreach ($process in $processes) { try { if (-not $process.HasExited) { $process.Kill() } } catch { } }
    foreach ($log in $logs) {
        foreach ($path in @($log, $log + '.err')) {
            try { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force } } catch { }
        }
    }
}
