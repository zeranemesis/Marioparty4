param([switch]$CompileOnly, [string]$Abi = "arm64-v8a", [string]$Serial, [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk")
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot -Parent
$taskAdb = Join-Path $Sdk 'platform-tools/adb.exe'
function Check-Exit([string]$step) {
    if ($LASTEXITCODE -ne 0) { throw "$step failed ($LASTEXITCODE)" }
}
# What this needs and a machine may simply not have - the Android SDK, the NDK,
# a headset on USB - ends the script with exit code 2, which run_all_tests.ps1
# reports as "this environment cannot run me" rather than as a failure. A
# release gate must not fail on a Windows machine for want of a headset.
function Not-Applicable([string]$reason) { Write-Output "not applicable: $reason"; exit 2 }
if (-not $CompileOnly -and -not (Test-Path -LiteralPath $taskAdb)) {
    Not-Applicable "no adb at $taskAdb; pass -Sdk, or -CompileOnly to build without a headset"
}
if (-not $CompileOnly -and -not $Serial) {
    $taskDevices = @(& $taskAdb devices | Where-Object { $_ -match '^([^\s]+)\s+device$' })
    if ($taskDevices.Count -eq 0) { Not-Applicable 'no headset connected over adb' }
    if ($taskDevices.Count -ne 1) { throw 'Specify -Serial when several devices are connected.' }
    $Serial = ($taskDevices[0] -split '\s+')[0]
}
$taskAbi = $Abi
if (-not $CompileOnly) {
    $taskAbi = (& $taskAdb -s $Serial shell getprop ro.product.cpu.abi).Trim()
    Check-Exit 'Device ABI query'
}
$taskTriple = switch ($taskAbi) {
    'arm64-v8a' { 'aarch64-linux-android28' }
    'x86_64' { 'x86_64-linux-android28' }
    default { throw "Unsupported ABI: $taskAbi" }
}
$taskNdk = Join-Path $Sdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin'
$taskCompiler = Join-Path $taskNdk "$taskTriple-clang++.cmd"
if (-not (Test-Path -LiteralPath $taskCompiler)) { Not-Applicable "no NDK compiler at $taskCompiler" }
# Gradle resolves the OpenXR headers through Prefab; reuse that exact version.
$taskNinja = Get-ChildItem (Join-Path $taskRepo 'platforms/android/app/.cxx/Debug') -Filter build.ninja -Recurse |
    Where-Object { $_.Directory.Name -eq $taskAbi } | Select-Object -First 1
if (-not $taskNinja) { throw 'Build assembleQuestDebug once to resolve the OpenXR headers.' }
$taskIncludeLine = Get-Content $taskNinja.FullName | Where-Object { $_ -match '^  INCLUDES = -isystem (.+/modules/headers/include)$' } | Select-Object -First 1
if (-not $taskIncludeLine) { throw 'OpenXR Prefab include path missing from the configured build.' }
$taskHeaders = $taskIncludeLine.Substring('  INCLUDES = -isystem '.Length)
$taskOutput = Join-Path $taskRepo 'build/quest-tests'
New-Item -ItemType Directory -Force $taskOutput | Out-Null
$taskExecutable = Join-Path $taskOutput "stereo-lifecycle-$taskAbi"
& $taskCompiler -std=c++20 -ffunction-sections -fdata-sections '-Wl,--gc-sections' -I $taskHeaders -I (Join-Path $taskRepo 'platforms/android/app/src/main/cpp') (Join-Path $taskRepo 'tools/tests/quest_stereo_lifecycle_test.cpp') (Join-Path $taskRepo 'platforms/android/app/src/main/cpp/stereo_view.cpp') -o $taskExecutable -llog -lEGL -lGLESv3 -landroid -static-libstdc++
Check-Exit 'Native test build'
if ($CompileOnly) { Write-Output 'Native lifecycle test compiled; execution requires an Android device.'; exit 0 }
& $taskAdb -s $Serial push $taskExecutable /data/local/tmp/partyboard-stereo-test
Check-Exit 'Native test upload'
& $taskAdb -s $Serial shell chmod 700 /data/local/tmp/partyboard-stereo-test
Check-Exit 'Native test permissions'
& $taskAdb -s $Serial shell /data/local/tmp/partyboard-stereo-test
Check-Exit 'Native lifecycle test'
