# Checks the headset's instanced stereo (both eyes in one draw) on this PC,
# with no headset: Aurora's own shader generator and uniform code write a
# scene's WGSL and uniform blocks (tools/tests/stereo_render/harness.cpp),
# Dawn validates every generated shader (validate.cpp) and draws the scene
# per eye and instanced (render.cpp), and the two images must match.
#
# Needs Visual Studio's C++ tools, a GPU with clip distances, and an Android
# Quest build tree for the dependency sources (build/android-arm64-quest).
# Dawn for Windows, the version Aurora ships, is downloaded once into
# build/dawn-windows.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$tests = Join-Path $PSScriptRoot 'tests/stereo_render'
$aurora = Join-Path $repo 'extern/aurora'
$deps = Join-Path $repo 'build/android-arm64-quest/_deps'
$out = Join-Path $repo 'build/quest-stereo-render'
# A missing Quest build tree is a prerequisite this machine lacks, not a
# failure: exit 2 is run_all_tests.ps1's "this environment cannot run me".
if (-not (Test-Path "$deps/fmt-src")) {
    Write-Output 'not applicable: configure build/android-arm64-quest once, its sources are reused here'
    exit 2
}
New-Item -ItemType Directory -Force $out, "$out/shaders", "$out/scene" | Out-Null

$version = (Select-String -Path "$aurora/CMakeLists.txt" -Pattern 'AURORA_DAWN_VERSION "([^"]+)"').Matches[0].Groups[1].Value
$dawn = Join-Path $repo "build/dawn-windows/$version"
if (-not (Test-Path "$dawn/bin/webgpu_dawn.dll")) {
    New-Item -ItemType Directory -Force $dawn | Out-Null
    $url = "https://github.com/encounter/dawn-build/releases/download/$version/dawn-windows-amd64.tar.gz"
    Invoke-WebRequest -Uri $url -OutFile "$dawn/dawn.tar.gz" -UseBasicParsing
    tar -xzf "$dawn/dawn.tar.gz" -C $dawn
    if ($LASTEXITCODE -ne 0) { throw "Unable to unpack $url" }
}

$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools not found.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$flags = '/nologo /std:c++latest /EHsc /O1 /MD /utf-8 /Zc:preprocessor /DNOMINMAX'
$auroraFlags = "$flags /DWEBGPU_DAWN /DAURORA /DTARGET_PC /DAURORA_ENABLE_GX " +
    "/I`"$aurora/include`" /I`"$aurora/lib`" /I`"$deps/dawn_prebuilt-src/include`" /I`"$deps/fmt-src/include`" " +
    "/I`"$deps/abseil-cpp-src`" /I`"$deps/tracy-src/public`" /I`"$deps/xxhash-src`" /I`"$deps/sdl-src/include`""
$absl = "$deps/abseil-cpp-src/absl"
$harnessSources = "`"$tests/harness.cpp`" `"$tests/stubs.cpp`" `"$aurora/lib/gx/shader.cpp`" `"$aurora/lib/gx/shader_info.cpp`" " +
    "`"$deps/fmt-src/src/format.cc`" `"$deps/xxhash-src/xxhash.c`" `"$absl/container/internal/raw_hash_set.cc`" " +
    "`"$absl/hash/internal/hash.cc`" `"$absl/base/internal/raw_logging.cc`" `"$absl/hash/internal/low_level_hash.cc`" " +
    "`"$absl/hash/internal/city.cc`""
$dawnFlags = "$flags /I`"$dawn/include`""
$dawnLib = "`"$dawn/lib/webgpu_dawn.lib`""
$script = @"
@echo off
call "$vcvars" >nul
cd /d "$out"
cl $auroraFlags $harnessSources /Fe:harness.exe /link /OPT:REF || exit /b 1
cl $dawnFlags "$tests/validate.cpp" /Fe:validate.exe /link $dawnLib || exit /b 1
cl $dawnFlags "$tests/render.cpp" /Fe:render.exe /link $dawnLib || exit /b 1
"@
Set-Content -Path "$out/build.cmd" -Value $script -Encoding ascii
# From here native tools report through their exit codes; Dawn and vcvars
# write harmless warnings on stderr, which Windows PowerShell would make fatal.
$ErrorActionPreference = 'Continue'
cmd /c "`"$out/build.cmd`" > `"$out/build.log`" 2>&1"
if ($LASTEXITCODE -ne 0) { Get-Content "$out/build.log" | Select-String ' error ' | Select-Object -First 10; throw 'Build failed.' }
Copy-Item "$dawn/bin/*.dll" $out -Force

& "$out/harness.exe" "$out/shaders"
if ($LASTEXITCODE -ne 0) { throw 'Shader generation failed.' }
& "$out/validate.exe" @(Get-ChildItem "$out/shaders/*.wgsl" | ForEach-Object FullName) 2>$null
if ($LASTEXITCODE -ne 0) { throw 'Generated shaders rejected by Dawn.' }
& "$out/harness.exe" "$out/scene" scene
& "$out/render.exe" "$out/scene" 2>$null
if ($LASTEXITCODE -ne 0) { throw 'Instanced stereo does not match per-eye rendering.' }
Write-Output 'PASS: generated shaders valid, instanced stereo identical to per-eye rendering'
