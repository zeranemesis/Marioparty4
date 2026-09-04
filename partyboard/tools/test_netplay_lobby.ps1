$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ Build Tools not found.' }
$vcPath = (Get-ChildItem "$installation\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkPath = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = (Get-ChildItem "$sdkPath\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$testOutput = Join-Path $projectPath 'build\netplay-lobby-tests'
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
& "$vcPath\bin\Hostx64\x64\cl.exe" /nologo /std:c++20 /EHsc /W4 /WX /O2 `
    "/I$projectPath\include" "/I$vcPath\include" "/I$sdkPath\Include\$sdkVersion\ucrt" `
    "/Fe$testOutput\netplay_lobby_test.exe" "/Fo$testOutput\netplay_lobby_test.obj" `
    "$projectPath\tools\tests\netplay_lobby_test.cpp" /link `
    "/LIBPATH:$vcPath\lib\x64" "/LIBPATH:$sdkPath\Lib\$sdkVersion\ucrt\x64" `
    "/LIBPATH:$sdkPath\Lib\$sdkVersion\um\x64"
if ($LASTEXITCODE -ne 0) { throw "Lobby test compilation failed: $LASTEXITCODE" }
& "$testOutput\netplay_lobby_test.exe"
if ($LASTEXITCODE -ne 0) { throw "Lobby state tests failed: $LASTEXITCODE" }
