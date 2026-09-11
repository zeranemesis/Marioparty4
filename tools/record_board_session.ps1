# Records a human-played online session so it can be replayed as a regression
# test.
#
# THREE ROLES.
#
#   -Role Local   (default) two visible instances on this machine, as before.
#   -Role Host    one instance on this machine, listening; the other player
#                 connects from their own machine.
#   -Role Join    one instance on this machine, connecting to their host.
#
# Local is unchanged and is still the right thing for a quick rehearsal. Host
# and Join exist because the two-machine session is the one property no local
# loopback run can establish, and until now the tool could not record it: it
# hardcoded 127.0.0.1 and supervised both processes itself, so an hour of real
# two-machine play produced an anecdote rather than a regression test.
#
#   Machine A:  tools\record_board_session.ps1 -DiscPath <iso> -Role Host
#   Machine B:  tools\record_board_session.ps1 -DiscPath <iso> -Role Join -JoinAddress A.B.C.D:PORT
#
# Each side supervises only its own process, classifies only its own
# termination, and writes its own session folder with a machine-readable
# session.json. Afterwards, bring the two folders together and run:
#
#   tools\merge_session.ps1 -HostSession <folderA> -JoinSession <folderB>
#
# which performs the cross-peer determinism comparison that a single machine
# cannot do alone.
#
# A recording is tied to the build it was made with: any change to
# determinism-affecting behaviour invalidates it and it must be re-recorded.
#
# This script reports FIVE separate verdicts and never collapses them into one:
#
#   DETERMINISM     the two peers computed the same state
#                   (DEFERRED in Host/Join - only the merge can decide it)
#   STABILITY       neither process disappeared
#   USER_TERMINATED the final shutdown was the one the user asked for
#   PLAYABILITY     the session was not spent waiting for the other machine
#   OVERALL         all of the above
#
# PLAYABILITY exists because the first four can all be green on a session no
# human would sit through. Lockstep waits for the remote input; on loopback that
# wait is nil and on two real networks it is not. Its thresholds are in
# docs/playability_thresholds.md and were fixed BEFORE the first two-machine
# session, so the first measurement cannot become a negotiation.
#
# The script's own exit code is NOT the game's exit code. Each instance's real
# exit code is captured and classified; a clean run of this script around a game
# that crashed is a FAIL, which is exactly the mistake this reporting exists to
# make impossible.
param(
    [Parameter(Mandatory = $true)][string]$DiscPath,
    [ValidateSet('Local', 'Host', 'Join')][string]$Role = 'Local',
    # Host: the UDP port to listen on. 0 picks a free one and prints it.
    [int]$Port = 0,
    # Join: where the other machine is listening, as address:port.
    [string]$JoinAddress = '',
    [string]$BinaryDirectory = 'build/aexp/RelWithDebInfo',
    [string]$Output = 'work/netplay-recordings/board.txt',
    [int]$HostPad = 1,
    [int]$ClientPad = 1,
    # A name for this session folder, so the two machines' folders can be told
    # apart once they are copied together.
    [string]$Label = ''
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
if ($Role -eq 'Join' -and [string]::IsNullOrWhiteSpace($JoinAddress)) {
    throw "-Role Join needs -JoinAddress <address:port>, the value the host machine printed."
}
if ($Role -eq 'Join' -and $JoinAddress -notmatch '^[^:]+:\d+$') {
    throw "-JoinAddress must be address:port, for example 203.0.113.9:52341. Got '$JoinAddress'."
}

# Which seats this machine runs. Local runs both; Host runs seat 0; Join seat 1.
$sides = switch ($Role) {
    'Local' { @(0, 1) }
    'Host'  { @(0) }
    'Join'  { @(1) }
}

$stamp = (Get-Date).ToString('yyyy-MM-dd_HHmmss')
$suffix = if ($Label) { "$stamp-$($Role.ToLower())-$Label" } else { "$stamp-$($Role.ToLower())" }
$runPath = Join-Path (Resolve-TestPath 'work/netplay-recordings') $suffix
New-Item -ItemType Directory -Path $runPath -Force | Out-Null

if ($Role -eq 'Join') {
    $port = [int]($JoinAddress -split ':')[1]
} elseif ($Port -gt 0) {
    $port = $Port
} else {
    # Reserve an ephemeral port, then release it for the host to bind. Bound on
    # Any rather than Loopback: a host the other machine must reach cannot be
    # chosen from the loopback range.
    $bindAddress = if ($Role -eq 'Local') { [Net.IPAddress]::Loopback } else { [Net.IPAddress]::Any }
    $reservation = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new($bindAddress, 0))
    $port = $reservation.Client.LocalEndPoint.Port
    $reservation.Dispose()
}

