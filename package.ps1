# Builds a portable Sim-QVFR release from a commit: a clean checkout of it in a scratch
# folder (so nothing uncommitted or outside the repo gets in), a Release build, every offline test,
# then dist\sim-qvfr-<version>.zip and its folder.
#   powershell -ExecutionPolicy Bypass -File package.ps1 [-Version 1.0.0-beta] [-Commit HEAD] [-Sign]
# Without -Version it packages the commit's planned release (SIM_QVFR_RELEASE in CMakeLists.txt).
# -Sign Authenticode-signs the launcher and the layer DLL before they are hashed and zipped. The
# key is Certum's cloud key (SimplySign): log in to SimplySign Desktop first, so the certificate
# shows up in the CurrentUser\My store; signtool then asks the cloud to sign.
# DLSS in LMU: -NgxSdk <folder> builds with NVIDIA's DLSS SDK, from github.com/NVIDIA/DLSS and never
# in this repository: its include\, nvsdk_ngx_s.lib, LICENSE.txt and the release nvngx_dlss.dll
# (lib\Windows_x86_64\rel), side by side in one folder. The DLL ships beside the layer with NVIDIA's own signature (not
# re-signed: NGX checks it), NVIDIA's licence in licenses\. Or -NoDlss for a release without DLSS. One of the two is
# required, so no release leaves DLSS out by accident.
#   powershell -ExecutionPolicy Bypass -File package.ps1 -NgxSdk E:\path\to\dlss-sdk [-Sign]
param([string]$Version = '',
      [string]$Commit = 'HEAD',
      [string]$OutputDirectory = '',
      [string]$NgxSdk = '',
      [switch]$NoDlss,
      [string[]]$SkipTest = @(),  # test builds only: ctest names left out of the run (VERSION.txt says so)
      [switch]$Sign,
      [switch]$NoScan,  # local builds only: skips the Windows Defender scan before zipping
      [string]$CertificateThumbprint = '4683DA63C881A76D5A1E803751B481F621ADF9C1',  # Certum, until 2027-11-04
      [string]$TimestampUrl = 'http://time.certum.pl')
$ErrorActionPreference = 'Stop'

