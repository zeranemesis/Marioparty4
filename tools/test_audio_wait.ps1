$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ Build Tools not found.' }
$vcPath = (Get-ChildItem "$installation\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkPath = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = (Get-ChildItem "$sdkPath\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
# The regression source lives in the MusyX submodule and is not tracked there,
# so it never reached patches/musyx-partyboard.patch and a fresh checkout does
# not have it - the same way synth_wait.h went missing. Say so instead of
# failing on a compiler error that names no cause.
$source = Join-Path $projectPath 'extern\musyx\test\wait_ms_regression.c'
if (-not (Test-Path -LiteralPath $source)) {
    Write-Output "Missing $source; it is in no patch and in no commit, so only its author has it."
    exit 2
}

$testOutput = Join-Path $projectPath 'build\audio-wait-tests'
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
& "$vcPath\bin\Hostx64\x64\cl.exe" /nologo /std:clatest /O2 /DNDEBUG `
    /DMUSY_TARGET=0 /DMUSY_VERSION_MAJOR=1 /DMUSY_VERSION_MINOR=5 /DMUSY_VERSION_PATCH=4 `
    "/I$projectPath\extern\musyx\include" "/I$vcPath\include" "/I$sdkPath\Include\$sdkVersion\ucrt" `
    "/Fe$testOutput\wait_ms_regression.exe" "/Fo$testOutput\wait_ms_regression.obj" `
    "$source" /link `
    "/LIBPATH:$vcPath\lib\x64" "/LIBPATH:$sdkPath\Lib\$sdkVersion\ucrt\x64" `
    "/LIBPATH:$sdkPath\Lib\$sdkVersion\um\x64"
if ($LASTEXITCODE -ne 0) { throw "Audio wait test compilation failed: $LASTEXITCODE" }
& "$testOutput\wait_ms_regression.exe"
if ($LASTEXITCODE -ne 0) { throw "Audio wait regression failed: $LASTEXITCODE" }
