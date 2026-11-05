# Builds a portable Sim-QVFR release from a commit: a clean checkout of it in a scratch
# folder (so nothing uncommitted or outside the repo gets in), a Release build, every offline test,
# then dist\sim-qvfr-<version>.zip and its folder.
#   powershell -ExecutionPolicy Bypass -File package.ps1 [-Version 1.0.0-sim-qvfr-beta] [-Commit HEAD]
# Without -Version it packages the commit's planned release (SIM_QVFR_RELEASE in CMakeLists.txt).
param([string]$Version = '',
      [string]$Commit = 'HEAD',
      [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'dist' }

function Invoke-Native([string]$What, [scriptblock]$Command) {
    # Native tools write progress to stderr; only the exit code decides.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = & $Command 2>&1 } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "$What failed (exit $LASTEXITCODE)"
    }
    return $output
}

$hash = (Invoke-Native 'git rev-parse' { git -C $repo rev-parse --verify "$Commit^{commit}" } | Select-Object -Last 1).ToString().Trim()
$short = $hash.Substring(0, 7)
if (Invoke-Native 'git status' { git -C $repo status --porcelain --untracked-files=no }) {
    Write-Warning "The working tree has uncommitted changes; they are NOT in this package (it is built from $short)."
}
if (-not $Version) {
    $cmake = (Invoke-Native 'git show' { git -C $repo show "${hash}:CMakeLists.txt" }) -join "`n"
    if ($cmake -notmatch 'set\(SIM_QVFR_RELEASE "([^"]+)"\)') { throw "No SIM_QVFR_RELEASE in $short's CMakeLists.txt; pass -Version" }
    $Version = $Matches[1]
}
if ($Version -notmatch '^[0-9A-Za-z.\-]+$') { throw "Version '$Version' may only hold letters, digits, dots and dashes" }
Write-Host "Packaging Sim-QVFR $Version"
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$name = "sim-qvfr-$Version"
$stage = Join-Path $OutputDirectory $name
$archive = "$stage.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)) { throw "$stage(.zip) already exists" }

# Outside the repo's folder tree (so a sibling folder cannot stand in for a missing file) and not
# under %TEMP%, which MSBuild warns about (MSB8029).
$work = Join-Path (Join-Path $env:LOCALAPPDATA 'sim-qvfr-package') ([Guid]::NewGuid().ToString('N').Substring(0, 8))
Invoke-Native 'git worktree add' { git -C $repo worktree add --detach $work $hash } | Out-Null
try {
    Write-Host "Building $short in $work"
    Invoke-Native 'cmake configure' { cmake -S $work -B (Join-Path $work 'build') -A x64 "-DSIM_QVFR_VERSION=$Version ($short)" } | Out-Null
    $build = Invoke-Native 'cmake build' { cmake --build (Join-Path $work 'build') --config Release }
    $warnings = $build | Where-Object { $_ -match ': warning ' }
    if ($warnings) { $warnings | ForEach-Object { Write-Host $_ }; throw 'The build has warnings' }
    $release = Join-Path $work 'build\Release'

    # Every offline test, with a fresh log (some tests read it back).
    $env:SIM_QVFR_LOG = Join-Path $work 'tests.log'
    foreach ($test in Get-ChildItem -LiteralPath $release -Filter '*_test.exe') {
        $null = & $test.FullName 2>&1
        if ($LASTEXITCODE -ne 0) { throw "$($test.Name) failed (exit $LASTEXITCODE)" }
        Write-Host "  $($test.Name) passed"
    }
    Remove-Item Env:SIM_QVFR_LOG

    New-Item -ItemType Directory -Path $stage, (Join-Path $stage 'layer'), (Join-Path $stage 'layer\helmet'), (Join-Path $stage 'licenses') | Out-Null
    $files = [ordered]@{
        'build\Release\sim-qvfr-launcher.exe'              = 'sim-qvfr-launcher.exe'
        'build\Release\sim-qvfr.dll'                       = 'layer\sim-qvfr.dll'
        'manifest\XR_APILAYER_SIM_qvfr.json'               = 'layer\XR_APILAYER_SIM_qvfr.json'
        'assets\helmet\top.png'                            = 'layer\helmet\top.png'
        'assets\helmet\bottom.png'                         = 'layer\helmet\bottom.png'
        'assets\helmet\visor.png'                          = 'layer\helmet\visor.png'
        'manifest\sim-qvfr.ini'                            = 'sim-qvfr.ini'
        'packaging\README.txt'                             = 'README.txt'
        'LICENSE.md'                                       = 'LICENSE.md'
        'THIRD_PARTY_NOTICES.md'                           = 'THIRD_PARTY_NOTICES.md'
        'third_party\minhook\LICENSE.txt'                  = 'licenses\MinHook-LICENSE.txt'
        'third_party\openxr\LICENSES\Apache-2.0.txt'       = 'licenses\OpenXR-Apache-2.0.txt'
        'third_party\openxr\LICENSES\MIT.txt'              = 'licenses\OpenXR-MIT.txt'
    }
    foreach ($source in $files.Keys) {
        Copy-Item -LiteralPath (Join-Path $work $source) -Destination (Join-Path $stage $files[$source])
    }
    Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Value "Sim-QVFR $Version`r`ncommit $hash" -Encoding ASCII
    Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length).TrimStart('\', '/').Replace('\', '/')
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $relative
    } | Set-Content -LiteralPath (Join-Path $stage 'SHA256.txt') -Encoding ASCII
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive
} finally {
    Invoke-Native 'git worktree remove' { git -C $repo worktree remove --force $work } | Out-Null
}
Write-Output "Package folder:  $stage"
Write-Output "Package archive: $archive"