# DLSS: the SDK checked before anything is built.
if ($NgxSdk -and $NoDlss) { throw 'Pass -NgxSdk or -NoDlss, not both' }
if (-not $NgxSdk -and -not $NoDlss) {
    throw 'Pass -NgxSdk <folder> (NVIDIA''s DLSS SDK: include\nvsdk_ngx.h, nvsdk_ngx_s.lib, LICENSE.txt, nvngx_dlss.dll) for a release with DLSS, or -NoDlss for one without'
}
if ($NgxSdk) {
    $NgxSdk = [IO.Path]::GetFullPath($NgxSdk)
    foreach ($needed in 'include\nvsdk_ngx.h', 'nvsdk_ngx_s.lib', 'LICENSE.txt', 'nvngx_dlss.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $NgxSdk $needed))) { throw "The DLSS SDK folder $NgxSdk has no $needed" }
    }
    if (-not (Select-String -LiteralPath (Join-Path $NgxSdk 'LICENSE.txt') -Pattern 'NVIDIA RTX SDKs LICENSE' -SimpleMatch -Quiet)) {
        throw "$NgxSdk\LICENSE.txt isn't NVIDIA's RTX SDKs licence"
    }
    # The DLL players get: NVIDIA's, signed by NVIDIA, DLSS 310 or newer (what Sim-QVFR is made for), and one of the
    # SDK's release DLLs (lib\Windows_x86_64\rel). The SDK's development DLL (dev) has the same signature and
    # version, so each release DLL is listed by its git blob hash in github.com/NVIDIA/DLSS (git ls-tree shows it,
    # git hash-object computes it): the list can be checked against NVIDIA's repository. A newer SDK: add its line.
    $dlssReleases = @{
        '875981984f56e71b4fde6605484ef767dadf9910' = '310.9.1'  # commit 3749594, 2026-09-08
    }
    $dlssDll = Join-Path $NgxSdk 'nvngx_dlss.dll'
    $dlssBytes = [IO.File]::ReadAllBytes($dlssDll)
    $blobHeader = [Text.Encoding]::ASCII.GetBytes("blob $($dlssBytes.Length)`0")
    $sha1 = [Security.Cryptography.SHA1]::Create()
    [void]$sha1.TransformBlock($blobHeader, 0, $blobHeader.Length, $null, 0)
    [void]$sha1.TransformFinalBlock($dlssBytes, 0, $dlssBytes.Length)
    $dlssBlob = -join ($sha1.Hash | ForEach-Object { $_.ToString('x2') })
    $sha1.Dispose()
    $dlssBytes = $null
    if (-not $dlssReleases.ContainsKey($dlssBlob)) {
        throw "$dlssDll (git blob $dlssBlob) isn't a release DLL of NVIDIA's DLSS SDK that package.ps1 lists: take lib\Windows_x86_64\rel\nvngx_dlss.dll from an SDK release (not dev) and add its blob hash to `$dlssReleases"
    }
    $dlssSdk = $dlssReleases[$dlssBlob]
    $dlssSignature = Get-AuthenticodeSignature -LiteralPath $dlssDll
    if ($dlssSignature.Status -ne 'Valid' -or $dlssSignature.SignerCertificate.Subject -notmatch 'O=NVIDIA Corporation') {
        throw "$dlssDll isn't signed by NVIDIA ($($dlssSignature.Status))"
    }
    $dlssInfo = (Get-Item -LiteralPath $dlssDll).VersionInfo
    if ($dlssInfo.FileMajorPart -lt 310) { throw "$dlssDll is DLSS $($dlssInfo.FileMajorPart): 310 or newer needed" }
    $dlssVersion = '{0}.{1}.{2}.{3}' -f $dlssInfo.FileMajorPart, $dlssInfo.FileMinorPart, $dlssInfo.FileBuildPart, $dlssInfo.FilePrivatePart
}

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
# Visual Studio's build environment for Ninja and the MSVC x64 tools, checked before anything is checked out.
. (Join-Path $repo 'tools\vs_env.ps1')
Import-VsEnvironment

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
# NVIDIA's DLSS SDK is never in the repository (.gitignore): a commit with any of its files isn't packaged.
$sdkCommitted = Invoke-Native 'git ls-tree' { git -C $repo ls-tree -r --name-only $hash } | Where-Object {
    $_ -match '(^|/)(nvsdk_ngx[^/]*\.(h|lib)|nvngx_[^/]*\.dll|NVIDIA-RTX-SDKs-LICENSE\.txt)$|^third_party/(nvidia-dlss-sdk|ngx-sdk)/'
}
if ($sdkCommitted) { throw "Commit $short has NVIDIA SDK files in it, which must never be committed: $($sdkCommitted -join ', ')" }
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

