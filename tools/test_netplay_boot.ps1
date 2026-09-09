param(
    [Parameter(Mandatory=$true)][string]$DiscPath,
    [string]$BinaryDirectory='build/aexp/RelWithDebInfo',
    [string]$OutputDirectory='work/netplay-real-boot',
    [int]$DurationSeconds=30
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
        @{
            'backend.graphicsBackend'='d3d12';'backend.isoPath'=$disc;
            'backend.isoVerification'=2;'backend.wasPresetChosen'=$true;
            'game.internalResolutionScale'=1;'game.shadowResolutionMultiplier'=1;
            'video.targetFrameRate'=60;'audio.masterVolume'=0
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $profile 'config.json') -Encoding utf8NoBOM
        $prefix='Local\PartyBoardOnlineStart-'+[Guid]::NewGuid().ToString('N')
        $ready=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$prefix+'-ready')
        $go=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$prefix+'-go')
        $cancel=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$prefix+'-cancel')
        $start=[Diagnostics.ProcessStartInfo]::new()
        $start.FileName=Join-Path $binaryPath 'partyboard.exe'
        $start.WorkingDirectory=$binaryPath
        $transport=if ($side -eq 0) { "--netplay-host $port" } else { "--netplay-join 127.0.0.1:$port" }
        $start.Arguments="$transport --netplay-full --netplay-loopback --netplay-delay 3"
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
                if ($log -match 'DESYNC[^\r\n]*') { throw $Matches[0] }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    $hashes=@(@{},@{})
    foreach ($peer in $testPeers) {
        $log=Get-Content -Raw -LiteralPath $peer.Diagnostic
        $samples=[regex]::Matches($log,'checkpoint hash_frame=(\d+) state_hash=([0-9a-f]+) hash_version=(\d+) equal_next=(\d+)')
        if ($samples.Count -lt 2 -or [int]$samples[$samples.Count-1].Groups[4].Value -lt 120) {
            throw 'No confirmed real gameplay state progress.'
        }
        foreach ($sample in $samples) { $hashes[$peer.Side][$sample.Groups[1].Value]=$sample.Groups[2].Value }
    }
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
Write-Output "PASS: real online boot observed for $DurationSeconds seconds. No board/minigame scenario coverage claimed."