# Named Windows status codes, so a report says STATUS_HEAP_CORRUPTION rather
# than only 0xC0000374. Matches the table in src/port/crash_report.cpp.
$exceptionNames = @{
    0xC0000005 = 'EXCEPTION_ACCESS_VIOLATION'
    0xC0000006 = 'EXCEPTION_IN_PAGE_ERROR'
    0xC000001D = 'EXCEPTION_ILLEGAL_INSTRUCTION'
    0xC000008C = 'EXCEPTION_ARRAY_BOUNDS_EXCEEDED'
    0xC0000094 = 'EXCEPTION_INT_DIVIDE_BY_ZERO'
    0xC0000096 = 'EXCEPTION_PRIV_INSTRUCTION'
    0xC00000FD = 'EXCEPTION_STACK_OVERFLOW'
    0xC0000374 = 'STATUS_HEAP_CORRUPTION'
    0xC0000409 = 'STATUS_STACK_BUFFER_OVERRUN'
    0xC0000602 = 'STATUS_FAIL_FAST_EXCEPTION'
    0xC000013A = 'STATUS_CONTROL_C_EXIT'
}
function Format-ExitCode([int]$code) {
    $unsigned = [uint32]([uint32]::MaxValue -band $code)
    $name = $exceptionNames[[int64]$unsigned]
    $text = ('0x{0:X8} ({1})' -f $unsigned, $code)
    if ($name) { return "$text $name" }
    return $text
}
# A non-zero exit code that sits in the 0xC0000000 range is an NT status, which
# means the process was killed by an exception rather than returning a value.
function Test-IsNtStatus([int]$code) {
    $unsigned = [uint32]([uint32]::MaxValue -band $code)
    return $unsigned -ge 0xC0000000
}

$peers = @()
foreach ($side in $sides) {
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
    $net = if ($side -eq 0) {
        "--netplay-host $port"
    } elseif ($Role -eq 'Join') {
        "--netplay-join $JoinAddress"
    } else {
        "--netplay-join 127.0.0.1:$port"
    }
    $role = if ($side -eq 0) { 'host' } else { 'client' }
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
    # Crash reports, minidumps and the live state file land in the session
    # folder, named per seat. The previous handler wrote a fixed filename in the
    # working directory, so two instances overwrote each other's evidence.
    $start.EnvironmentVariables['PARTYBOARD_CRASH_DIR'] = $runPath
    $start.EnvironmentVariables['PARTYBOARD_CRASH_PEER'] = "$side"
    $start.EnvironmentVariables['PARTYBOARD_CRASH_ROLE'] = $role
    # The detector that could have explained the heap corruption that ended
    # session S1. A human session is the most expensive evidence this project
    # collects; running it with the instrument switched off wastes it.
    $start.EnvironmentVariables['PARTYBOARD_MEM_DIAGNOSTICS'] = '1'
    $start.EnvironmentVariables['PARTYBOARD_AUDIO_DIAGNOSTICS'] = '1'
    $process = [Diagnostics.Process]::Start($start)
    $peers += @{ Process = $process; Side = $side; Role = $role; Recording = $recording
        Out = $process.StandardOutput.ReadToEndAsync()
        StartTime = $process.StartTime
        ClosedBySupervisor = $false
        ObservedFirst = $false }
}

switch ($Role) {
    'Local' {
        Write-Output "Host on UDP port $port. Two windows are open."
    }
    'Host' {
        Write-Output "Listening on UDP port $port."
        Write-Output ''
        Write-Output 'Give the other machine this command (substitute your own address):'
        $addresses = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
            ForEach-Object { $_.IPAddress })
        foreach ($address in $addresses) {
            Write-Output "  tools\record_board_session.ps1 -DiscPath <iso> -Role Join -JoinAddress ${address}:$port"
        }
        if ($addresses.Count -eq 0) {
            Write-Output "  tools\record_board_session.ps1 -DiscPath <iso> -Role Join -JoinAddress <this-machine>:$port"
        }
        Write-Output ''
        Write-Output "Those are this machine's local addresses. Across the internet the other"
        Write-Output "machine needs your public address and UDP $port forwarded to this machine."
    }
    'Join' {
        Write-Output "Connecting to $JoinAddress."
    }
}
foreach ($peer in $peers) {
    $seat = if ($peer.Side -eq 0) { 'host, player 1' } else { 'client, player 2' }
    Write-Output ("  instance $($peer.Side) ($seat): PID $($peer.Process.Id), " +
        "profile-$($peer.Side), reports under $runPath")
}
Write-Output 'Play normally. Nothing in this script touches the menus.'
if ($Role -eq 'Local') {
    Write-Output 'When you are done, close either window; the other is closed with it.'
} else {
    Write-Output 'When you are done, close the window. The other machine records its own side.'
}
Write-Output "Session files: $runPath"

