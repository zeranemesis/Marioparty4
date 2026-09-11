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
#
# This script reports FOUR separate verdicts and never collapses them into one:
#
#   DETERMINISM     the two peers computed the same state
#   STABILITY       neither process disappeared
#   USER_TERMINATED the final shutdown was the one the user asked for
#   OVERALL         all three of the above
#
# The script's own exit code is NOT the game's exit code. Each instance's real
# exit code is captured and classified; a clean run of this script around a game
# that crashed is a FAIL, which is exactly the mistake this reporting exists to
# make impossible.
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
    $process = [Diagnostics.Process]::Start($start)
    $peers += @{ Process = $process; Side = $side; Role = $role; Recording = $recording
        Out = $process.StandardOutput.ReadToEndAsync()
        StartTime = $process.StartTime
        ClosedBySupervisor = $false
        ObservedFirst = $false }
}

Write-Output "Host on UDP port $port. Two windows are open."
foreach ($peer in $peers) {
    $seat = if ($peer.Side -eq 0) { 'host, player 1' } else { 'client, player 2' }
    Write-Output ("  instance $($peer.Side) ($seat): PID $($peer.Process.Id), " +
        "profile-$($peer.Side), reports under $runPath")
}
Write-Output 'Play normally. Nothing in this script touches the menus.'
Write-Output 'When you are done, close either window; the other is closed with it.'
Write-Output "Session files: $runPath"

# One window closing ends the session; stop the other so both recordings close.
# Whichever is seen gone first is recorded, but the poll interval is 250 ms, so
# two deaths inside one interval cannot be ordered and must not be claimed to be.
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

    # The game writes its shutdown intent into the live state file on every
    # heartbeat and flushes it the moment a shutdown is requested.
    $livePath = Join-Path $runPath "live-state-peer-$side.txt"
    $peer.LiveState = $livePath
    $peer.ShutdownIntent = 'UNKNOWN'
    $peer.LastFrame = 'unknown'
    $peer.LastOverlay = 'unknown'
    $peer.LastHash = 'unknown'
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
    }

    $peer.Fault = Get-FaultRecord $peer.Process.Id $peer.StartTime $peer.ExitTime

    # Classification, most specific cause first.
    if ($peer.Fault -or $peer.CrashReports.Count -gt 0 -or (Test-IsNtStatus $peer.ExitCode)) {
        $peer.Classification = 'PROCESS_CRASH'
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
$determinismProblems = @()
$recorded = @()
foreach ($peer in $peers) {
    if (-not (Test-Path -LiteralPath $peer.Recording)) {
        $determinismProblems += "Peer $($peer.Side) wrote no input recording."
        $recorded += , @()
    } else {
        $recorded += , @(Get-Content -LiteralPath $peer.Recording)
    }
}
$shared = 0
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

$overlays = @(, @(), @())
foreach ($peer in $peers) {
    $side = $peer.Side
    $logPath = Join-Path $runPath "peer-$side-stdout.log"
    if (-not (Test-Path -LiteralPath $logPath)) { continue }
    $log = Get-Content -LiteralPath $logPath -Raw
    $overlays[$side] = @([regex]::Matches($log, 'game context (-?\d+) at network frame (\d+)') |
        ForEach-Object { "$($_.Groups[1].Value)@$($_.Groups[2].Value)" })
}
if ($overlays[0].Count -gt 0 -and $overlays[1].Count -gt 0) {
    if (Compare-Object $overlays[0] $overlays[1] -SyncWindow 0) {
        $determinismProblems += ("The two instances took different overlay paths. " +
            "instance 0: $($overlays[0] -join ' ') | instance 1: $($overlays[1] -join ' ')")
    }
}

# The recording is kept even when the session failed: a crash worth reproducing
# is worth replaying, and the shared prefix is valid up to the point of failure.
$target = Resolve-TestPath $Output
if ($shared -ge 600 -and $determinismProblems.Count -eq 0) {
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    [IO.File]::WriteAllLines($target, $recorded[0][0..($shared - 1)])
    [IO.File]::WriteAllLines("$target.overlays", $overlays[0])
} elseif ($shared -gt 0) {
    $target = Join-Path $runPath 'partial-recording.txt'
    [IO.File]::WriteAllLines($target, $recorded[0][0..($shared - 1)])
}

$crashed = @($peers | Where-Object { $_.Classification -eq 'PROCESS_CRASH' })
$unexplained = @($peers | Where-Object { $_.Classification -eq 'UNKNOWN_ABNORMAL_EXIT' })
$userAsked = @($peers | Where-Object { $_.Classification -eq 'USER_REQUESTED_EXIT' })

$determinism = if ($determinismProblems.Count -eq 0 -and $shared -ge 600) { 'PASS' } else { 'FAIL' }
$stability = if ($crashed.Count -eq 0 -and $unexplained.Count -eq 0) { 'PASS' } else { 'FAIL' }
$userTerminated = if ($userAsked.Count -gt 0) { 'YES' } else { 'NO' }
$overall = if ($determinism -eq 'PASS' -and $stability -eq 'PASS' -and $userTerminated -eq 'YES') {
    'PASS'
} else { 'FAIL' }

# Session summary, written whether or not anything went wrong.
$summaryPath = Join-Path $runPath 'session-crash-summary.txt'
$summary = New-Object Collections.Generic.List[string]
$summary.Add('PARTYBOARD_SESSION_SUMMARY version=1')
$summary.Add("session_directory=$runPath")
$summary.Add("udp_port=$port")
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
    $summary.Add('')
}

