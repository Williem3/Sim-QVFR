# Builds Sim-QVFR (Release) into build\Release with Ninja and the MSVC x64 tools, from any PowerShell: it loads
# Visual Studio's build environment itself (tools\vs_env.ps1). -Test then runs the offline suite side by side;
# -Filter runs only the tests whose names match (and implies -Test).
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Test] [-Filter lmu] [-Target sim_qvfr_launcher] [-Jobs 8]
# The build and the tests run at below-normal priority, so a game in the headset meanwhile keeps its frames.
# A build folder made by another generator (the Visual Studio one) is switched to Ninja by removing only CMake's
# cache (CMakeCache.txt and CMakeFiles\): build\Release keeps the development launcher's settings, logs and profiles.
param([switch]$Test,
      [string]$Filter = '',
      [string]$Target = '',
      [int]$Jobs = 0,
      [string]$BuildDirectory = '',
      [switch]$NoScan)
$ErrorActionPreference = 'Stop'
$repo = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo 'build' }
if ($Filter) { $Test = $true }
. (Join-Path $repo 'tools\vs_env.ps1')
Import-VsEnvironment

$generator = 'Ninja Multi-Config'
$cache = Join-Path $BuildDirectory 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    $text = Get-Content -LiteralPath $cache
    $made = ($text | Where-Object { $_ -like 'CMAKE_GENERATOR:INTERNAL=*' } | Select-Object -First 1) -replace '^[^=]*=', ''
    $compiler = ($text | Where-Object { $_ -like 'CMAKE_CXX_COMPILER:*=*' } | Select-Object -First 1) -replace '^[^=]*=', ''
    $reason = if ($made -ne $generator) { "it was made by the '$made' generator" }
              elseif ($compiler -and -not (Test-Path -LiteralPath $compiler)) { "its compiler is gone (a Visual Studio update): $compiler" }
    if ($reason) {
        Write-Host "build.ps1: configuring $BuildDirectory again for $generator, since $reason (only CMake's cache is removed)"
        Remove-Item -LiteralPath $cache
        $files = Join-Path $BuildDirectory 'CMakeFiles'
        if (Test-Path -LiteralPath $files) { Remove-Item -LiteralPath $files -Recurse -Force }
    }
}

$self = [Diagnostics.Process]::GetCurrentProcess()
$priority = $self.PriorityClass
$self.PriorityClass = 'BelowNormal'  # the compilers and tests inherit it
try {
    if (-not (Test-Path -LiteralPath $cache)) {
        cmake -S $repo -B $BuildDirectory -G $generator -DCMAKE_DEFAULT_BUILD_TYPE=Release
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
    }
    $arguments = @('--build', $BuildDirectory, '--config', 'Release')
    if ($Target) { $arguments += @('--target', $Target) }
    if ($Jobs -gt 0) { $arguments += @('--parallel', $Jobs) }
    cmake @arguments
    if ($LASTEXITCODE -ne 0) { throw "the build failed (exit $LASTEXITCODE)" }
    if ($Test) {
        $arguments = @('--test-dir', $BuildDirectory, '-C', 'Release', '-j', $env:NUMBER_OF_PROCESSORS, '--output-on-failure')
        if ($Filter) { $arguments += @('-R', $Filter) }
        ctest @arguments
        if ($LASTEXITCODE -ne 0) { throw "tests failed (exit $LASTEXITCODE)" }
    }
    # Windows Defender's verdict on what was built (tools\defender_scan.ps1): a flagged binary fails the build, false
    # positive or not. -NoScan skips it (then run the script before committing).
    if (-not $NoScan) {
        & (Join-Path $repo 'tools\defender_scan.ps1') -Folder (Join-Path $BuildDirectory 'Release')
        if ($LASTEXITCODE -ne 0) { throw 'Windows Defender flagged a binary of the build (defender_scan above)' }
    }
} finally {
    $self.PriorityClass = $priority
}
