param(
    [Parameter(Mandatory=$true)][string]$DiscPath,
    [string]$BinaryDirectory='build/aexp/RelWithDebInfo',
    [string]$OutputDirectory='work/netplay-real-boot',
    [int]$DurationSeconds=30,
    [switch]$Menu,
    [switch]$Walk,
    [string]$RecordInput,
    [string]$ReplayInput,
    [string]$HostProfile
)
$ErrorActionPreference='Stop'
$projectPath=Split-Path $PSScriptRoot -Parent
function Resolve-TestPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}
$binaryPath=Resolve-TestPath $BinaryDirectory
$disc=[IO.Path]::GetFullPath($DiscPath)
if (-not (Test-Path -LiteralPath $disc -PathType Leaf)) { throw 'Disc file missing.' }
if ($DurationSeconds -lt 1 -or $DurationSeconds -gt 300) { throw 'Duration must be 1..300 seconds.' }
$runPath=Join-Path (Resolve-TestPath $OutputDirectory) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runPath -Force | Out-Null
$reservation=[Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback,0))
$port=$reservation.Client.LocalEndPoint.Port
$reservation.Dispose()
$testPeers=@()
$failure=$null
try {
    foreach ($side in 0,1) {
        $profile=Join-Path $runPath "profile-$side"
        New-Item -ItemType Directory -Path $profile | Out-Null
        $settings=@{
            'backend.graphicsBackend'='d3d12';'backend.isoPath'=$disc;
            'backend.isoVerification'=2;'backend.wasPresetChosen'=$true;
            'game.internalResolutionScale'=1;'game.shadowResolutionMultiplier'=1;
            'video.targetFrameRate'=60;'audio.masterVolume'=0
        }
        if ($side -eq 0 -and $HostProfile) {
            $sourceProfile=Resolve-TestPath $HostProfile
            # ConvertFrom-Json -AsHashtable needs PowerShell 6.2+; 5.1 returns a
            # PSCustomObject, which ConvertTo-Json would re-emit unchanged but
            # which cannot take the audio override below.
            $parsed=Get-Content -LiteralPath (Join-Path $sourceProfile 'config.json') -Raw | ConvertFrom-Json
            $settings=@{}
            foreach ($field in $parsed.PSObject.Properties) { $settings[$field.Name]=$field.Value }
            foreach ($card in Get-ChildItem -LiteralPath $sourceProfile -File -Filter 'MemoryCard*.raw') {
                Copy-Item -LiteralPath $card.FullName -Destination $profile
            }
            # Preserve a different offline disc path to exercise the online override.
            $settings['audio.masterVolume']=0
        }
        # Windows PowerShell 5.1 has no utf8NoBOM encoding name, and the game's
        # config parser rejects a byte-order mark.
        [IO.File]::WriteAllText((Join-Path $profile 'config.json'),
            ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
        $prefix='Local\PartyBoardOnlineStart-'+[Guid]::NewGuid().ToString('N')
        $ready=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$prefix+'-ready')
        $go=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$prefix+'-go')
        $cancel=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$prefix+'-cancel')
        $start=[Diagnostics.ProcessStartInfo]::new()
        $start.FileName=Join-Path $binaryPath 'partyboard.exe'
        $start.WorkingDirectory=$binaryPath
        $transport=if ($side -eq 0) { "--netplay-host $port" } else { "--netplay-join 127.0.0.1:$port" }
        $start.Arguments="$transport --netplay-full --netplay-loopback --netplay-delay 3"
        if ($Menu) { $start.Arguments+=" --netplay-menu-probe" }
        if ($Walk) { $start.Arguments+=" --netplay-walk-probe" }
        # Each peer records both seats to its own file so the two can be
        # compared; a replay reads one shared file.
        if ($RecordInput) {
            $start.Arguments+=" --netplay-record-input " + [char]34 +
                (Join-Path $runPath "peer-$side-input.txt") + [char]34
        }
        if ($ReplayInput) {
            $start.Arguments+=" --netplay-replay-input " + [char]34 +
                (Resolve-TestPath $ReplayInput) + [char]34
        }
        $start.UseShellExecute=$false
        $start.CreateNoWindow=$true
        $start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
        $start.RedirectStandardOutput=$true
        $start.RedirectStandardError=$true
        $start.EnvironmentVariables['PARTYBOARD_ONLINE_DISC']=$disc
        $start.EnvironmentVariables['PARTYBOARD_ONLINE_READY']=$prefix+'-ready'
        $start.EnvironmentVariables['PARTYBOARD_ONLINE_GO']=$prefix+'-go'
        $start.EnvironmentVariables['PARTYBOARD_ONLINE_CANCEL']=$prefix+'-cancel'
        $start.EnvironmentVariables['PARTYBOARD_NETPLAY_TEST_PROFILE']=$profile
        $diagnostic=Join-Path $runPath "peer-$side-native.log"
        $start.EnvironmentVariables['PARTYBOARD_NET_DIAGNOSTIC']=$diagnostic
        # Crash reports, minidumps and the live state file go beside the run's
        # other evidence, named per seat. Without this the reporter falls back to
        # the diagnostic's directory, which works but cannot name the seat.
        $start.EnvironmentVariables['PARTYBOARD_CRASH_DIR']=$runPath
        $start.EnvironmentVariables['PARTYBOARD_CRASH_PEER']="$side"
        $start.EnvironmentVariables['PARTYBOARD_CRASH_ROLE']=$(if ($side -eq 0) { 'host' } else { 'client' })
        $process=[Diagnostics.Process]::Start($start)
        $testPeers+=@{Process=$process;Out=$process.StandardOutput.ReadToEndAsync();Err=$process.StandardError.ReadToEndAsync();Ready=$ready;Go=$go;Cancel=$cancel;Side=$side;Diagnostic=$diagnostic}
    }
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while (-not ($testPeers[0].Ready.WaitOne(0) -and $testPeers[1].Ready.WaitOne(0))) {
        foreach ($peer in $testPeers) { if ($peer.Process.HasExited) { throw "Peer $($peer.Side) exited during real boot." } }
        if ($timer.Elapsed.TotalSeconds -gt 120) { throw 'Real boot did not reach READY.' }
        Start-Sleep -Milliseconds 100
    }
    foreach ($peer in $testPeers) { $peer.Go.Set() | Out-Null }
    $timer.Restart()
    while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
        foreach ($peer in $testPeers) {
            if ($peer.Process.HasExited) { throw "Peer $($peer.Side) exited during gameplay." }
            if (Test-Path -LiteralPath $peer.Diagnostic) {
                $log=Get-Content -Raw -LiteralPath $peer.Diagnostic
                if ($log -match '(DESYNC|PROTOCOL)[^\r\n]*') { throw $Matches[0] }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    $hashes=@(@{},@{})
    $discIds=@()
    foreach ($peer in $testPeers) {
        $log=Get-Content -Raw -LiteralPath $peer.Diagnostic
        if ($log -notmatch 'event=boot_disc id=([A-Z0-9]{6})') { throw 'Missing actual disc identity.' }
        $discIds+=$Matches[1]
        $samples=[regex]::Matches($log,'checkpoint hash_frame=(\d+) state_hash=([0-9a-f]+) hash_version=(\d+) equal_next=(\d+)')
        if ($samples.Count -lt 2 -or [int]$samples[$samples.Count-1].Groups[4].Value -lt 120) {
            throw 'No confirmed real gameplay state progress.'
        }
        foreach ($sample in $samples) { $hashes[$peer.Side][$sample.Groups[1].Value]=$sample.Groups[2].Value }
    }
    if ($discIds[0] -ne $discIds[1]) { throw 'The games loaded different discs.' }
    Write-Output "Actual disc: $($discIds[0])"
    $compared=0
    foreach ($frame in $hashes[0].Keys) {
        if ($hashes[1].ContainsKey($frame)) {
            if ($hashes[0][$frame] -ne $hashes[1][$frame]) { throw "Real checkpoint mismatch at frame $frame" }
            ++$compared
        }
    }
    if ($compared -lt 2) { throw 'Insufficient matching real checkpoints.' }
    Write-Output "Compared real checkpoints: $compared"
} catch { $failure=$_.Exception.Message }
finally {
    foreach ($peer in $testPeers) {
        $peer.Cancel.Set() | Out-Null
        if (-not $peer.Process.HasExited) {
            $peer.Process.CloseMainWindow() | Out-Null
            if (-not $peer.Process.WaitForExit(3000)) { $peer.Process.Kill();$peer.Process.WaitForExit() }
        }
        [IO.File]::WriteAllText((Join-Path $runPath "peer-$($peer.Side)-stdout.log"),$peer.Out.Result)
        [IO.File]::WriteAllText((Join-Path $runPath "peer-$($peer.Side)-stderr.log"),$peer.Err.Result)
        $peer.Process.Dispose();$peer.Ready.Dispose();$peer.Go.Dispose();$peer.Cancel.Dispose()
    }
}
Write-Output "Real boot logs: $runPath"
if ($failure) { throw $failure }
# Both peers must cross the same overlays on the same simulation frames. A
# transition even one frame apart is already a divergent game path, and it is
# what the wall-clock boot waits used to produce.
$overlays = @()
foreach ($side in 0, 1) {
    $log = Get-Content -LiteralPath (Join-Path $runPath "peer-$side-stdout.log") -Raw
    $seen = [regex]::Matches($log, 'game context (-?\d+) at network frame (\d+)') |
        ForEach-Object { "$($_.Groups[1].Value)@$($_.Groups[2].Value)" }
    $overlays += , @($seen)
}
Write-Output ("Overlay path: " + ($overlays[0] -join " -> "))
if ($overlays[0].Count -lt 2) { throw 'No overlay transition observed.' }
if (Compare-Object $overlays[0] $overlays[1] -SyncWindow 0) {
    $a = $overlays[0] -join ' '
    $b = $overlays[1] -join ' '
    throw "Peers took different overlay paths. peer0: $a | peer1: $b"
}
Write-Output "PASS: both peers crossed $($overlays[0].Count) overlay transitions on identical frames."
if ($RecordInput) {
    # Every peer records both seats, so the frames both of them reached must match
    # exactly. A difference there would mean the peers disagreed about the input
    # timeline itself, which the canonical hash could only catch a frame later.
    # The two files do differ in length: the run ends by closing both processes,
    # and one of them always commits a few more frames than the other before it
    # goes. Only the common prefix is an invariant, and only that prefix is kept
    # as the replay timeline.
    $recorded = @()
    foreach ($side in 0, 1) {
        $path = Join-Path $runPath "peer-$side-input.txt"
        if (-not (Test-Path -LiteralPath $path)) { throw "Peer $side wrote no input recording." }
        $recorded += , @(Get-Content -LiteralPath $path)
    }
    $shared = [Math]::Min($recorded[0].Count, $recorded[1].Count)
    if ($shared -lt 60) { throw "Input recording too short: $shared shared rows." }
    if (Compare-Object $recorded[0][0..($shared - 1)] $recorded[1][0..($shared - 1)] -SyncWindow 0) {
        throw "The two peers disagreed within their first $shared recorded rows."
    }
    $target = Resolve-TestPath $RecordInput
    [IO.File]::WriteAllLines($target, $recorded[0][0..($shared - 1)])
    # Store the overlay path the recording produced. A later replay must land on
    # the same overlays at the same frames, which is what makes the recording a
    # regression test rather than just a saved session.
    [IO.File]::WriteAllLines("$target.overlays", $overlays[0])
    $tail = [Math]::Abs($recorded[0].Count - $recorded[1].Count)
    Write-Output "PASS: both peers recorded an identical $shared-row input timeline ($tail trailing rows dropped at shutdown)."
    Write-Output "Recording saved to $target"
}

if ($ReplayInput) {
    $expectedPath = (Resolve-TestPath $ReplayInput) + '.overlays'
    if (Test-Path -LiteralPath $expectedPath) {
        $expected = @(Get-Content -LiteralPath $expectedPath)
        if (Compare-Object $expected $overlays[0] -SyncWindow 0) {
            $want = $expected -join ' -> '
            $got = $overlays[0] -join ' -> '
            throw "Replay diverged from the recording. expected: $want | got: $got"
        }
        Write-Output "PASS: the replay reproduced all $($expected.Count) recorded overlay transitions on the same frames."
    } else {
        Write-Output "NOTE: no $expectedPath beside the recording, so the replay path was not compared."
    }
}

if ($Menu) {
    foreach ($side in 0,1) {
        $log=Get-Content -LiteralPath (Join-Path $runPath "peer-$side-stdout.log") -Raw
        $native=Get-Content -LiteralPath (Join-Path $runPath "peer-$side-native.log") -Raw
        if ($native -notmatch 'event=mode_select online=1 menu_event=0 skip_file=1' -or
            $log -notmatch 'Online mode select: fresh session profile') {
            throw "Peer $side did not reach the shared online mode-selection path."
        }
    }
    Write-Output 'PASS: both peers entered mode selection with online=1 and skip_file=1.'
}
Write-Output "PASS: real online boot observed for $DurationSeconds seconds. No board/minigame scenario coverage claimed."
