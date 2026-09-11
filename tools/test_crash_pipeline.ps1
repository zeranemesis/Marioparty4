# Builds and runs the crash manifest and uploader self-tests on their own.
#
# Both modules depend on nothing from the game, so this needs no dol.dll and
# runs in seconds - including while a campaign is holding the game's DLL open.
# The same self-tests also run inside partyboard.exe under --netplay-self-test.

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -prerelease -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ Build Tools not found.' }
$vcPath = (Get-ChildItem "$installation\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkPath = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = (Get-ChildItem "$sdkPath\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name

# nlohmann/json comes from the CMake build tree; the modules are header-only
# consumers of it, so a configured build directory is all this needs.
$json = Join-Path $projectPath 'build/aexp/_deps/json-src/include'
if (-not (Test-Path -LiteralPath $json)) {
    throw "nlohmann/json headers not found at $json. Configure build/aexp first."
}

$output = Join-Path $projectPath 'build/crash-pipeline-test'
New-Item -ItemType Directory -Force -Path $output | Out-Null
& "$vcPath\bin\Hostx64\x64\cl.exe" /nologo /std:c++20 /EHsc /W3 /O2 /D_CRT_SECURE_NO_WARNINGS `
    "/I$projectPath\include" "/I$json" `
    "/I$vcPath\include" "/I$sdkPath\Include\$sdkVersion\ucrt" `
    "/I$sdkPath\Include\$sdkVersion\um" "/I$sdkPath\Include\$sdkVersion\shared" `
    "/Fe$output\crash_pipeline_test.exe" "/Fo$output\\" `
    "$projectPath\tools\tests\crash_pipeline_main.cpp" `
    "$projectPath\src\port\crash_manifest.cpp" `
    "$projectPath\src\port\crash_uploader.cpp" `
    /link "/LIBPATH:$vcPath\lib\x64" "/LIBPATH:$sdkPath\Lib\$sdkVersion\ucrt\x64" `
    "/LIBPATH:$sdkPath\Lib\$sdkVersion\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'Crash pipeline test compilation failed.' }

& "$output\crash_pipeline_test.exe"
$code = $LASTEXITCODE
if ($code -ne 0) { throw "Crash pipeline self-tests failed with exit code $code." }
exit 0
