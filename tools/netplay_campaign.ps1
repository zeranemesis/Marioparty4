# Runs a campaign of netplay scenarios and keeps everything it finds.
#
# One campaign is a list of runs. Each run starts two real instances, plays a
# scenario, watches both of them, and ends with a classification that is written
# down whether it is good news or not. A failure is never deleted because the
# loop continued, and a deterministic crash is never retried until a lucky PASS
# appears: a FAIL stays a FAIL until a person decides otherwise.
#
#   tools\netplay_campaign.ps1 -DiscPath <iso>
#   tools\netplay_campaign.ps1 -DiscPath <iso> -Scenario boo-replay -Repeat 20
#   tools\netplay_campaign.ps1 -DiscPath <iso> -Resume work\netplay-campaigns\<stamp>
#
# Results, per run: PASS CRASH DESYNC TIMEOUT ABNORMAL_EXIT HARNESS_FAILURE.
# Run states, for resume: PENDING RUNNING PASS FAIL RETRY_REQUIRED.
#
# HARNESS_FAILURE means the harness itself could not deliver the scenario (the
# peers never reached READY, a file was missing). It is kept separate from
# FAIL so a broken harness can never be read as a broken game, in either
# direction.

param(
    [Parameter(Mandatory = $true)][string]$DiscPath,
    [string]$Manifest = 'tests/scenarios/scenarios.json',
    [string]$BinaryDirectory = 'build/aexp/RelWithDebInfo',
    [string]$CampaignRoot = 'work/netplay-campaigns',
    # Resume an existing campaign directory instead of starting a new one.
    [string]$Resume,
    # Only these scenario ids.
    [string[]]$Scenario,
    # Override the manifest's repeat count.
    [int]$Repeat = 0,
    # Stop after this many runs in this invocation (0 = no limit).
    [int]$MaxRuns = 0,
    # Re-run runs that previously failed. Off by default on purpose: a failing
    # run is evidence, and re-rolling it until it passes destroys that evidence.
    [switch]$RerunFailures,
    # Verbosity of the audio lifetime trace: '' off, '1' banks and violations,
    # '2' every voice event as well.
    [string]$AudioDiagnostics = '1',
    # Forced local rollback probe, '' off. See PARTYBOARD_FORCE_ROLLBACK.
    [string]$ForceRollback = '',
    # Suffix for the campaign directory, so several campaigns started in the
    # same second do not collide.
    [string]$Label = '',
    # Rendered frames per second each peer targets. 60 makes the frame pacer
    # return exactly one simulation tick per rendered frame; above it the pacer
    # catches up with real time and can batch several, which is the only
    # condition under which defect D6 can occur. Two peers may be given
    # different values, which is itself something no code prevents.
    [int]$TargetFrameRate = 60,
    [int]$TargetFrameRatePeer1 = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'netplay_session.ps1')

$projectPath = Split-Path $PSScriptRoot -Parent
function Resolve-CampaignPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}

$disc = [IO.Path]::GetFullPath($DiscPath)
if (-not (Test-Path -LiteralPath $disc -PathType Leaf)) { throw "Disc file missing: $disc" }
$binaryPath = Resolve-CampaignPath $BinaryDirectory
$exePath = Join-Path $binaryPath 'partyboard.exe'
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { throw "Binary missing: $exePath" }

# The build identity every result is stamped with. A campaign whose runs came
# from different commits is not a campaign, so this is recorded per run and
# checked on resume.
function Get-BuildIdentity {
    $commit = 'unknown'; $branch = 'unknown'; $dirty = 'unknown'
    Push-Location $projectPath
    try {
        $commit = (& git rev-parse HEAD 2>$null); if ($LASTEXITCODE -ne 0) { $commit = 'unknown' }
        $branch = (& git rev-parse --abbrev-ref HEAD 2>$null); if ($LASTEXITCODE -ne 0) { $branch = 'unknown' }
        $status = (& git status --porcelain 2>$null)
        if ($LASTEXITCODE -eq 0) { $dirty = if ($status) { 'dirty' } else { 'clean' } }
    } finally { Pop-Location }
    return @{
        Commit = "$commit"; Branch = "$branch"; Tree = "$dirty"
        Binary = $exePath
        BinaryWritten = (Get-Item -LiteralPath $exePath).LastWriteTimeUtc.ToString('o')
    }
}

