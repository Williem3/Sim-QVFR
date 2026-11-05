# Starts the launcher's game (the launcher's Game tab, [launcher] game) under RenderDoc exactly as the launcher
# would start it: the launcher's dry run writes this launch's run folder (the layer manifest pointing at
# build\Release\sim-qvfr.dll, the settings snapshot) and prints the game, its arguments and the variables; this
# script sets them and has renderdoccmd start the game with them, so RenderDoc hooks the game's graphics API from
# the first frame. Captures (F12 or PrtScn in the game window, or qrenderdoc's File > Attach to Running Instance)
# land on the Desktop as <CaptureName>_<date>_<time>_frame<N>.rdc (CaptureName: the game's exe name by default).
#
# -ExtraArgs appends to the game's arguments (X-Plane's --gfx_validation turns on the Vulkan SDK's validation
# layer, whose reports X-Plane writes to its Log.txt); -NoRenderDoc starts the game the same way without RenderDoc.
#
#   powershell -ExecutionPolicy Bypass -File scripts\Start-RenderDoc.ps1 [-Build Release] [-CaptureName acc]
#       [-ExtraArgs '--gfx_validation'] [-NoRenderDoc]
param(
    [string]$Build = 'Release',
    [string]$CaptureName = '',
    [string]$ExtraArgs = '',
    [switch]$NoRenderDoc,
    [string]$RenderDoc = 'C:\Program Files\RenderDoc\renderdoccmd.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo "build\$Build"
$launcher = Join-Path $bin 'sim-qvfr-launcher.exe'
foreach ($p in @($launcher) + @(if (-not $NoRenderDoc) { $RenderDoc })) { if (-not (Test-Path $p)) { throw "missing: $p" } }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running; start it first' }

# The launcher is a GUI-subsystem exe: `&` neither waits for it nor captures its output, so redirect to a file.
$planFile = Join-Path $env:TEMP 'sim-qvfr-renderdoc-plan.txt'
Start-Process -FilePath $launcher -ArgumentList '--dry-run' -WorkingDirectory $bin -Wait -NoNewWindow `
    -RedirectStandardOutput $planFile
$plan = Get-Content $planFile
$plan | ForEach-Object { Write-Host $_ }
$game = ($plan | Where-Object { $_ -match '^\s+game:\s+(.+)$' } | Select-Object -First 1) -replace '^\s+game:\s+', ''
if (-not $game -or -not (Test-Path $game)) { throw "no game path in the launcher's plan" }
$gameArgs = ($plan | Where-Object { $_ -match '^\s+args:\s+(.*)$' } | Select-Object -First 1) -replace '^\s+args:\s+', ''
$exeName = [IO.Path]::GetFileNameWithoutExtension($game)
if (Get-Process $exeName -ErrorAction SilentlyContinue) { throw "$exeName is already running; close it first" }
if ($ExtraArgs) { $gameArgs = "$gameArgs $ExtraArgs".Trim() }
if (-not $CaptureName) { $CaptureName = $exeName -replace '[^A-Za-z0-9]', '' }
$set = 0
foreach ($line in $plan) {
    if ($line -match '^\s+set ([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        $set++
    }
}
if ($set -eq 0) { throw "no variables in the launcher's plan" }

$argList = @($gameArgs -split ' ' | Where-Object { $_ })
if ($NoRenderDoc) {
    Write-Host ""
    Write-Host "Starting $exeName without RenderDoc ($set variables set, arguments: $gameArgs)"
    Start-Process -FilePath $game -ArgumentList $argList -WorkingDirectory (Split-Path -Parent $game)
    return
}
$captures = Join-Path ([Environment]::GetFolderPath('Desktop')) $CaptureName
Write-Host ""
Write-Host "Starting $exeName under RenderDoc ($set variables set, arguments: $gameArgs); captures: $captures*.rdc"
& $RenderDoc capture -d (Split-Path -Parent $game) -c $captures $game @argList