# Local: one window closing ends the session; stop the other so both recordings
# close. Whichever is seen gone first is recorded, but the poll interval is
# 250 ms, so two deaths inside one interval cannot be ordered and must not be
# claimed to be.
#
# Host/Join: this machine supervises ONE process and has no business terminating
# anything on the other machine. The other side ends its own session and its own
# recording, which is precisely why the determinism comparison has to move to a
# merge step performed afterwards on both folders.
$pollMilliseconds = 250
while ($true) {
    $exited = @($peers | Where-Object { $_.Process.HasExited })
    if ($exited.Count -gt 0) {
        foreach ($peer in $exited) { $peer.ObservedFirst = $true }
        break
    }
    Start-Sleep -Milliseconds $pollMilliseconds
}
$observedSimultaneously = (@($peers | Where-Object { $_.ObservedFirst })).Count -gt 1

foreach ($peer in $peers) {
    if (-not $peer.Process.HasExited) {
        # This peer is still alive, so its termination is the supervisor's doing
        # and must never be counted against the game.
        $peer.ClosedBySupervisor = $true
        $peer.Process.CloseMainWindow() | Out-Null
        if (-not $peer.Process.WaitForExit(10000)) { $peer.Process.Kill() }
    }
    $peer.Process.WaitForExit()
    $peer.ExitCode = $peer.Process.ExitCode
    $peer.ExitTime = $peer.Process.ExitTime
    # One log file per instance, kept beside its own diagnostic and reports.
    [IO.File]::WriteAllText((Join-Path $runPath "peer-$($peer.Side)-stdout.log"), $peer.Out.Result)
}

# Windows raises STATUS_HEAP_CORRUPTION through __fastfail, which bypasses both
# SetUnhandledExceptionFilter and vectored handlers by design, so the game cannot
# always describe its own death. The event log can, from out of process.
function Get-FaultRecord([int]$processId, [datetime]$from, [datetime]$to) {
    try {
        $events = Get-WinEvent -FilterHashtable @{
            LogName = 'Application'; ProviderName = 'Application Error'; Id = 1000
            StartTime = $from.AddSeconds(-5); EndTime = $to.AddSeconds(30)
        } -ErrorAction Stop
    } catch { return $null }
    foreach ($event in $events) {
        # Field 8 of this event is the faulting process id, in hex.
        $text = $event.Message
        $match = [regex]::Match($text, '(?:process id|ID du processus)[^:]*:\s*0x([0-9A-Fa-f]+)')
        if (-not $match.Success) { continue }
        if ([Convert]::ToInt32($match.Groups[1].Value, 16) -ne $processId) { continue }
        $code = [regex]::Match($text, 'Exception code:\s*0x([0-9A-Fa-f]+)')
        $module = [regex]::Match($text, '(?:faulting module name|Nom du module défaillant)[^:]*:\s*([^\r\n,]+)')
        $offset = [regex]::Match($text, 'Fault offset:\s*0x([0-9A-Fa-f]+)')
        return @{
            Time = $event.TimeCreated
            Code = if ($code.Success) { $code.Groups[1].Value } else { 'unknown' }
            Module = if ($module.Success) { $module.Groups[1].Value.Trim() } else { 'unknown' }
            Offset = if ($offset.Success) { $offset.Groups[1].Value } else { 'unknown' }
        }
    }
    return $null
}

