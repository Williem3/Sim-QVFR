# Windows Defender's verdict on what a build made. A requirement: no binary of Sim-QVFR's, shipped or a test's, may be
# flagged, false positive or not (a flagged test exe vanishes from the suite; a flagged release file reaches players).
#
#   tools\defender_scan.ps1 [-Folder <dir>] [-Since <time>]
#
# Two checks, either failing it (exit 1):
#   - a custom scan of the folder, reporting only (MpCmdRun -DisableRemediation: nothing quarantined by it);
#   - Defender's detection history for files under the folder since -Since (default: 24 hours back): what real-time
#     protection quarantined meanwhile (during the build or the suite) and the cloud's later verdicts. A detection
#     counts while the file it named is gone or no newer than the detection (rebuilt since: superseded, listed only).
# Defender's cloud can flag a file minutes after a first clean scan: run this again ten or more minutes after the last
# link, before committing or packaging (build.ps1 runs it after every build; package.ps1 before it zips).
# Reading Defender's verdicts changes none of its settings.
param([string]$Folder = '',
      [datetime]$Since = (Get-Date).AddHours(-24))
$ErrorActionPreference = 'Stop'
if (-not $Folder) { $Folder = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release' }
$Folder = (Resolve-Path -LiteralPath $Folder).Path.TrimEnd('\')
$mpcmd = Join-Path $env:ProgramFiles 'Windows Defender\MpCmdRun.exe'
if (-not (Test-Path -LiteralPath $mpcmd)) {
    Write-Host "defender_scan: no MpCmdRun.exe ($mpcmd): Windows Defender not here, nothing checked"
    exit 0
}

$flagged = @()
# 1. The scan (exit code 2 when it found something).
$scan = & $mpcmd -Scan -ScanType 3 -File $Folder -DisableRemediation 2>&1
$scanCode = $LASTEXITCODE
if ($scanCode -eq 2 -or ($scan | Select-String -SimpleMatch 'found' | Select-String -NotMatch 'found no threats')) {
    $flagged += ($scan | Where-Object { $_ -match 'Threat|file:|found' } | ForEach-Object { "scan: $_" })
    if (-not $flagged) { $flagged += "scan: MpCmdRun exit $scanCode" }
}

# 2. The history: detections of files under the folder since -Since, unless the file was rebuilt after.
$superseded = @()
try {
    $detections = Get-MpThreatDetection -ErrorAction Stop | Where-Object { $_.InitialDetectionTime -ge $Since }
} catch {
    $detections = @()
    Write-Host "defender_scan: Defender's detection history not readable ($($_.Exception.Message)); the scan alone"
}
foreach ($d in $detections) {
    $name = $null
    try { $name = (Get-MpThreat -ThreatID $d.ThreatID -ErrorAction Stop).ThreatName } catch { $name = "threat $($d.ThreatID)" }
    foreach ($resource in $d.Resources) {
        $path = $resource -replace '^[a-z]+:_', ''
        if (-not $path.StartsWith($Folder + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
        $file = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $line = '{0:yyyy-MM-dd HH:mm:ss} {1}: {2}' -f $d.InitialDetectionTime, $name, $path
        # The exe-per-test builds' programs (<name>_test.exe, <name>_check.exe) are no longer built: CMake removes
        # them from a build folder that has the one test program, so one that is gone there was not quarantined.
        $retired = -not $file -and $path -match '_(test|check)\.exe$' -and
                   (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $path) 'sim_qvfr_tests.exe'))
        if ($retired) {
            Write-Host ('defender_scan: retired (no longer built): {0}, detected {1:yyyy-MM-dd HH:mm} {2}' -f
                        (Split-Path -Leaf $path), $d.InitialDetectionTime, $name)
        } elseif ($file -and $file.LastWriteTime -gt $d.InitialDetectionTime) {
            $superseded += [pscustomobject]@{ Path = $path; Time = $d.InitialDetectionTime; Name = $name }
        } else {
            $flagged += "history: $line$(if (-not $file) { ' (gone: quarantined)' })"
        }
    }
}

if ($superseded) {  # one line per file: how many detections, the last one
    $superseded | Group-Object Path | ForEach-Object {
        $last = $_.Group | Sort-Object Time | Select-Object -Last 1
        Write-Host ('defender_scan: superseded (rebuilt since): {0}, {1} detection(s), the last {2:yyyy-MM-dd HH:mm} {3}' -f
                    (Split-Path -Leaf $_.Name), $_.Count, $last.Time, $last.Name)
    }
}
if ($flagged) {
    Write-Host "defender_scan: FLAGGED under $Folder"
    $flagged | ForEach-Object { Write-Host "  $_" }
    Write-Host '  Find what in the binary looks like malware to Defender and change that; never an exclusion. Seen so far:'
    Write-Host '  the linker''s default manifest on small test exes (CMakeLists.txt gives them version info and'
    Write-Host '  /MANIFEST:NO); rdtsc timing around a loop sweeping memory to flush the caches (a side channel''s shape).'
    exit 1
}
Write-Host "defender_scan: clean ($Folder; history since $('{0:yyyy-MM-dd HH:mm}' -f $Since))"
exit 0
