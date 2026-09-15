param(
    [string]$Replay = '',
    [string]$ExtraArgs = '',
    [Parameter(Mandatory = $true)][int]$Context,
    # Staging copy on purpose: a run keeps dol.dll open, and building into the
    # same directory then fails with LNK1168. Refresh it before a run when the
    # latest build matters.
    [string]$Binary = 'build/install-d24',
    [string]$Disc = 'C:\Users\valen\Downloads\Mario Party 4 (USA) (Rev 1).rvz',
    [int]$Seconds = 420,
    [int]$Shots = 6,
    [int]$ShotGapSeconds = 4,
    [int]$ResolutionScale = 1,
    # Simulation frames to photograph, comma separated. When set, the shots are
    # taken when the traced frame passes each value instead of on a timer, so
    # two runs can be compared at the same moment rather than the same second.
    [string]$AtFrames = ''
)

# Drives a scripted local pair to a chosen overlay and photographs it.
#
# Rendering defects are the one class this project cannot detect: the canonical
# hash excludes presentation by construction, so no campaign, however long, can
# ever see a misplaced texture. Until now the only evidence was a human saying
# what he saw. This produces a picture instead.
#
# It uses a local PAIR rather than one offline instance because the input
# machinery (--netplay-replay-input) is a netplay feature, and because a pair
# reproduces the settings netplay forces - 1x internal resolution, letterbox -
# which is what the player is actually looking at when he reports a defect.

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path '.').Path
$binDir = (Resolve-Path (Join-Path $root $Binary)).Path
$exe = Join-Path $binDir 'partyboard.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "introuvable: $exe" }
if (-not (Test-Path -LiteralPath $Disc)) { throw "disque introuvable: $Disc" }
$replayPath = if ($Replay) { (Resolve-Path $Replay).Path } else { '' }

$run = Join-Path $env:TEMP ('pb-shot-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $run -Force | Out-Null
Write-Output "dossier : $run"

$reservation = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
$port = $reservation.Client.LocalEndPoint.Port
$reservation.Dispose()

$before = @(Get-Process -Name partyboard -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Id)
$handshake = @()
foreach ($side in 0, 1) {
    $profile = Join-Path $run "profile-$side"
    New-Item -ItemType Directory -Path $profile -Force | Out-Null
    $settings = @{
        'backend.graphicsBackend' = 'd3d12'; 'backend.isoPath' = $Disc
        'backend.isoVerification' = 2; 'backend.wasPresetChosen' = $true
        'game.internalResolutionScale' = $ResolutionScale
        'game.shadowResolutionMultiplier' = 1
        'video.targetFrameRate' = 60; 'audio.masterVolume' = 0
    }
    [IO.File]::WriteAllText((Join-Path $profile 'config.json'),
        ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))

    $prefix = 'Local\PartyBoardOnlineStart-' + [Guid]::NewGuid().ToString('N')
    $handshake += [pscustomobject]@{
        Ready  = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-ready')
        Go     = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-go')
        Cancel = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $prefix + '-cancel')
    }

    $transport = if ($side -eq 0) { "--netplay-host $port" } else { "--netplay-join 127.0.0.1:$port" }
    $args = "$transport --netplay-full --netplay-loopback --netplay-delay 3"
    if ($replayPath) { $args += ' --netplay-replay-input "' + $replayPath + '"' }
    if ($ExtraArgs) { $args += ' ' + $ExtraArgs }
    $log = Join-Path $run "peer-$side-stdout.txt"

    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $env:ComSpec
    $start.Arguments = '/c ""' + $exe + '" ' + $args + ' > "' + $log + '" 2>&1"'
    $start.WorkingDirectory = $binDir
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.EnvironmentVariables['PARTYBOARD_ONLINE_READY'] = $prefix + '-ready'
    $start.EnvironmentVariables['PARTYBOARD_ONLINE_GO'] = $prefix + '-go'
    $start.EnvironmentVariables['PARTYBOARD_ONLINE_CANCEL'] = $prefix + '-cancel'
    $start.EnvironmentVariables['PARTYBOARD_ONLINE_DISC'] = $Disc
    $start.EnvironmentVariables['PARTYBOARD_NETPLAY_TEST_PROFILE'] = $profile
    $start.EnvironmentVariables['PARTYBOARD_NET_DIAGNOSTIC'] = (Join-Path $run "peer-$side-native.log")
    $start.EnvironmentVariables['PARTYBOARD_CRASH_DIR'] = $run
    [Diagnostics.Process]::Start($start) | Out-Null
}