# A termination counts as normal ONLY with explicit evidence that it was
# expected. Absence of evidence means abnormal.
foreach ($peer in $peers) {
    $side = $peer.Side
    $peer.CrashReports = @(Get-ChildItem -LiteralPath $runPath -Filter "crash-report-peer-$side-*.txt" `
        -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    $peer.Minidumps = @(Get-ChildItem -LiteralPath $runPath -Filter "crash-peer-$side-*.dmp" `
        -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    $peer.DesyncReports = @(Get-ChildItem -LiteralPath $runPath -Recurse -Filter 'netplay_desync_*.log' `
        -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    $peer.MemCorruption = @(Get-ChildItem -LiteralPath $runPath -Filter "mem-corruption-peer-$side-*.txt" `
        -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })

    # The game writes its shutdown intent into the live state file on every
    # heartbeat and flushes it the moment a shutdown is requested.
    $livePath = Join-Path $runPath "live-state-peer-$side.txt"
    $peer.LiveState = $livePath
    $peer.ShutdownIntent = 'UNKNOWN'
    $peer.LastFrame = 'unknown'
    $peer.LastOverlay = 'unknown'
    $peer.LastHash = 'unknown'
    $peer.StalledTicks = $null
    $peer.LongestStall = $null
    if (Test-Path -LiteralPath $livePath) {
        $live = Get-Content -LiteralPath $livePath -Raw
        $m = [regex]::Match($live, 'shutdown_intent=(\S+)')
        if ($m.Success) { $peer.ShutdownIntent = $m.Groups[1].Value }
        $m = [regex]::Match($live, 'simulation_frame=(\d+)')
        if ($m.Success) { $peer.LastFrame = $m.Groups[1].Value }
        $m = [regex]::Match($live, 'game_context=(-?\d+) overlay=(-?\d+)')
        if ($m.Success) { $peer.LastOverlay = "context $($m.Groups[1].Value) overlay $($m.Groups[2].Value)" }
        $m = [regex]::Match($live, 'last_state_hash=([0-9a-f]+) at_frame=(\d+)')
        if ($m.Success) { $peer.LastHash = "$($m.Groups[1].Value)@$($m.Groups[2].Value)" }
        # Lockstep waiting. Zero on loopback; the whole point of measuring it is
        # that it will not be zero between two machines.
        $m = [regex]::Match($live, 'stalled_ticks=(\d+) longest_stall=(\d+)')
        if ($m.Success) {
            $peer.StalledTicks = [int]$m.Groups[1].Value
            $peer.LongestStall = [int]$m.Groups[2].Value
        }
    }

    $peer.Fault = Get-FaultRecord $peer.Process.Id $peer.StartTime $peer.ExitTime

    # Classification, most specific cause first.
    if ($peer.Fault -or $peer.CrashReports.Count -gt 0 -or (Test-IsNtStatus $peer.ExitCode)) {
        $peer.Classification = 'PROCESS_CRASH'
    } elseif ($peer.MemCorruption.Count -gt 0) {
        $peer.Classification = 'HEAP_CORRUPTION'
    } elseif ($peer.DesyncReports.Count -gt 0) {
        $peer.Classification = 'NETPLAY_DESYNC'
    } elseif ($peer.ClosedBySupervisor) {
        $peer.Classification = 'SUPERVISOR_TERMINATED'
    } elseif ($peer.ShutdownIntent -eq 'USER_REQUESTED_EXIT') {
        $peer.Classification = 'USER_REQUESTED_EXIT'
    } elseif ($peer.ShutdownIntent -eq 'NORMAL_GAME_EXIT') {
        $peer.Classification = 'NORMAL_GAME_EXIT'
    } elseif ($peer.ExitCode -eq 0) {
        # Exit code 0 with no recorded intent. Most likely the window was closed,
        # but nothing proves it, so it stays abnormal rather than being assumed.
        $peer.Classification = 'UNKNOWN_ABNORMAL_EXIT'
    } else {
        $peer.Classification = 'UNKNOWN_ABNORMAL_EXIT'
    }
}

# Determinism is assessed separately and must not abort before the stability
# verdict has been reported: a crashed session still has to say so clearly.
#
# In Host/Join this machine holds one seat's recording, so the comparison is not
# possible here and is NOT approximated. It is deferred to merge_session.ps1,
# which is given both folders. Saying DEFERRED is the honest answer; saying PASS
# on the strength of one side's data would be a claim about a machine this
# script never saw.
$determinismProblems = @()
$recorded = @{}
foreach ($peer in $peers) {
    if (-not (Test-Path -LiteralPath $peer.Recording)) {
        $determinismProblems += "Peer $($peer.Side) wrote no input recording."
        $recorded[$peer.Side] = @()
    } else {
        $recorded[$peer.Side] = @(Get-Content -LiteralPath $peer.Recording)
    }
}
$shared = 0
if ($Role -eq 'Local') {
    if ($recorded[0].Count -gt 0 -and $recorded[1].Count -gt 0) {
        $shared = [Math]::Min($recorded[0].Count, $recorded[1].Count)
        if ($shared -lt 600) {
            $determinismProblems += ("Session too short to be useful: only $shared shared rows " +
                "(about $([int]($shared / 120)) seconds).")
        } else {
            # Both peers record both seats, so everything they both reached must match.
            if (Compare-Object $recorded[0][0..($shared - 1)] $recorded[1][0..($shared - 1)] -SyncWindow 0) {
                $determinismProblems += "The two peers disagreed within their first $shared recorded rows."
            }
        }
    }
} else {
    $shared = $recorded[$sides[0]].Count
    if ($shared -lt 600) {
        $determinismProblems += ("Session too short to be useful: only $shared recorded rows " +
            "(about $([int]($shared / 120)) seconds).")
    }
}

