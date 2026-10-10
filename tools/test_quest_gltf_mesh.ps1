$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot -Parent
$taskVswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$taskVs = & $taskVswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'Visual Studio C++ tools not found.' }
$taskVc = (Get-ChildItem "$taskVs/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$taskSdk = "${env:ProgramFiles(x86)}/Windows Kits/10"
$taskVersion = (Get-ChildItem "$taskSdk/Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$taskOutput = Join-Path $taskRepo 'build/quest-gltf-mesh-tests'
New-Item -ItemType Directory -Force $taskOutput | Out-Null
& "$taskVc/bin/Hostx64/x64/cl.exe" /nologo /std:c++20 /EHsc /O2 "/I$taskVc/include" "/I$taskSdk/Include/$taskVersion/ucrt" `
    "/Fe$taskOutput/quest-gltf-mesh-test.exe" "/Fo$taskOutput/quest-gltf-mesh-test.obj" "$taskRepo/tools/tests/quest_gltf_mesh_test.cpp" /link `
    "/LIBPATH:$taskVc/lib/x64" "/LIBPATH:$taskSdk/Lib/$taskVersion/ucrt/x64" "/LIBPATH:$taskSdk/Lib/$taskVersion/um/x64"
if ($LASTEXITCODE -ne 0) { throw 'Quest glTF mesh test compilation failed.' }
& "$taskOutput/quest-gltf-mesh-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Quest glTF mesh regression failed.' }
