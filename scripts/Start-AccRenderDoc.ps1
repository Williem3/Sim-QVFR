# Starts ACC under RenderDoc exactly as the launcher would start it: the launcher's dry run writes this launch's
# run folder (the layer manifest pointing at build\Release\sim-qvfr.dll, the settings snapshot) and prints the
# variables; this script sets them and has renderdoccmd start the game with them, so RenderDoc hooks D3D11 from
# the first frame. Captures (F12 or PrtScn in the game window, or qrenderdoc's File > Attach to Running Instance)
# land on the Desktop as acc_<date>_<time>_frame<N>.rdc.
#
#   powershell -ExecutionPolicy Bypass -File scripts\Start-AccRenderDoc.ps1 [-Build Release] [-CaptureName acc]
param(
    [string]$Build = 'Release',
    [string]$CaptureName = 'acc',
    [string]$RenderDoc = 'C:\Program Files\RenderDoc\renderdoccmd.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo "build\$Build"
$launcher = Join-Path $bin 'sim-qvfr-launcher.exe'
foreach ($p in $launcher, $RenderDoc) { if (-not (Test-Path $p)) { throw "missing: $p" } }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running; start it first' }
if (Get-Process AC2-Win64-Shipping -ErrorAction SilentlyContinue) { throw 'ACC is already running; close it first' }

# The launcher is a GUI-subsystem exe: `&` neither waits for it nor captures its output, so redirect to a file.
$planFile = Join-Path $env:TEMP 'sim-qvfr-renderdoc-plan.txt'
Start-Process -FilePath $launcher -ArgumentList '--dry-run' -WorkingDirectory $bin -Wait -NoNewWindow `
    -RedirectStandardOutput $planFile
$plan = Get-Content $planFile
$plan | ForEach-Object { Write-Host $_ }
$game = ($plan | Where-Object { $_ -match '^\s+game:\s+(.+)$' } | Select-Object -First 1) -replace '^\s+game:\s+', ''
if (-not $game -or -not (Test-Path $game)) { throw "no game path in the launcher's plan" }
$set = 0
foreach ($line in $plan) {
    if ($line -match '^\s+set ([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        $set++
    }
}
if ($set -eq 0) { throw "no variables in the launcher's plan" }

$captures = Join-Path ([Environment]::GetFolderPath('Desktop')) $CaptureName
Write-Host ""
Write-Host "Starting ACC under RenderDoc ($set variables set); captures: $captures*.rdc"
& $RenderDoc capture -d (Split-Path -Parent $game) -c $captures $game -vr hmd=SteamVR
