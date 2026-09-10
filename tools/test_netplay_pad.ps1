param([string]$BinaryDirectory = 'build/aexp/RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ Build Tools not found.' }
$vcPath = (Get-ChildItem "$installation\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkPath = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = (Get-ChildItem "$sdkPath\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$testOutput = Join-Path $projectPath 'build/netplay-pad-tests'
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
& "$vcPath\bin\Hostx64\x64\cl.exe" /nologo /std:c11 /W3 /O2 /DTARGET_PC /DTARGET_DOL /DVERSION=0 /DMUSY_TARGET=0 `
    "/I$projectPath/include" "/I$projectPath/extern/aurora/include" "/I$projectPath/extern/aurora/include/dolphin" "/I$projectPath/extern" `
    "/I$projectPath/extern/musyx/include" `
    "/I$vcPath/include" "/I$sdkPath/Include/$sdkVersion/ucrt" `
    "/I$sdkPath/Include/$sdkVersion/um" "/I$sdkPath/Include/$sdkVersion/shared" `
    "/Fe$testOutput/netplay_pad_tick_test.exe" "/Fo$testOutput/netplay_pad_tick_test.obj" `
    "$projectPath/tools/tests/netplay_pad_tick_test.c" /link `
    "/LIBPATH:$vcPath/lib/x64" "/LIBPATH:$sdkPath/Lib/$sdkVersion/ucrt/x64" `
    "/LIBPATH:$sdkPath/Lib/$sdkVersion/um/x64"
if ($LASTEXITCODE -ne 0) { throw 'PAD regression compilation failed.' }
& "$testOutput/netplay_pad_tick_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'PAD tick regression failed.' }

$binaryPath = Join-Path $projectPath $BinaryDirectory
function Start-Probe([string]$arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $binaryPath 'partyboard.exe'
    $start.WorkingDirectory = $binaryPath
    $start.Arguments = "$arguments --netplay-full --netplay-pad-probe"
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    return @{ Process = $process; Out = $process.StandardOutput.ReadToEndAsync(); Err = $process.StandardError.ReadToEndAsync() }
}
$cases = @(
    @{ Delay = 0; Extra = '' },
    @{ Delay = 3; Extra = '' },
    @{ Delay = 8; Extra = '' },
    @{ Delay = 0; Extra = '--netplay-probe-context-mismatch' },
    @{ Delay = 3; Extra = '--netplay-probe-context-mismatch' },
    @{ Delay = 3; Extra = '--netplay-probe-disconnect' },
    @{ Delay = 0; Extra = '--netplay-probe-desync' },
    @{ Delay = 3; Extra = '--netplay-probe-desync' },
    @{ Delay = 8; Extra = '--netplay-probe-desync' }
)
foreach ($case in $cases) {
    $delay = $case.Delay
    $extra = $case.Extra
    # Reserve an ephemeral local UDP port, then release it for the host.
    $reservation = [Net.Sockets.UdpClient]::new(0)
    $port = $reservation.Client.LocalEndPoint.Port
    $reservation.Dispose()
    $peers = @()
    try {
        $peers += Start-Probe "--netplay-host $port --netplay-delay $delay --netplay-pad 1 $extra"
        $peers += Start-Probe "--netplay-join 127.0.0.1:$port --netplay-delay $delay --netplay-pad 4 $extra"
        foreach ($peer in $peers) {
            $deadlineMs = if ($extra -eq '--netplay-probe-disconnect') { 145000 } else { 30000 }
            if (-not $peer.Process.WaitForExit($deadlineMs)) { throw 'Netplay PAD probe timed out.' }
            Write-Output $peer.Out.Result
            if ($peer.Err.Result) { Write-Output $peer.Err.Result }
            if ($peer.Process.ExitCode -ne 0) { throw "Netplay PAD probe failed (delay $delay): $($peer.Process.ExitCode)" }
        }
    } finally {
        foreach ($peer in $peers) {
            if (-not $peer.Process.HasExited) { $peer.Process.Kill() }
            $peer.Process.Dispose()
        }
    }
}