# ---------------------------------------------------------------------------
# Manifest and plan
# ---------------------------------------------------------------------------

function Read-Manifest([string]$path) {
    $full = Resolve-CampaignPath $path
    if (-not (Test-Path -LiteralPath $full)) { throw "Scenario manifest missing: $full" }
    $parsed = Get-Content -LiteralPath $full -Raw | ConvertFrom-Json
    $list = @()
    foreach ($entry in $parsed.scenarios) {
        $list += @{
            Id = [string]$entry.id
            Description = [string]$entry.description
            Board = [string]$entry.board
            Minigame = if ($entry.PSObject.Properties.Name -contains 'minigame') { [string]$entry.minigame } else { '' }
            Kind = [string]$entry.kind
            Replay = if ($entry.PSObject.Properties.Name -contains 'replay') { [string]$entry.replay } else { '' }
            ExpectedOverlays = if ($entry.PSObject.Properties.Name -contains 'expectedOverlays') { [string]$entry.expectedOverlays } else { '' }
            DurationSeconds = [int]$entry.durationSeconds
            MinFrames = [int]$entry.minFrames
            Repeats = [int]$entry.repeats
            NetplayDelay = if ($entry.PSObject.Properties.Name -contains 'netplayDelay') { [int]$entry.netplayDelay } else { 3 }
            Seed = if ($entry.PSObject.Properties.Name -contains 'seed') { [string]$entry.seed } else { 'default' }
        }
    }
    return $list
}

$campaignPath = $null
$state = $null

if ($Resume) {
    $campaignPath = Resolve-CampaignPath $Resume
    $statePath = Join-Path $campaignPath 'campaign-state.json'
    if (-not (Test-Path -LiteralPath $statePath)) { throw "No campaign to resume at $campaignPath" }
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    Write-Output "Resuming campaign $campaignPath"
} else {
    $stamp = (Get-Date).ToString('yyyy-MM-dd_HHmmss')
    if ($Label) { $stamp = "$stamp-$Label" }
    $campaignPath = Join-Path (Resolve-CampaignPath $CampaignRoot) $stamp
    New-Item -ItemType Directory -Path $campaignPath -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $campaignPath 'runs') -Force | Out-Null
}

$build = Get-BuildIdentity
$scenarios = Read-Manifest $Manifest
if ($Scenario) { $scenarios = @($scenarios | Where-Object { $Scenario -contains $_.Id }) }
if ($scenarios.Count -eq 0) { throw 'No scenario selected.' }

if ($null -eq $state) {
    $plan = @()
    foreach ($entry in $scenarios) {
        $count = if ($Repeat -gt 0) { $Repeat } else { [Math]::Max(1, $entry.Repeats) }
        for ($index = 1; $index -le $count; $index++) {
            $plan += [pscustomobject]@{
                ScenarioId = $entry.Id
                RunIndex = $index
                State = 'PENDING'
                Result = ''
                Directory = ''
                Fingerprint = ''
                StartedAt = ''
                DurationSeconds = 0
                LastFrame = 0
            }
        }
    }
    $state = [pscustomobject]@{
        Version = 1
        CreatedAt = (Get-Date).ToString('o')
        Disc = $disc
        Build = [pscustomobject]$build
        Manifest = (Resolve-CampaignPath $Manifest)
        Runs = $plan
    }
} else {
    if ($state.Build.Commit -ne $build.Commit) {
        Write-Warning ("This campaign was started at commit $($state.Build.Commit) and the tree " +
            "is now at $($build.Commit). Runs from two different builds are not comparable; " +
            "start a new campaign instead of resuming unless you know why you are mixing them.")
    }
}

$statePath = Join-Path $campaignPath 'campaign-state.json'
function Save-State { $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $statePath -Encoding utf8 }
Save-State

# ---------------------------------------------------------------------------
# One run
# ---------------------------------------------------------------------------

