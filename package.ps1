# Builds a portable ace-quadview release from a commit: a clean checkout of it in a scratch
# folder (so nothing uncommitted or outside the repo gets in), a Release build, every offline test,
# then dist\ace-quadview-<version>.zip and its folder.
#   powershell -ExecutionPolicy Bypass -File package.ps1 -Version 2026.11.1.4-alpha [-Commit HEAD] [-Sign]
# -Sign Authenticode-signs the launcher and the layer DLL before they are hashed and zipped. The
# key is Certum's cloud key (SimplySign): log in to SimplySign Desktop first, so the certificate
# shows up in the CurrentUser\My store; signtool then asks the cloud to sign.
param([Parameter(Mandatory = $true)][ValidatePattern('^[0-9A-Za-z.\-]+$')][string]$Version,
      [string]$Commit = 'HEAD',
      [string]$OutputDirectory = '',
      [switch]$Sign,
      [string]$CertificateThumbprint = '4683DA63C881A76D5A1E803751B481F621ADF9C1',  # Certum, until 2027-11-04
      [string]$TimestampUrl = 'http://time.certum.pl')
$ErrorActionPreference = 'Stop'

if ($Sign) {
    # Checked before the build, so a missing login fails in seconds, not after the tests.
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $signtool = Get-ChildItem -LiteralPath $kits -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) { throw "signtool.exe not found under $kits (install the Windows SDK)" }
    $signtool = $signtool.FullName
    $certificate = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Thumbprint -eq $CertificateThumbprint }
    if (-not $certificate) {
        throw "Certificate $CertificateThumbprint is not in CurrentUser\My. Log in to SimplySign Desktop (its tray icon) and try again."
    }
    if ($certificate.NotAfter -lt (Get-Date)) { throw "Certificate $CertificateThumbprint expired on $($certificate.NotAfter)" }
}
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
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$name = "ace-quadview-$Version"
$stage = Join-Path $OutputDirectory $name
$archive = "$stage.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)) { throw "$stage(.zip) already exists" }

# Outside the repo's folder tree (so a sibling folder cannot stand in for a missing file) and not
# under %TEMP%, which MSBuild warns about (MSB8029).
$work = Join-Path (Join-Path $env:LOCALAPPDATA 'ace-quadview-package') ([Guid]::NewGuid().ToString('N').Substring(0, 8))
Invoke-Native 'git worktree add' { git -C $repo worktree add --detach $work $hash } | Out-Null
try {
    Write-Host "Building $short in $work"
    Invoke-Native 'cmake configure' { cmake -S $work -B (Join-Path $work 'build') -A x64 "-DACE_QV_VERSION=$Version ($short)" } | Out-Null
    $build = Invoke-Native 'cmake build' { cmake --build (Join-Path $work 'build') --config Release }
    $warnings = $build | Where-Object { $_ -match ': warning ' }
    if ($warnings) { $warnings | ForEach-Object { Write-Host $_ }; throw 'The build has warnings' }
    $release = Join-Path $work 'build\Release'

    # Every offline test, with a fresh log (some tests read it back).
    $env:ACE_QV_LOG = Join-Path $work 'tests.log'
    foreach ($test in Get-ChildItem -LiteralPath $release -Filter '*_test.exe') {
        $null = & $test.FullName 2>&1
        if ($LASTEXITCODE -ne 0) { throw "$($test.Name) failed (exit $LASTEXITCODE)" }
        Write-Host "  $($test.Name) passed"
    }
    Remove-Item Env:ACE_QV_LOG

    New-Item -ItemType Directory -Path $stage, (Join-Path $stage 'ace'), (Join-Path $stage 'ace\helmet'), (Join-Path $stage 'licenses') | Out-Null
    $files = [ordered]@{
        'build\Release\ace-quadview-launcher.exe'          = 'ace-quadview-launcher.exe'
        'build\Release\ace-quadview.dll'                   = 'ace\ace-quadview.dll'
        'manifest\XR_APILAYER_ACE_quadview.json'           = 'ace\XR_APILAYER_ACE_quadview.json'
        'assets\helmet\top.png'                            = 'ace\helmet\top.png'
        'assets\helmet\bottom.png'                         = 'ace\helmet\bottom.png'
        'assets\helmet\visor.png'                          = 'ace\helmet\visor.png'
        'manifest\ace-quadview.ini'                        = 'ace-quadview.ini'
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
    if ($Sign) {
        # Signed before SHA256.txt is written, so its hashes are of the signed files.
        $binaries = @('ace-quadview-launcher.exe', 'ace\ace-quadview.dll') | ForEach-Object { Join-Path $stage $_ }
        foreach ($binary in $binaries) {
            Invoke-Native "signing $binary" {
                & $signtool sign /sha1 $CertificateThumbprint /fd sha256 /tr $TimestampUrl /td sha256 /d 'Sim-QuadView' $binary
            } | Out-Null
            Invoke-Native "verifying $binary" { & $signtool verify /pa $binary } | Out-Null
            Write-Host "  signed $(Split-Path -Leaf $binary)"
        }
    }
    Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Value "ace-quadview $Version`r`ncommit $hash" -Encoding ASCII
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