$overlays = @{}
foreach ($peer in $peers) {
    $side = $peer.Side
    $overlays[$side] = @()
    $logPath = Join-Path $runPath "peer-$side-stdout.log"
    if (-not (Test-Path -LiteralPath $logPath)) { continue }
    $log = Get-Content -LiteralPath $logPath -Raw
    $overlays[$side] = @([regex]::Matches($log, 'game context (-?\d+) at network frame (\d+)') |
        ForEach-Object { "$($_.Groups[1].Value)@$($_.Groups[2].Value)" })
}
# ---- which board mechanics this session actually reached ----
# The 15-line checklist in the validation matrix IS the contents of
# src/game/board/. Reading it back from the game's own output is what stops the
# matrix being updated from someone's recollection of the evening.
$coverage = @{}
foreach ($side in $sides) {
    $logPath = Join-Path $runPath "peer-$side-stdout.log"
    if (-not (Test-Path -LiteralPath $logPath)) { continue }
    foreach ($hit in Select-String -Path $logPath -Pattern '^COVERAGE> (\S+) first reached at frame (\d+)' -ErrorAction SilentlyContinue) {
        $name = $hit.Matches[0].Groups[1].Value
        $frame = [int]$hit.Matches[0].Groups[2].Value
        if (-not $coverage.ContainsKey($name) -or $coverage[$name] -gt $frame) { $coverage[$name] = $frame }
    }
}
$coverageList = @($coverage.Keys | Sort-Object | ForEach-Object { "$_@$($coverage[$_])" })

if ($Role -eq 'Local' -and $overlays[0].Count -gt 0 -and $overlays[1].Count -gt 0) {
    if (Compare-Object $overlays[0] $overlays[1] -SyncWindow 0) {
        $determinismProblems += ("The two instances took different overlay paths. " +
            "instance 0: $($overlays[0] -join ' ') | instance 1: $($overlays[1] -join ' ')")
    }
}

# ---- PLAYABILITY ----
#
# Thresholds from docs/playability_thresholds.md, fixed before the first
# two-machine session. A stalled tick is a frame the simulation did NOT advance
# because the remote input for it had not arrived, so stalls translate directly
# into lost wall-clock time: 3600 stalls in a minute is a minute of nothing.
$playabilityGoodPerMinute = 180      # 3 s lost per minute, 5 % - unnoticed
$playabilityMaxPerMinute = 720       # 12 s per minute, 20 % - slow but finishable
$playabilityGoodLongest = 30         # half a second
$playabilityMaxLongest = 180         # three seconds; beyond this it reads as a hang
$playabilityNotes = @()
$stallRates = @()
$longestStalls = @()
foreach ($peer in $peers) {
    if ($null -eq $peer.StalledTicks) {
        $playabilityNotes += "Peer $($peer.Side) reported no stall counters; the live state file is missing or truncated."
        continue
    }
    $minutes = [Math]::Max(($peer.ExitTime - $peer.StartTime).TotalMinutes, 0.0001)
    $rate = [Math]::Round($peer.StalledTicks / $minutes, 1)
    $peer.StallsPerMinute = $rate
    $stallRates += $rate
    $longestStalls += $peer.LongestStall
    $playabilityNotes += ("Peer $($peer.Side): $($peer.StalledTicks) stalled ticks over " +
        "$([Math]::Round($minutes, 2)) min = $rate/min, longest single wait $($peer.LongestStall) frames " +
        "($([Math]::Round($peer.LongestStall / 60.0, 2)) s).")
}
if ($stallRates.Count -eq 0) {
    $playability = 'UNKNOWN'
} else {
    $worstRate = ($stallRates | Measure-Object -Maximum).Maximum
    $worstStall = ($longestStalls | Measure-Object -Maximum).Maximum
    if ($worstRate -gt $playabilityMaxPerMinute -or $worstStall -gt $playabilityMaxLongest) {
        $playability = 'FAIL'
    } elseif ($worstRate -gt $playabilityGoodPerMinute -or $worstStall -gt $playabilityGoodLongest) {
        $playability = 'MARGINAL'
    } else {
        $playability = 'PASS'
    }
}

# The recording is kept even when the session failed: a crash worth reproducing
# is worth replaying, and the shared prefix is valid up to the point of failure.
$target = Resolve-TestPath $Output
if ($Role -eq 'Local') {
    if ($shared -ge 600 -and $determinismProblems.Count -eq 0) {
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        [IO.File]::WriteAllLines($target, $recorded[0][0..($shared - 1)])
        [IO.File]::WriteAllLines("$target.overlays", $overlays[0])
    } elseif ($shared -gt 0) {
        $target = Join-Path $runPath 'partial-recording.txt'
        [IO.File]::WriteAllLines($target, $recorded[0][0..($shared - 1)])
    }
} else {
    # One machine's recording is not yet a regression test - the merge decides
    # whether the two agree - so it stays in the session folder under a name
    # that says which seat produced it.
    $target = Join-Path $runPath "seat-$($sides[0])-recording.txt"
    if ($shared -gt 0) { [IO.File]::WriteAllLines($target, $recorded[$sides[0]]) }
}