$summary.Add('[ORDER OF TERMINATION]')
if ($observedSimultaneously) {
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
$summary.Add("DETERMINISM: $determinism")
$summary.Add("STABILITY: $stability")
$summary.Add("USER_TERMINATED: $userTerminated")
$summary.Add("OVERALL: $overall")
foreach ($problem in $determinismProblems) { $summary.Add("determinism_problem=$problem") }
[IO.File]::WriteAllLines($summaryPath, $summary)

Write-Output ''
if ($shared -gt 0) {
    Write-Output "Both instances agreed on $shared recorded rows (about $([int]($shared / 120)) seconds of play)."
}
if ($overlays[0].Count -gt 0) {
    Write-Output "Overlay transitions: $($overlays[0].Count)"
    Write-Output ("Overlay path: " + ($overlays[0] -join " -> "))
}
foreach ($peer in $peers) {
    Write-Output ("peer-$($peer.Side) exit_code=$(Format-ExitCode $peer.ExitCode) " +
        "classification=$($peer.Classification) last_frame=$($peer.LastFrame)")
}
foreach ($problem in $determinismProblems) { Write-Output "DETERMINISM PROBLEM: $problem" }
Write-Output ''
Write-Output "DETERMINISM: $determinism"
Write-Output "STABILITY: $stability"
Write-Output "USER_TERMINATED: $userTerminated"
Write-Output "OVERALL: $overall"
Write-Output ''
Write-Output "Session summary: $summaryPath"
if (Test-Path -LiteralPath $target) { Write-Output "Recording: $target" }
foreach ($peer in $peers) {
    foreach ($report in $peer.CrashReports) { Write-Output "Crash report: $report" }
    foreach ($dump in $peer.Minidumps) { Write-Output "Minidump: $dump" }
}
if ($overall -ne 'PASS') {
    Write-Output ''
    Write-Output 'This session is NOT a pass. A session passes only when the peers stayed in sync,'
    Write-Output 'neither process disappeared, and the final shutdown was the one you asked for.'
    exit 1
}
Write-Output ''
Write-Output 'Replay it with:'
Write-Output "  tools\test_netplay_boot.ps1 -DiscPath ""$disc"" -ReplayInput ""$Output"""
exit 0
