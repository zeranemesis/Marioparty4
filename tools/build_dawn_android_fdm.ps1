# Builds Dawn for the Quest (Android arm64) with patches/dawn-fragment-density-map.patch,
# at Aurora's Dawn revision, and packages it like Aurora's prebuilt Dawn, for
# a game build with the eyes' fragment density map (foveation):
#
#   ./tools/build_dawn_android_fdm.ps1             # -> C:/mp4w/dawn-src/dawn-android-fdm.tar.gz
#   cmake --preset android-arm64 -B build/android-arm64-quest-fdm `
#       -DAURORA_DAWN_PROVIDER=package -DAURORA_DAWN_PACKAGE_URL=C:/mp4w/dawn-src/dawn-android-fdm.tar.gz `
#       -DAURORA_DAWN_FOVEATION=ON
#
# Needs git, Python 3 (`py -3`), Ninja and the NDK (ANDROID_HOME, NDK 29.0.14206865).
param(
    [string]$Work = 'C:/mp4w/dawn-src',
    [string]$Ndk = "$env:LOCALAPPDATA/Android/Sdk/ndk/29.0.14206865",
    [string]$Ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$ref = (Select-String -Path "$repo/extern/aurora/CMakeLists.txt" -Pattern 'AURORA_DAWN_REF "([0-9a-f]+)"').Matches[0].Groups[1].Value
$src = "$Work/dawn-$ref"
New-Item -ItemType Directory -Force $Work | Out-Null

if (-not (Test-Path "$src/CMakeLists.txt")) {
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri "https://github.com/google/dawn/archive/$ref.tar.gz" -OutFile "$Work/dawn.tar.gz" -UseBasicParsing
    tar -xzf "$Work/dawn.tar.gz" -C $Work # a macOS-only symlink may fail to extract: harmless
    Push-Location $src
    py -3 tools/fetch_dawn_dependencies.py
    if ($LASTEXITCODE -ne 0) { throw 'fetch_dawn_dependencies.py failed' }
    git apply --whitespace=nowarn "$repo/patches/dawn-fragment-density-map.patch"
    if ($LASTEXITCODE -ne 0) { throw 'the fragment density map patch does not apply' }
    Pop-Location
}

$build = "$Work/build-android"
$install = "$Work/install-android"
$ErrorActionPreference = 'Continue'
cmake -G Ninja -S $src -B $build "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DCMAKE_TOOLCHAIN_FILE=$Ndk/build/cmake/android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$install" `
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DDAWN_ENABLE_INSTALL=ON -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF `
    -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DDAWN_BUILD_BENCHMARKS=OFF -DDAWN_ENABLE_VULKAN=ON `
    -DDAWN_ENABLE_OPENGLES=ON -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_NULL=ON -DDAWN_USE_GLFW=OFF `
    -DDAWN_SUPPORTS_GLFW_FOR_WINDOWING=OFF -DDAWN_FETCH_DEPENDENCIES=OFF `
    -DDAWN_BUILD_PROTOBUF=OFF -DTINT_BUILD_IR_BINARY=OFF # protoc would be built for Android and run here
if ($LASTEXITCODE -ne 0) { throw 'Dawn configure failed' }
cmake --build $build --target install
if ($LASTEXITCODE -ne 0) { throw 'Dawn build failed' }
& "$Ndk/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objcopy.exe" --strip-debug "$install/lib/libwebgpu_dawn.a"
tar -czf "$Work/dawn-android-fdm.tar.gz" -C $install .
Write-Output "Package: $Work/dawn-android-fdm.tar.gz"
