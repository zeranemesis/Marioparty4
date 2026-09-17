param(
    [string]$Binary = 'build/d24/RelWithDebInfo',
    [int]$InjectFrame = 400,
    [int]$Blocks = 7,
    [int]$Seconds = 75
)

# Two local peers, one of which deliberately leaks blocks on a chosen frame.
# The point is not to find a defect: it is to watch the D30 heap census produce
# lines, and to check that the caller it prints is a module-relative offset
# rather than a raw ASLR address.

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path '.').Path
$binDir = (Resolve-Path (Join-Path $root $Binary)).Path
$exe = Join-Path $binDir 'partyboard.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "introuvable: $exe" }

$disc = 'C:\Users\valen\Downloads\Mario Party 4 (USA) (Rev 1).rvz'
if (-not (Test-Path -LiteralPath $disc)) { throw "disque introuvable: $disc" }

$run = Join-Path $env:TEMP ('pb-census-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $run -Force | Out-Null
Write-Output "dossier de travail : $run"

$reservation = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
$port = $reservation.Client.LocalEndPoint.Port
$reservation.Dispose()

$peers = @()
$handshake = @()
foreach ($side in 0, 1) {
    $profile = Join-Path $run "profile-$side"
    New-Item -ItemType Directory -Path $profile -Force | Out-Null
    $settings = @{
        'backend.graphicsBackend' = 'd3d12'; 'backend.isoPath' = $disc
        'backend.isoVerification' = 2; 'backend.wasPresetChosen' = $true
        'game.internalResolutionScale' = 1; 'game.shadowResolutionMultiplier' = 1
        'video.targetFrameRate' = 60; 'audio.masterVolume' = 0
    }
    [IO.File]::WriteAllText((Join-Path $profile 'config.json'),
        ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))

    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $exe
    $start.WorkingDirectory = $binDir
    $transport = if ($side -eq 0) { "--netplay-host $port" } else { "--netplay-join 127.0.0.1:$port" }
    $start.Arguments = "$transport --netplay-full --netplay-loopback --netplay-delay 3"
    # Only ONE side leaks. That asymmetry is the whole experiment.
    if ($side -eq 0) { $start.Arguments += " --netplay-inject-heap=${InjectFrame}:${Blocks}" }
    $log = Join-Path $run "peer-$side-stdout.txt"
    $start.FileName = $env:ComSpec
    $start.Arguments = '/c ""' + $exe + '" ' + $start.Arguments + ' > "' + $log + '" 2>&1"'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $prefix = 'Local\PartyBoardOnlineStart-' + [Guid]::NewGuid().ToString('N')
    $ready = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-ready')
    $go = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-go')
    $cancel = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-cancel')
    $handshake += [pscustomobject]@{ Ready = $ready; Go = $go; Cancel = $cancel }
    $start.EnvironmentVariables['PARTYBOARD_ONLINE_READY'] = $prefix + '-ready'
    $start.EnvironmentVariables['PARTYBOARD_ONLINE_GO'] = $prefix + '-go'
    $start.EnvironmentVariables['PARTYBOARD_ONLINE_CANCEL'] = $prefix + '-cancel'
    $start.EnvironmentVariables['PARTYBOARD_ONLINE_DISC'] = $disc
    $start.EnvironmentVariables['PARTYBOARD_NETPLAY_TEST_PROFILE'] = $profile
    $start.EnvironmentVariables['PARTYBOARD_NET_DIAGNOSTIC'] = (Join-Path $run "peer-$side-native.log")
    $start.EnvironmentVariables['PARTYBOARD_CRASH_DIR'] = $run
    $peers += [Diagnostics.Process]::Start($start)
    Write-Output ("pair {0} lance, pid {1}{2}" -f $side, $peers[-1].Id,
        $(if ($side -eq 0) { " (fuite de $Blocks blocs a la frame $InjectFrame)" } else { '' }))
}

# Both peers signal READY when they are initialised; the supervisor releases
# GO. Without it the game exits cleanly at frame 0.
$waitReady = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $waitReady -and
    -not ($handshake[0].Ready.WaitOne(0) -and $handshake[1].Ready.WaitOne(0))) {
    Start-Sleep -Milliseconds 250
}
if ($handshake[0].Ready.WaitOne(0) -and $handshake[1].Ready.WaitOne(0)) {
    foreach ($h in $handshake) { $h.Go.Set() | Out-Null }
    Write-Output 'les deux pairs sont prets, GO envoye'
} else {
    Write-Output 'ATTENTION: un pair n a jamais signale READY'
}
$graceUntil = (Get-Date).AddSeconds(15)   # demarrage d'Aurora, disque, auto-tests
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 1500
    $reports = @(Get-ChildItem -Path $run -Recurse -Filter 'netplay_desync_*.log' -ErrorAction SilentlyContinue)
    if ($reports.Count) { Write-Output "rapport ecrit apres $([int]((Get-Date) - $deadline.AddSeconds(-$Seconds)).TotalSeconds) s"; break }
    if ((Get-Date) -lt $graceUntil) { continue }
    $alive = @(Get-Process -Name partyboard -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($binDir, 'OrdinalIgnoreCase') })
    if ($alive.Count -lt 2) { Write-Output "seulement $($alive.Count) pair(s) en vie"; if ($alive.Count -eq 0) { break } }
}

Get-Process -Name partyboard -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($binDir, 'OrdinalIgnoreCase') } |
    Stop-Process -Force -ErrorAction SilentlyContinue
foreach ($p in $peers) { try { if (-not $p.HasExited) { $p.Kill() } } catch {} }
Start-Sleep -Milliseconds 500
Write-Output "---- resultat ----"
Get-ChildItem -Path $run -Recurse -Filter 'netplay_desync_*.log' -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Output ("  {0}  ({1:N0} octets)" -f $_.FullName, $_.Length) }
Write-Output "RUNPATH=$run"
