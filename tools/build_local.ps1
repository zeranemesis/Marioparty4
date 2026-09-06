param([string]$BuildDirectory = 'build/aexp', [string]$Configuration = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$cache = Join-Path $projectPath "$BuildDirectory/CMakeCache.txt"
$cmakeLine = Get-Content -LiteralPath $cache | Where-Object { $_ -like 'CMAKE_COMMAND:INTERNAL=*' }
if (-not $cmakeLine) { throw 'Configure the build directory with CMake first.' }
$cmake = $cmakeLine.Substring('CMAKE_COMMAND:INTERNAL='.Length)
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $cmake
$start.WorkingDirectory = $projectPath
$start.UseShellExecute = $false
$start.Arguments = "--build `"$BuildDirectory`" --config $Configuration --target partyboard --parallel 4 -- /verbosity:quiet /nologo"
# Some launch environments contain both Path and PATH. MSBuild's .NET
# Framework child-process launcher requires case-insensitive unique names.
$environment = [Environment]::GetEnvironmentVariables()
$start.Environment.Clear()
$seen = @{}
foreach ($key in $environment.Keys) {
    if (-not $seen.ContainsKey($key)) {
        $start.Environment[$key] = $environment[$key]
        $seen[$key] = $true
    }
}
$process = [Diagnostics.Process]::Start($start)
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "CMake build failed: $($process.ExitCode)" }
if ($BuildDirectory -eq 'build/aexp' -and $Configuration -eq 'RelWithDebInfo') {
    & (Join-Path $PSScriptRoot 'build_online.ps1')
}