function Invoke-CampaignRun($entry, [int]$runIndex, [string]$runPath) {
    New-Item -ItemType Directory -Path $runPath -Force | Out-Null

    $record = [ordered]@{
        scenario = $entry.Id
        description = $entry.Description
        board = $entry.Board
        minigame = $entry.Minigame
        run_index = $runIndex
        build_commit = $build.Commit
        build_branch = $build.Branch
        build_tree = $build.Tree
        binary = $build.Binary
        binary_written = $build.BinaryWritten
        seed = $entry.Seed
        audio_diagnostics = $AudioDiagnostics
        force_rollback = $ForceRollback
        target_frame_rate = $TargetFrameRate
        target_frame_rate_peer_1 = $(if ($TargetFrameRatePeer1 -gt 0) { $TargetFrameRatePeer1 } else { $TargetFrameRate })
        replay = $entry.Replay
        netplay_delay = $entry.NetplayDelay
        started_at = (Get-Date).ToString('o')
        duration_seconds = 0
        last_frame = 0
        overlays = @()
        transitions = 0
        exit_peer_0 = 'unknown'
        exit_peer_1 = 'unknown'
        classification_peer_0 = 'unknown'
        classification_peer_1 = 'unknown'
        mismatch = 'unknown'
        rng_sync = 'unknown'
        repaired = 'unknown'
        send_errors = 'unknown'
        audio_lifetime_violations = 0
        audio_lifetime_bank_frees = 0
        audio_lifetime_armed = $false
        d6_batched_frames = 0
        d6_worst_batch = 0
        crash_fingerprint = ''
        desync_fingerprint = ''
        result = 'HARNESS_FAILURE'
        notes = @()
    }

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $peers = @()
    $harnessError = $null

    try {
        $reservation = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
        $port = $reservation.Client.LocalEndPoint.Port
        $reservation.Dispose()

        foreach ($side in 0, 1) {
            $profile = Join-Path $runPath "profile-$side"
            New-Item -ItemType Directory -Path $profile -Force | Out-Null
            $settings = @{
                'backend.graphicsBackend' = 'd3d12'; 'backend.isoPath' = $disc
                'backend.isoVerification' = 2; 'backend.wasPresetChosen' = $true
                'game.internalResolutionScale' = 1; 'game.shadowResolutionMultiplier' = 1
                'video.targetFrameRate' = $TargetFrameRate; 'audio.masterVolume' = 0
            }
            if ($side -eq 1 -and $TargetFrameRatePeer1 -gt 0) {
                $settings['video.targetFrameRate'] = $TargetFrameRatePeer1
            }
            # PowerShell 5.1 writes a byte-order mark the config parser rejects.
            [IO.File]::WriteAllText((Join-Path $profile 'config.json'),
                ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))

            $prefix = 'Local\PartyBoardOnlineStart-' + [Guid]::NewGuid().ToString('N')
            $ready = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-ready')
            $go = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-go')
            $cancel = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-cancel')

            $start = [Diagnostics.ProcessStartInfo]::new()
            $start.FileName = $exePath
            $start.WorkingDirectory = $binaryPath
            $transport = if ($side -eq 0) { "--netplay-host $port" } else { "--netplay-join 127.0.0.1:$port" }
            $start.Arguments = "$transport --netplay-full --netplay-loopback --netplay-delay $($entry.NetplayDelay)"
            if ($entry.Replay) {
                $start.Arguments += ' --netplay-replay-input ' + [char]34 + (Resolve-CampaignPath $entry.Replay) + [char]34
            }
            $start.UseShellExecute = $false
            $start.CreateNoWindow = $true
            $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
            $start.RedirectStandardOutput = $true
            $start.RedirectStandardError = $true
            $diagnostic = Join-Path $runPath "peer-$side-native.log"
            $start.EnvironmentVariables['PARTYBOARD_ONLINE_DISC'] = $disc
            $start.EnvironmentVariables['PARTYBOARD_ONLINE_READY'] = $prefix + '-ready'
            $start.EnvironmentVariables['PARTYBOARD_ONLINE_GO'] = $prefix + '-go'
            $start.EnvironmentVariables['PARTYBOARD_ONLINE_CANCEL'] = $prefix + '-cancel'
            $start.EnvironmentVariables['PARTYBOARD_NETPLAY_TEST_PROFILE'] = $profile
            $start.EnvironmentVariables['PARTYBOARD_NET_DIAGNOSTIC'] = $diagnostic
            $start.EnvironmentVariables['PARTYBOARD_CRASH_DIR'] = $runPath
            $start.EnvironmentVariables['PARTYBOARD_CRASH_PEER'] = "$side"
            $start.EnvironmentVariables['PARTYBOARD_CRASH_ROLE'] = $(if ($side -eq 0) { 'host' } else { 'client' })
            # Set explicitly rather than inherited, both of them, so a variable
            # left over in the launching shell can never silently change what a
            # campaign measured.
            $start.EnvironmentVariables['PARTYBOARD_AUDIO_DIAGNOSTICS'] = $AudioDiagnostics
            $start.EnvironmentVariables['PARTYBOARD_FORCE_ROLLBACK'] = $ForceRollback

            $process = [Diagnostics.Process]::Start($start)
            $peers += @{
                Process = $process; Side = $side
                Role = $(if ($side -eq 0) { 'host' } else { 'client' })
                Out = $process.StandardOutput.ReadToEndAsync()
                Err = $process.StandardError.ReadToEndAsync()
                Ready = $ready; Go = $go; Cancel = $cancel
                Diagnostic = $diagnostic
                StartTime = (Get-Date)
                ExitTime = $null
                ExitCode = 0
                ClosedBySupervisor = $false
                CrashReports = @(); Minidumps = @(); DesyncReports = @()
                LiveState = $null; Fault = $null; Classification = 'unknown'
            }
        }

        $wait = [Diagnostics.Stopwatch]::StartNew()
        while (-not ($peers[0].Ready.WaitOne(0) -and $peers[1].Ready.WaitOne(0))) {
            foreach ($peer in $peers) {
                if ($peer.Process.HasExited) { throw "Peer $($peer.Side) exited before READY." }
            }
            if ($wait.Elapsed.TotalSeconds -gt 120) { throw 'Neither peer reached READY within 120 s.' }
            Start-Sleep -Milliseconds 100
        }
        foreach ($peer in $peers) { $peer.Go.Set() | Out-Null }

        # The run itself. Ends when the budget elapses, when a peer dies, or when
        # a desync is seen in a peer's own diagnostic.
        $wait.Restart()
        $endedEarly = $null
        while ($wait.Elapsed.TotalSeconds -lt $entry.DurationSeconds) {
            foreach ($peer in $peers) {
                if ($peer.Process.HasExited) {
                    $endedEarly = "peer $($peer.Side) exited during gameplay"
                    break
                }
                if (Test-Path -LiteralPath $peer.Diagnostic) {
                    $log = Get-Content -Raw -LiteralPath $peer.Diagnostic
                    if ($log -match '(DESYNC|PROTOCOL)[^\r\n]*') {
                        $endedEarly = "peer $($peer.Side): $($Matches[0])"
                        break
                    }
                }
            }
            if ($endedEarly) { break }
            Start-Sleep -Milliseconds 200
        }
        if ($endedEarly) { $record.notes += $endedEarly }
    } catch {
        $harnessError = $_.Exception.Message
    } finally {
        foreach ($peer in $peers) {
            if (-not $peer.Process.HasExited) {
                $peer.ClosedBySupervisor = $true
                $peer.Cancel.Set() | Out-Null
                $peer.Process.CloseMainWindow() | Out-Null
                if (-not $peer.Process.WaitForExit(5000)) { $peer.Process.Kill() }
            }
            $peer.Process.WaitForExit()
            $peer.ExitCode = $peer.Process.ExitCode
            $peer.ExitTime = $peer.Process.ExitTime
            [IO.File]::WriteAllText((Join-Path $runPath "peer-$($peer.Side)-stdout.log"), $peer.Out.Result)
            [IO.File]::WriteAllText((Join-Path $runPath "peer-$($peer.Side)-stderr.log"), $peer.Err.Result)
            $peer.Process.Dispose(); $peer.Ready.Dispose(); $peer.Go.Dispose(); $peer.Cancel.Dispose()
        }
    }

    $timer.Stop()
    $record.duration_seconds = [int]$timer.Elapsed.TotalSeconds

    if ($peers.Count -lt 2) {
        $record.result = 'HARNESS_FAILURE'
        if ($harnessError) { $record.notes += $harnessError }
        return $record
    }

    # ---- what happened to each peer ----
    foreach ($peer in $peers) {
        $side = $peer.Side
        $peer.CrashReports = @(Get-ChildItem -LiteralPath $runPath -Filter "crash-report-peer-$side-*.txt" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
        $peer.Minidumps = @(Get-ChildItem -LiteralPath $runPath -Filter "crash-peer-$side-*.dmp" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
        $peer.DesyncReports = @(Get-ChildItem -LiteralPath $runPath -Recurse -Filter 'netplay_desync_*.log' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
        $peer.LiveState = Read-LiveState (Join-Path $runPath "live-state-peer-$side.txt")
        $peer.Fault = Get-FaultRecord $peer.Process.Id $peer.StartTime $peer.ExitTime
        $peer.Classification = Get-PeerClassification $peer
        $record["exit_peer_$side"] = Format-ExitCode $peer.ExitCode
        $record["classification_peer_$side"] = $peer.Classification
    }

    $record.last_frame = [Math]::Max($peers[0].LiveState.Frame, $peers[1].LiveState.Frame)
    $record.mismatch = $peers[0].LiveState.Mismatch
    $record.rng_sync = $peers[0].LiveState.RngSync
    $record.repaired = $peers[0].LiveState.Repaired
    $record.send_errors = $peers[0].LiveState.SendErrors

    # ---- audio lifetime evidence ----
    #
    # Counting violations is only half of it. A detector that was never armed
    # reports zero exactly like a detector that found nothing, so the run also
    # has to show the detector was alive: a trace that contains at least one
    # BANK_FREE is one that watched a bank being released.
    $violations = 0
    $bankFrees = 0
    foreach ($side in 0, 1) {
        $trace = Join-Path $runPath "audio-lifetime-peer-$side.txt"
        if (Test-Path -LiteralPath $trace) {
            $hits = @(Select-String -Path $trace -Pattern 'STALE_REFERENCE_AT_FREE|STALE_SAMPLE_READ|STARTED_ON_RETIRED_SAMPLE' -ErrorAction SilentlyContinue)
            $violations += $hits.Count
            $bankFrees += @(Select-String -Path $trace -Pattern '^BANK_FREE ' -ErrorAction SilentlyContinue).Count
        }
    }
    $record.audio_lifetime_violations = $violations
    $record.audio_lifetime_bank_frees = $bankFrees
    $record.audio_lifetime_armed = ($AudioDiagnostics -ne '') -and ($bankFrees -gt 0)

    # ---- overlay paths ----
    $overlays = @(, @(), @())
    foreach ($side in 0, 1) {
        $logPath = Join-Path $runPath "peer-$side-stdout.log"
        if (Test-Path -LiteralPath $logPath) {
            $log = Get-Content -LiteralPath $logPath -Raw
            $overlays[$side] = @([regex]::Matches($log, 'game context (-?\d+) at network frame (\d+)') |
                ForEach-Object { "$($_.Groups[1].Value)@$($_.Groups[2].Value)" })
        }
    }
    $record.overlays = $overlays[0]
    $record.transitions = $overlays[0].Count

    # Defect D6: a rendered frame that batched more than one simulation tick.
    # The game prints it; the campaign only has to notice.
    $batched = 0
    $worstBatch = 0
    foreach ($side in 0, 1) {
        $logPath = Join-Path $runPath "peer-$side-stdout.log"
        if (-not (Test-Path -LiteralPath $logPath)) { continue }
        foreach ($hit in Select-String -Path $logPath -Pattern 'D6> (\d+) simulation ticks in one rendered frame \(occurrences (\d+), worst (\d+)\)') {
            $batched = [Math]::Max($batched, [int]$hit.Matches[0].Groups[2].Value)
            $worstBatch = [Math]::Max($worstBatch, [int]$hit.Matches[0].Groups[3].Value)
        }
    }
    $record.d6_batched_frames = $batched
    $record.d6_worst_batch = $worstBatch

    # ---- fingerprints ----
    # A minidump with no report beside it is a crash the reporter could not
    # describe, and it must not be able to pass as a clean run. An empty one is
    # a reporter that could not finish, which is its own kind of finding.
    $dumps = @($peers[0].Minidumps) + @($peers[1].Minidumps)
    $solidDumps = @($dumps | Where-Object { (Get-Item -LiteralPath $_).Length -gt 0 })
    $emptyDumps = @($dumps | Where-Object { (Get-Item -LiteralPath $_).Length -eq 0 })
    if ($emptyDumps.Count -gt 0) {
        $record.notes += "$($emptyDumps.Count) empty minidump(s): the reporter started and did not finish"
    }

    $crashReports = @($peers[0].CrashReports) + @($peers[1].CrashReports)
    if ($crashReports.Count -gt 0) {
        $facts = Get-CrashReportFacts $crashReports[0]
        $record.crash_fingerprint = Get-CrashFingerprint $facts @{
            AudioLifetimeViolation = ($violations -gt 0)
            MemoryCorruption = $false
        }
        $record.notes += "crash report: $($crashReports[0])"
    }
    $desyncReports = @($peers[0].DesyncReports) + @($peers[1].DesyncReports)
    if ($desyncReports.Count -gt 0) {
        $record.desync_fingerprint = Get-DesyncFingerprint $desyncReports[0]
    }

    # A forced rollback that did not reproduce its own frame is a determinism
    # failure, even though no peer disagreed with the other: the same inputs
    # gave two different answers on one machine.
    $rollbackReports = @(Get-ChildItem -LiteralPath $runPath -Filter 'rollback-failure-*.txt' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    if ($rollbackReports.Count -gt 0) {
        $text = Get-Content -LiteralPath $rollbackReports[0] -Raw
        $subsystem = 'unknown'; $field = 'unknown'
        $m = [regex]::Match($text, 'first_divergent_subsystem=(\S+)'); if ($m.Success) { $subsystem = $m.Groups[1].Value }
        $m = [regex]::Match($text, 'first_divergent_field=([^
]+)');  if ($m.Success) { $field = $m.Groups[1].Value.Trim() }
        $record.desync_fingerprint = "ROLLBACK:$subsystem`:$field"
        $record.notes += "$($rollbackReports.Count) rollback failure report(s), first divergence in $subsystem at $field"
    }

    # ---- verdict ----
    # Most severe cause first. A crash outranks everything: a run that crashed
    # cannot also be said to have passed some other way.
    $classifications = @($peers[0].Classification, $peers[1].Classification)
    if ($harnessError) {
        $record.result = 'HARNESS_FAILURE'
        $record.notes += $harnessError
    } elseif ($classifications -contains 'PROCESS_CRASH') {
        $record.result = 'CRASH'
    } elseif ($classifications -contains 'NETPLAY_DESYNC' -or $rollbackReports.Count -gt 0 -or ($record.mismatch -ne 'unknown' -and [int]$record.mismatch -gt 0)) {
        $record.result = 'DESYNC'
    } elseif ($solidDumps.Count -gt 0) {
        $record.result = 'CRASH'
        $record.notes += "minidump written with no crash report: $($solidDumps[0])"
        if (-not $record.crash_fingerprint) {
            $record.crash_fingerprint = 'UNDESCRIBED_CRASH:unknown:minidump_without_report'
        }
    } elseif ($classifications -contains 'UNKNOWN_ABNORMAL_EXIT') {
        $record.result = 'ABNORMAL_EXIT'
    } elseif ($record.last_frame -lt $entry.MinFrames) {
        $record.result = 'TIMEOUT'
        $record.notes += "reached frame $($record.last_frame), scenario needs $($entry.MinFrames)"
    } elseif ($overlays[0].Count -eq 0 -or (Compare-Object $overlays[0] $overlays[1] -SyncWindow 0)) {
        $record.result = 'DESYNC'
        $record.notes += 'the two peers took different overlay paths'
    } elseif (($AudioDiagnostics -ne '') -and -not $record.audio_lifetime_armed) {
        # The scenario asked for the lifetime detector and the run has no
        # evidence it ever watched a bank being released. Whatever else the run
        # shows, it did not measure what it was asked to.
        $record.result = 'HARNESS_FAILURE'
        $record.notes += "the audio lifetime detector was requested but never observed a bank free"
    } elseif ($violations -gt 0) {
        # A lifetime violation is a defect even when nothing crashed. Calling
        # such a run PASS would hide the very thing the detector exists for.
        $record.result = 'CRASH'
        $record.notes += "$violations audio lifetime violations with no fault raised"
        if (-not $record.crash_fingerprint) {
            $record.crash_fingerprint = 'AUDIO_UAF:detected-without-fault:audio_lifetime_detector'
        }
    } else {
        $record.result = 'PASS'
    }

    # Expected overlay path, when the scenario declares one. A replay that stops
    # short is a TIMEOUT above, so what is checked here is the shape of the
    # prefix, not its length.
    if ($entry.ExpectedOverlays -and $record.result -eq 'PASS') {
        $expectedPath = Resolve-CampaignPath $entry.ExpectedOverlays
        if (Test-Path -LiteralPath $expectedPath) {
            $expected = @(Get-Content -LiteralPath $expectedPath)
            $shared = [Math]::Min($expected.Count, $overlays[0].Count)
            if ($shared -gt 0 -and (Compare-Object $expected[0..($shared - 1)] $overlays[0][0..($shared - 1)] -SyncWindow 0)) {
                $record.result = 'DESYNC'
                $record.notes += 'the overlay path diverged from the recorded one'
            }
        }
    }

    return $record
}

# ---------------------------------------------------------------------------
# Campaign loop
# ---------------------------------------------------------------------------

$executed = 0
foreach ($run in $state.Runs) {
    if ($MaxRuns -gt 0 -and $executed -ge $MaxRuns) { break }
    if ($run.State -eq 'PASS') { continue }
    if ($run.State -eq 'FAIL' -and -not $RerunFailures) { continue }
    if ($Scenario -and ($Scenario -notcontains $run.ScenarioId)) { continue }

    $entry = $scenarios | Where-Object { $_.Id -eq $run.ScenarioId } | Select-Object -First 1
    if (-not $entry) { continue }

    $runPath = Join-Path $campaignPath ("runs/{0}/{1:d3}" -f $run.ScenarioId, $run.RunIndex)
    $run.State = 'RUNNING'
    $run.Directory = $runPath
    $run.StartedAt = (Get-Date).ToString('o')
    Save-State

    Write-Output ("[{0}] {1} run {2} starting ({3} s budget)" -f (Get-Date).ToString('HH:mm:ss'),
        $run.ScenarioId, $run.RunIndex, $entry.DurationSeconds)

    $record = Invoke-CampaignRun $entry $run.RunIndex $runPath
    $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runPath 'run.json') -Encoding utf8

    $run.Result = $record.result
    $run.Fingerprint = $record.crash_fingerprint
    $run.DurationSeconds = $record.duration_seconds
    $run.LastFrame = $record.last_frame
    $run.State = switch ($record.result) {
        'PASS' { 'PASS' }
        'HARNESS_FAILURE' { 'RETRY_REQUIRED' }
        default { 'FAIL' }
    }
    Save-State
    $executed++

    Write-Output ("[{0}] {1} run {2}: {3} (frame {4}, {5} s) {6}" -f (Get-Date).ToString('HH:mm:ss'),
        $run.ScenarioId, $run.RunIndex, $record.result, $record.last_frame,
        $record.duration_seconds, $record.crash_fingerprint)
}

# ---------------------------------------------------------------------------
# Summaries
# ---------------------------------------------------------------------------

$records = @()
foreach ($run in $state.Runs) {
    if (-not $run.Directory) { continue }
    $path = Join-Path $run.Directory 'run.json'
    if (Test-Path -LiteralPath $path) {
        $records += (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
    }
}

$summary = [ordered]@{
    campaign = $campaignPath
    created_at = $state.CreatedAt
    finished_at = (Get-Date).ToString('o')
    build_commit = $build.Commit
    build_branch = $build.Branch
    build_tree = $build.Tree
    disc = $disc
    planned = @($state.Runs).Count
    executed = @($records).Count
    pass = @($records | Where-Object { $_.result -eq 'PASS' }).Count
    crash = @($records | Where-Object { $_.result -eq 'CRASH' }).Count
    desync = @($records | Where-Object { $_.result -eq 'DESYNC' }).Count
    timeout = @($records | Where-Object { $_.result -eq 'TIMEOUT' }).Count
    abnormal_exit = @($records | Where-Object { $_.result -eq 'ABNORMAL_EXIT' }).Count
    harness_failure = @($records | Where-Object { $_.result -eq 'HARNESS_FAILURE' }).Count
    max_frames = 0
    fingerprints = @()
    runs = $records
}
if (@($records).Count -gt 0) {
    $summary.max_frames = (@($records | ForEach-Object { [int]$_.last_frame }) | Measure-Object -Maximum).Maximum
}

# Deduplication: identical fingerprints are one defect with many occurrences.
$groups = @{}
foreach ($record in $records) {
    if (-not $record.crash_fingerprint) { continue }
    $key = $record.crash_fingerprint
    if (-not $groups.ContainsKey($key)) {
        $groups[$key] = @{ fingerprint = $key; occurrences = 0; first_run = ''; scenarios = @(); frames = @() }
    }
    $groups[$key].occurrences++
    if (-not $groups[$key].first_run) { $groups[$key].first_run = "$($record.scenario)/$($record.run_index)" }
    if ($groups[$key].scenarios -notcontains $record.scenario) { $groups[$key].scenarios += $record.scenario }
    $groups[$key].frames += [int]$record.last_frame
}
$summary.fingerprints = @($groups.Values | Sort-Object -Property occurrences -Descending)

$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $campaignPath 'campaign-summary.json') -Encoding utf8

$csv = New-Object Collections.Generic.List[string]
$csv.Add('scenario,run,result,last_frame,duration_s,exit_peer_0,exit_peer_1,class_peer_0,class_peer_1,mismatch,rng_sync,audio_violations,crash_fingerprint,desync_fingerprint')
foreach ($record in $records) {
    $csv.Add(('{0},{1},{2},{3},{4},"{5}","{6}",{7},{8},{9},{10},{11},"{12}","{13}"' -f
        $record.scenario, $record.run_index, $record.result, $record.last_frame,
        $record.duration_seconds, $record.exit_peer_0, $record.exit_peer_1,
        $record.classification_peer_0, $record.classification_peer_1,
        $record.mismatch, $record.rng_sync, $record.audio_lifetime_violations,
        $record.crash_fingerprint, $record.desync_fingerprint))
}
[IO.File]::WriteAllLines((Join-Path $campaignPath 'campaign-summary.csv'), $csv)

$md = New-Object Collections.Generic.List[string]
$md.Add("# Campagne netplay $(Split-Path $campaignPath -Leaf)")
$md.Add('')
$md.Add(('Commit `{0}` sur la branche `{1}`, arbre {2}.' -f $build.Commit, $build.Branch, $build.Tree))
$md.Add(('Disque : `{0}`.' -f $disc))
$md.Add('')
$md.Add("| runs | PASS | CRASH | DESYNC | TIMEOUT | ABNORMAL | HARNESS |")
$md.Add('|---|---|---|---|---|---|---|')
$md.Add("| $($summary.executed) | $($summary.pass) | $($summary.crash) | $($summary.desync) | $($summary.timeout) | $($summary.abnormal_exit) | $($summary.harness_failure) |")
$md.Add('')
$md.Add("Frame la plus lointaine atteinte : **$($summary.max_frames)**.")
$md.Add('')
if (@($summary.fingerprints).Count -gt 0) {
    $md.Add('## Signatures')
    $md.Add('')
    $md.Add('| signature | occurrences | premier run | scenarios |')
    $md.Add('|---|---|---|---|')
    foreach ($group in $summary.fingerprints) {
        $md.Add("| ``$($group.fingerprint)`` | $($group.occurrences) | $($group.first_run) | $($group.scenarios -join ', ') |")
    }
    $md.Add('')
} else {
    $md.Add('Aucune signature de crash enregistree.')
    $md.Add('')
}
$md.Add('## Runs')
$md.Add('')
$md.Add('| scenario | run | resultat | derniere frame | duree | mismatch | rng_sync | violations audio |')
$md.Add('|---|---|---|---|---|---|---|---|')
foreach ($record in $records) {
    $md.Add("| $($record.scenario) | $($record.run_index) | **$($record.result)** | $($record.last_frame) | $($record.duration_seconds) s | $($record.mismatch) | $($record.rng_sync) | $($record.audio_lifetime_violations) |")
}
[IO.File]::WriteAllLines((Join-Path $campaignPath 'campaign-summary.md'), $md)

Write-Output ''
Write-Output "Campaign: $campaignPath"
Write-Output ("runs={0} pass={1} crash={2} desync={3} timeout={4} abnormal={5} harness={6} max_frames={7}" -f
    $summary.executed, $summary.pass, $summary.crash, $summary.desync, $summary.timeout,
    $summary.abnormal_exit, $summary.harness_failure, $summary.max_frames)

# The exit code reports the campaign, and nothing filters it.
if ($summary.harness_failure -gt 0) { exit 3 }
if ($summary.crash -gt 0 -or $summary.desync -gt 0 -or $summary.abnormal_exit -gt 0 -or $summary.timeout -gt 0) { exit 1 }
if ($summary.executed -eq 0) { exit 2 }
exit 0
