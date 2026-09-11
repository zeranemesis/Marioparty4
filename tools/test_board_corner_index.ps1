# Compiles and runs the D1 corner-index test.
#
# The test is a transcription of the arithmetic in boo_event.c:1047-1060, so it
# needs nothing from the game and runs in a second. It answers what can be
# answered offline: which occupancies make the index leave its four-entry table.
# It does NOT answer whether those occupancies occur in a match; the detector
# compiled into BoardSpaceCornerPosGet answers that one.

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -prerelease -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ Build Tools not found.' }
$vcPath = (Get-ChildItem "$installation\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkPath = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = (Get-ChildItem "$sdkPath\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name

$output = Join-Path $projectPath 'build/d1-test'
New-Item -ItemType Directory -Force -Path $output | Out-Null
& "$vcPath\bin\Hostx64\x64\cl.exe" /nologo /std:c11 /W4 /O2 `
    "/I$vcPath/include" "/I$sdkPath/Include/$sdkVersion/ucrt" `
    "/I$sdkPath/Include/$sdkVersion/um" "/I$sdkPath/Include/$sdkVersion/shared" `
    "/Fe$output/board_corner_index_test.exe" "/Fo$output/board_corner_index_test.obj" `
    "$projectPath/tools/tests/board_corner_index_test.c" /link `
    "/LIBPATH:$vcPath/lib/x64" "/LIBPATH:$sdkPath/Lib/$sdkVersion/ucrt/x64" `
    "/LIBPATH:$sdkPath/Lib/$sdkVersion/um/x64"
if ($LASTEXITCODE -ne 0) { throw 'D1 corner index test compilation failed.' }

& "$output/board_corner_index_test.exe"
$code = $LASTEXITCODE
if ($code -ne 0) { throw "D1 corner index test failed with exit code $code." }
exit 0