# Outside the repo's folder tree, so a sibling folder cannot stand in for a missing file. Under %TEMP%, not
# %LOCALAPPDATA%: Defender's MpCmdRun can't scan folders directly under %LOCALAPPDATA% (fails with 0x80508023).
$work = Join-Path (Join-Path $env:TEMP 'sim-qvfr-package') ([Guid]::NewGuid().ToString('N').Substring(0, 8))
Invoke-Native 'git worktree add' { git -C $repo worktree add --detach $work $hash } | Out-Null
try {
    Write-Host "Building $short in $work"
    # The build and the tests at below-normal priority (the compilers and tests inherit it), so a game running in
    # the headset meanwhile keeps its frames.
    $self = [Diagnostics.Process]::GetCurrentProcess()
    $priority = $self.PriorityClass
    $self.PriorityClass = 'BelowNormal'
    try {
        $configure = @('-S', $work, '-B', (Join-Path $work 'build'), '-G', 'Ninja Multi-Config', "-DSIM_QVFR_VERSION=$Version ($short)")
        if ($NgxSdk) { $configure += "-DNGX_SDK_DIR=$NgxSdk" }
        Invoke-Native 'cmake configure' { cmake @configure } | Out-Null
        $build = Invoke-Native 'cmake build' { cmake --build (Join-Path $work 'build') --config Release }
        $warnings = $build | Where-Object { $_ -match ': warning ' }
        if ($warnings) { $warnings | ForEach-Object { Write-Host $_ }; throw 'The build has warnings' }

        # Every offline test, side by side, each with its own TEMP folder and log (the ctest suite in CMakeLists.txt).
        # -SkipTest: ctest leaves those tests out.
        $ctestSkip = @()
        if ($SkipTest) {
            foreach ($t in $SkipTest) {
                if ($t -notmatch '^[a-z0-9_]+_test$') { throw "-SkipTest takes test names (name_test), not '$t'" }
                Write-Warning "Test $t is SKIPPED in this package"
            }
            $ctestSkip = @('-E', ('^(' + ($SkipTest -join '|') + ')$'))
        }
        $tests = Invoke-Native 'ctest' {
            ctest --test-dir (Join-Path $work 'build') -C Release -j $env:NUMBER_OF_PROCESSORS --output-on-failure --no-tests=error @ctestSkip
        }
        $tests | Where-Object { $_ -match 'Test +#|tests passed' } | ForEach-Object { Write-Host "  $($_.ToString().Trim())" }
    } finally {
        $self.PriorityClass = $priority
    }

    New-Item -ItemType Directory -Path $stage, (Join-Path $stage 'layer'), (Join-Path $stage 'layer\helmet'), (Join-Path $stage 'layer\vulkan'), (Join-Path $stage 'licenses') | Out-Null
    $files = [ordered]@{
        'build\Release\sim-qvfr-launcher.exe'              = 'sim-qvfr-launcher.exe'
        'build\Release\sim-qvfr.dll'                       = 'layer\sim-qvfr.dll'
        'manifest\XR_APILAYER_SIM_qvfr.json'               = 'layer\XR_APILAYER_SIM_qvfr.json'
        'manifest\VkLayer_SIM_qvfr_early.json'             = 'layer\vulkan\VkLayer_SIM_qvfr_early.json'
        'assets\helmet\top.png'                            = 'layer\helmet\top.png'
        'assets\helmet\bottom.png'                         = 'layer\helmet\bottom.png'
        'assets\helmet\visor.png'                          = 'layer\helmet\visor.png'
        'manifest\sim-qvfr.ini'                            = 'sim-qvfr.ini'
        'packaging\README.txt'                             = 'README.txt'
        'docs\LMU-OPTIMIZATION-GUIDE.md'                   = 'LMU-OPTIMIZATION-GUIDE.md'
        'LICENSE.md'                                       = 'LICENSE.md'
        'THIRD_PARTY_NOTICES.md'                           = 'THIRD_PARTY_NOTICES.md'
        'third_party\minhook\LICENSE.txt'                  = 'licenses\MinHook-LICENSE.txt'
        'third_party\openxr\LICENSES\Apache-2.0.txt'       = 'licenses\OpenXR-Apache-2.0.txt'
        'third_party\openxr\LICENSES\MIT.txt'              = 'licenses\OpenXR-MIT.txt'
        'third_party\fidelityfx-cas\LICENSE.txt'           = 'licenses\FidelityFX-CAS-LICENSE.txt'
        'third_party\fonts\jost\OFL.txt'                   = 'licenses\Jost-OFL.txt'
        'third_party\fonts\inter\LICENSE.txt'              = 'licenses\Inter-OFL.txt'
    }
    foreach ($source in $files.Keys) {
        Copy-Item -LiteralPath (Join-Path $work $source) -Destination (Join-Path $stage $files[$source])
    }
    # DLSS: the layer has NGX in it exactly when it was asked for (NGX's library names its entry points in its own
    # strings, which Sim-QVFR's code never spells out), and NVIDIA's licence goes with it (the RTX SDKs licence's
    # distribution terms; THIRD_PARTY_NOTICES.md).
    $layerBytes = [IO.File]::ReadAllBytes((Join-Path $stage 'layer\sim-qvfr.dll'))
    $hasNgx = [Text.Encoding]::ASCII.GetString($layerBytes).Contains('NVSDK_NGX_D3D11_Init')
    if ($NgxSdk -and -not $hasNgx) { throw 'Built with -NgxSdk, but the layer has no NGX in it (CMake did not take the SDK)' }
    if ($NoDlss -and $hasNgx) { throw 'Built with -NoDlss, but the layer has NGX in it' }
    if ($NgxSdk) {
        Copy-Item -LiteralPath (Join-Path $NgxSdk 'LICENSE.txt') -Destination (Join-Path $stage 'licenses\NVIDIA-RTX-SDKs-LICENSE.txt')
        Copy-Item -LiteralPath $dlssDll -Destination (Join-Path $stage 'layer\nvngx_dlss.dll')
        Write-Host "  DLSS: built with the NGX SDK; NVIDIA's nvngx_dlss.dll $dlssVersion (DLSS SDK $dlssSdk) in layer\, NVIDIA's licence in licenses\"
    } else {
        Write-Host '  DLSS: not in this release (-NoDlss)'
    }
    # Of the SDK, only what its licence lets a release carry: its library compiled into sim-qvfr.dll, nvngx_dlss.dll
    # and the licence. No header, no .lib, no other NVIDIA file.
    $dlssShipped = Join-Path $stage 'layer\nvngx_dlss.dll'
    $sdkStaged = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
        $_.Extension -in '.h', '.hpp', '.lib' -or $_.Name -like 'nvsdk_ngx*' -or
        ($_.Name -like 'nvngx_*' -and $_.FullName -ne $dlssShipped)
    }
    if ($sdkStaged) { throw "The package has SDK files it must never carry: $($sdkStaged.FullName -join ', ')" }
    if ($Sign) {
        # Signed before SHA256.txt is written, so its hashes are of the signed files. Ours only: nvngx_dlss.dll keeps
        # NVIDIA's signature.
        $binaries = @('sim-qvfr-launcher.exe', 'layer\sim-qvfr.dll') | ForEach-Object { Join-Path $stage $_ }
        foreach ($binary in $binaries) {
            Invoke-Native "signing $binary" {
                & $signtool sign /sha1 $CertificateThumbprint /fd sha256 /tr $TimestampUrl /td sha256 /d 'Sim-QVFR' $binary
            } | Out-Null
            Invoke-Native "verifying $binary" { & $signtool verify /pa $binary } | Out-Null
            Write-Host "  signed $(Split-Path -Leaf $binary)"
        }
    }
    $dlssLine = if ($NgxSdk) { "DLSS: built with NVIDIA's DLSS SDK $dlssSdk, its nvngx_dlss.dll $dlssVersion included" } else { 'DLSS: not built in' }
    $versionText = "Sim-QVFR $Version`r`ncommit $hash`r`n$dlssLine"
    if ($SkipTest) { $versionText += "`r`ntests skipped: $($SkipTest -join ', ')" }
    Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Value $versionText -Encoding ASCII
    Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length).TrimStart('\', '/').Replace('\', '/')
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $relative
    } | Set-Content -LiteralPath (Join-Path $stage 'SHA256.txt') -Encoding ASCII
    # Windows Defender's verdict on what ships and on the build the suite ran (this repository's
    # tools\defender_scan.ps1, whichever commit is packaged): a flagged file stops the package, false positive or not.
    $scanner = Join-Path $PSScriptRoot 'tools\defender_scan.ps1'
    foreach ($scanned in $(if ($NoScan) { @() } else { @($stage, (Join-Path $work 'build\Release')) })) {
        & $scanner -Folder $scanned
        if ($LASTEXITCODE -ne 0) { throw "Windows Defender flagged a file under $scanned (defender_scan above)" }
    }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive
} finally {
    Invoke-Native 'git worktree remove' { git -C $repo worktree remove --force $work } | Out-Null
}
Write-Output "Package folder:  $stage"
Write-Output "Package archive: $archive"