# The games appear a few seconds after their cmd wrappers; whatever is new
# against the snapshot belongs to this run and to no other.
$mine = @()
$deadlineOwn = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $deadlineOwn -and $mine.Count -lt 2) {
    Start-Sleep -Milliseconds 500
    $mine = @(Get-Process -Name partyboard -ErrorAction SilentlyContinue |
        Where-Object { $before -notcontains $_.Id } | Select-Object -ExpandProperty Id)
}
Write-Output "processus de ce run : $($mine -join ', ')"

$waitReady = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $waitReady -and -not ($handshake[0].Ready.WaitOne(0) -and $handshake[1].Ready.WaitOne(0))) {
    Start-Sleep -Milliseconds 250
}
if (-not ($handshake[0].Ready.WaitOne(0) -and $handshake[1].Ready.WaitOne(0))) {
    Write-Output 'ECHEC: un pair n a jamais signale READY'
} else {
    foreach ($h in $handshake) { $h.Go.Set() | Out-Null }
    Write-Output 'GO envoye'
}

$native = Join-Path $run 'peer-0-native.log'
$deadline = (Get-Date).AddSeconds($Seconds)
$reached = $false
$seenContexts = @{}
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 2000
    if (-not (Test-Path -LiteralPath $native)) { continue }
    $last = Get-Content -LiteralPath $native -Tail 1 -ErrorAction SilentlyContinue
    if (-not $last) { continue }
    $m = [regex]::Match($last, ' context=(\d+)')
    if (-not $m.Success) { continue }
    $ctx = [int]$m.Groups[1].Value
    if ($ctx -ne 4294967295 -and -not $seenContexts.ContainsKey($ctx)) {
        $seenContexts[$ctx] = $true
        Write-Output "  contexte $ctx"
    }
    # -Context 0 means: stop at the first MINIGAME overlay, whichever it is.
    # Overlays 4..70 are the minigame modules; 1/3/72/74 are boot and menus.
    if ($Context -eq 0) {
        if ($ctx -ge 4 -and $ctx -le 70) { $reached = $true; $Context = $ctx; break }
    } elseif ($ctx -eq $Context) { $reached = $true; break }
}

if ($reached -and $AtFrames) {
    Write-Output "contexte $Context atteint, captures aux frames $AtFrames"
    foreach ($target in ($AtFrames -split ',')) {
        $want = [int]$target.Trim()
        $limit = (Get-Date).AddSeconds(600)
        while ((Get-Date) -lt $limit) {
            $last = Get-Content -LiteralPath $native -Tail 1 -ErrorAction SilentlyContinue
            $fm = if ($last) { [regex]::Match($last, ' frame=(\d+)') } else { $null }
            if ($fm -and $fm.Success -and [int]$fm.Groups[1].Value -ge $want) { break }
            Start-Sleep -Milliseconds 400
        }
        $shot = Join-Path $run ("frame{0}.png" -f $want)
        & (Join-Path $root 'tools/capture_fenetre.ps1') -Out $shot -PathPrefix $binDir
    }
} elseif ($reached) {
    Write-Output "contexte $Context atteint, captures..."
    for ($i = 0; $i -lt $Shots; $i++) {
        $shot = Join-Path $run ("ctx{0}-{1:d2}.png" -f $Context, $i)
        & (Join-Path $root 'tools/capture_fenetre.ps1') -Out $shot -PathPrefix $binDir
        Start-Sleep -Seconds $ShotGapSeconds
    }
} else {
    Write-Output "contexte $Context JAMAIS atteint. Contextes vus : $($seenContexts.Keys -join ', ')"
    $shot = Join-Path $run 'dernier-ecran.png'
    & (Join-Path $root 'tools/capture_fenetre.ps1') -Out $shot -PathPrefix $binDir
}

foreach ($id in $mine) {
    Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
}
Write-Output "RUNPATH=$run"
