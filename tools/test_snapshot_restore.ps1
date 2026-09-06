$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ Build Tools not found.' }
$vcPath = (Get-ChildItem "$installation\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkPath = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = (Get-ChildItem "$sdkPath\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$testOutput = Join-Path $projectPath 'build/snapshot-tests'
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
& "$vcPath\bin\Hostx64\x64\cl.exe" /nologo /std:c11 /W3 /O2 /Gy /DTARGET_PC /DTARGET_DOL /DVERSION=0 /DMUSY_TARGET=0 `
    "/I$projectPath/include" "/I$projectPath/extern/aurora/include" "/I$projectPath/extern/aurora/include/dolphin" "/I$projectPath/extern" `
    "/I$projectPath/extern/musyx/include" `
    "/I$vcPath/include" "/I$sdkPath/Include/$sdkVersion/ucrt" `
    "/I$sdkPath/Include/$sdkVersion/um" "/I$sdkPath/Include/$sdkVersion/shared" `
    "/Fe$testOutput/snapshot_restore_test.exe" "/Fo$testOutput/snapshot_restore_test.obj" `
    "$projectPath/tools/tests/snapshot_restore_test.c" /link /OPT:REF `
    "/LIBPATH:$vcPath/lib/x64" "/LIBPATH:$sdkPath/Lib/$sdkVersion/ucrt/x64" `
    "/LIBPATH:$sdkPath/Lib/$sdkVersion/um/x64"
if ($LASTEXITCODE -ne 0) { throw 'Snapshot regression compilation failed.' }
& "$testOutput/snapshot_restore_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Snapshot regression failed.' }