$crashed = @($peers | Where-Object { $_.Classification -eq 'PROCESS_CRASH' -or $_.Classification -eq 'HEAP_CORRUPTION' })
$unexplained = @($peers | Where-Object { $_.Classification -eq 'UNKNOWN_ABNORMAL_EXIT' })
$userAsked = @($peers | Where-Object { $_.Classification -eq 'USER_REQUESTED_EXIT' })

$determinism = if ($Role -ne 'Local') {
    if ($determinismProblems.Count -eq 0) { 'DEFERRED' } else { 'FAIL' }
} elseif ($determinismProblems.Count -eq 0 -and $shared -ge 600) { 'PASS' } else { 'FAIL' }
$stability = if ($crashed.Count -eq 0 -and $unexplained.Count -eq 0) { 'PASS' } else { 'FAIL' }
$userTerminated = if ($userAsked.Count -gt 0) { 'YES' } else { 'NO' }
$overall = if (($determinism -eq 'PASS' -or $determinism -eq 'DEFERRED') -and $stability -eq 'PASS' `
        -and $userTerminated -eq 'YES' -and $playability -ne 'FAIL') {
    if ($determinism -eq 'DEFERRED') { 'PENDING_MERGE' } else { 'PASS' }
} else { 'FAIL' }

# Session summary, written whether or not anything went wrong.
$summaryPath = Join-Path $runPath 'session-crash-summary.txt'
$summary = New-Object Collections.Generic.List[string]
$summary.Add('PARTYBOARD_SESSION_SUMMARY version=2')
$summary.Add("role=$Role")
$summary.Add("session_directory=$runPath")
$summary.Add("udp_port=$port")
if ($Role -eq 'Join') { $summary.Add("join_address=$JoinAddress") }
$summary.Add("disc=$disc")
$summary.Add("supervisor_poll_ms=$pollMilliseconds")
$summary.Add('')
foreach ($peer in $peers) {
    $side = $peer.Side
    $summary.Add("[PEER $side - $($peer.Role)]")
    $summary.Add("pid=$($peer.Process.Id)")
    $summary.Add("started_at=$($peer.StartTime.ToString('yyyy-MM-dd HH:mm:ss.fff'))")
    $summary.Add("exited_at=$($peer.ExitTime.ToString('yyyy-MM-dd HH:mm:ss.fff'))")
    $summary.Add("lifetime_seconds=$([math]::Round(($peer.ExitTime - $peer.StartTime).TotalSeconds, 3))")
    $summary.Add("exit_code=$(Format-ExitCode $peer.ExitCode)")
    $summary.Add("classification=$($peer.Classification)")
    $summary.Add("shutdown_intent_recorded_by_game=$($peer.ShutdownIntent)")
    $summary.Add("observed_exited_first=$($peer.ObservedFirst)")
    $summary.Add("closed_by_supervisor=$($peer.ClosedBySupervisor)")
    $summary.Add("last_simulation_frame=$($peer.LastFrame)")
    $summary.Add("last_context=$($peer.LastOverlay)")
    $summary.Add("last_state_hash=$($peer.LastHash)")
    $summary.Add("stalled_ticks=$($peer.StalledTicks)")
    $summary.Add("longest_stall_frames=$($peer.LongestStall)")
    $summary.Add("stalls_per_minute=$($peer.StallsPerMinute)")
    $summary.Add("recorded_rows=$($recorded[$side].Count)")
    $summary.Add("overlay_transitions=$($overlays[$side].Count)")
    if ($peer.Fault) {
        $summary.Add("fault_event_time=$($peer.Fault.Time.ToString('HH:mm:ss.fff'))")
        $summary.Add("fault_exception=0x$($peer.Fault.Code) $($exceptionNames[[int64]([Convert]::ToUInt32($peer.Fault.Code, 16))])")
        $summary.Add("fault_module=$($peer.Fault.Module)")
        $summary.Add("fault_offset=0x$($peer.Fault.Offset)")
    } else {
        $summary.Add('fault_event=none found in the Windows Application log')
    }
    $summary.Add("live_state_file=$($peer.LiveState)")
    foreach ($report in $peer.CrashReports) { $summary.Add("crash_report=$report") }
    foreach ($dump in $peer.Minidumps) { $summary.Add("minidump=$dump") }
    foreach ($report in $peer.DesyncReports) { $summary.Add("desync_report=$report") }
    foreach ($report in $peer.MemCorruption) { $summary.Add("mem_corruption_report=$report") }
    $summary.Add('')
}

$summary.Add('[ORDER OF TERMINATION]')
if ($peers.Count -lt 2) {
    $summary.Add(("This machine supervised one instance. The other machine's termination is " +
        "recorded in its own session folder and is NOT inferred here."))
} elseif ($observedSimultaneously) {
    $summary.Add(("Both peers were already gone when the supervisor next polled, " +
        "$pollMilliseconds ms apart at most. The real order is AMBIGUOUS and is not claimed."))
} else {
    $first = @($peers | Where-Object { $_.ObservedFirst })[0]
    $second = @($peers | Where-Object { -not $_.ObservedFirst })[0]
    $gap = [math]::Round(($second.ExitTime - $first.ExitTime).TotalMilliseconds, 1)
    $summary.Add("observed_first=peer $($first.Side) (pid $($first.Process.Id))")
    $summary.Add("observed_second=peer $($second.Side) (pid $($second.Process.Id))")
    $summary.Add("exit_time_difference_ms=$gap")
    if ($second.ClosedBySupervisor) {
        $summary.Add("peer $($second.Side) was closed BY THE SUPERVISOR after peer $($first.Side) disappeared")
    } else {
        $summary.Add("peer $($second.Side) exited on its own; the supervisor did not touch it")
    }
    # With a 250 ms poll, a sub-interval gap cannot establish causation.
    if ([math]::Abs($gap) -lt $pollMilliseconds -and -not $second.ClosedBySupervisor) {
        $summary.Add(("The gap is smaller than the supervisor's poll interval, so the order is " +
            "NOT reliable evidence of cause and effect."))
    }
}
$summary.Add('')
if ($crashed.Count -eq 2) {
    $codes = @($crashed | ForEach-Object { (Format-ExitCode $_.ExitCode) }) | Select-Object -Unique
    if ($codes.Count -eq 1) {
        $summary.Add(("CONCLUSION: both peers independently crashed on $($codes[0]) during the same " +
            "deterministic gameplay window. Each has its own fault record, so neither was " +
            "terminated by the other or by the supervisor."))
    } else {
        $summary.Add('CONCLUSION: both peers crashed independently, with different exception codes.')
    }
} elseif ($crashed.Count -eq 1) {
    $summary.Add("CONCLUSION: peer $($crashed[0].Side) crashed; see its crash report and minidump.")
} elseif ($unexplained.Count -gt 0) {
    $summary.Add(('CONCLUSION: a peer exited with no recorded intent and no fault record. ' +
        'Treated as abnormal until proven otherwise.'))
} else {
    $summary.Add('CONCLUSION: no abnormal termination detected.')
}
$summary.Add('')
$summary.Add('[BOARD COVERAGE]')
if ($coverageList.Count -gt 0) {
    $summary.Add("mechanics_reached=$($coverageList -join ' ')")
} else {
    $summary.Add('mechanics_reached=NONE - this session exercised no board mechanic at all')
}
$summary.Add('')
$summary.Add('[PLAYABILITY]')
foreach ($note in $playabilityNotes) { $summary.Add($note) }
$summary.Add(("thresholds: good <= $playabilityGoodPerMinute stalls/min and <= $playabilityGoodLongest frames; " +
    "fail above $playabilityMaxPerMinute stalls/min or $playabilityMaxLongest frames"))
$summary.Add('')
$summary.Add("DETERMINISM: $determinism")
$summary.Add("STABILITY: $stability")
$summary.Add("USER_TERMINATED: $userTerminated")
$summary.Add("PLAYABILITY: $playability")
$summary.Add("OVERALL: $overall")
foreach ($problem in $determinismProblems) { $summary.Add("determinism_problem=$problem") }
[IO.File]::WriteAllLines($summaryPath, $summary)

# Machine-readable twin of the above, so merge_session.ps1 never has to parse
# the human report. A session that has to be re-read by eye to be merged is a
# session that will be merged from memory.
$document = [ordered]@{
    schema = 2
    role = $Role
    label = $Label
    session_directory = $runPath
    udp_port = $port
    join_address = $JoinAddress
    disc = $disc
    build_commit = (& git -C $projectPath rev-parse HEAD 2>$null)
    build_tree_dirty = [bool](& git -C $projectPath status --porcelain 2>$null)
    started_at = ($peers[0].StartTime.ToString('o'))
    determinism = $determinism
    stability = $stability
    user_terminated = $userTerminated
    playability = $playability
    overall = $overall
    determinism_problems = @($determinismProblems)
    board_coverage = @($coverageList)
    playability_thresholds = [ordered]@{
        good_stalls_per_minute = $playabilityGoodPerMinute
        max_stalls_per_minute = $playabilityMaxPerMinute
        good_longest_stall_frames = $playabilityGoodLongest
        max_longest_stall_frames = $playabilityMaxLongest
    }
    seats = @(foreach ($peer in $peers) {
        [ordered]@{
            side = $peer.Side
            role = $peer.Role
            classification = $peer.Classification
            exit_code = $peer.ExitCode
            shutdown_intent = $peer.ShutdownIntent
            last_frame = $peer.LastFrame
            last_state_hash = $peer.LastHash
            lifetime_seconds = [math]::Round(($peer.ExitTime - $peer.StartTime).TotalSeconds, 3)
            stalled_ticks = $peer.StalledTicks
            longest_stall_frames = $peer.LongestStall
            stalls_per_minute = $peer.StallsPerMinute
            recorded_rows = $recorded[$peer.Side].Count
            recording = $peer.Recording
            overlay_path = @($overlays[$peer.Side])
            crash_reports = @($peer.CrashReports)
            minidumps = @($peer.Minidumps)
            desync_reports = @($peer.DesyncReports)
            mem_corruption_reports = @($peer.MemCorruption)
        }
    })
}
[IO.File]::WriteAllText((Join-Path $runPath 'session.json'),
    ($document | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))

Write-Output ''
if ($shared -gt 0) {
    if ($Role -eq 'Local') {
        Write-Output "Both instances agreed on $shared recorded rows (about $([int]($shared / 120)) seconds of play)."
    } else {
        Write-Output "This machine recorded $shared rows (about $([int]($shared / 120)) seconds of play)."
    }
}
foreach ($side in $sides) {
    if ($overlays[$side].Count -gt 0) {
        Write-Output "Overlay transitions (seat $side): $($overlays[$side].Count)"
        Write-Output ("Overlay path: " + ($overlays[$side] -join " -> "))
    }
}
foreach ($peer in $peers) {
    Write-Output ("peer-$($peer.Side) exit_code=$(Format-ExitCode $peer.ExitCode) " +
        "classification=$($peer.Classification) last_frame=$($peer.LastFrame)")
}
if ($coverageList.Count -gt 0) {
    Write-Output "Board mechanics reached: $($coverageList -join ' ')"
} else {
    Write-Output 'Board mechanics reached: NONE'
}
foreach ($note in $playabilityNotes) { Write-Output $note }
foreach ($problem in $determinismProblems) { Write-Output "DETERMINISM PROBLEM: $problem" }
Write-Output ''
Write-Output "DETERMINISM: $determinism"
Write-Output "STABILITY: $stability"
Write-Output "USER_TERMINATED: $userTerminated"
Write-Output "PLAYABILITY: $playability"
Write-Output "OVERALL: $overall"
Write-Output ''
Write-Output "Session summary: $summaryPath"
Write-Output "Session record:  $(Join-Path $runPath 'session.json')"
if (Test-Path -LiteralPath $target) { Write-Output "Recording: $target" }
foreach ($peer in $peers) {
    foreach ($report in $peer.CrashReports) { Write-Output "Crash report: $report" }
    foreach ($dump in $peer.Minidumps) { Write-Output "Minidump: $dump" }
    foreach ($report in $peer.MemCorruption) { Write-Output "Heap corruption report: $report" }
}

if ($Role -ne 'Local') {
    Write-Output ''
    Write-Output 'This is ONE side of a two-machine session. Determinism cannot be decided here.'
    Write-Output 'Copy this folder next to the other machine''s, then run:'
    $quoted = '"' + $runPath + '"'
    $mine = if ($Role -eq 'Host') { "-HostSession $quoted -JoinSession <the other folder>" }
            else { "-HostSession <the other folder> -JoinSession $quoted" }
    Write-Output "  tools\merge_session.ps1 $mine"
}

if ($overall -eq 'FAIL') {
    Write-Output ''
    Write-Output 'This session is NOT a pass. A session passes only when the peers stayed in sync,'
    Write-Output 'neither process disappeared, the final shutdown was the one you asked for, and the'
    Write-Output 'session was not spent waiting for the other machine.'
    exit 1
}
if ($overall -eq 'PENDING_MERGE') { exit 2 }
Write-Output ''
Write-Output 'Replay it with:'
Write-Output "  tools\test_netplay_boot.ps1 -DiscPath ""$disc"" -ReplayInput ""$Output"""
exit 0
