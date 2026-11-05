# Registers sim-qvfr.dll as an implicit OpenXR API layer for the current user only (HKCU), so
# Assetto Corsa EVO loads it when started normally from Steam. The DLL is inert in every other
# OpenXR app. Undo with Unregister-ProbeLayer.ps1.
#
# -Machine registers machine-wide (HKLM; run as administrator) instead and removes the per-user entry.
# The OpenXR loader puts HKLM layers above HKCU ones, so this is needed to sit above another machine-wide
# layer such as VectorXR; set the order between them in the VectorXR app's layer order.
#
# To skip the layer for one launch without unregistering: set DISABLE_XR_APILAYER_SIM_qvfr=1.
[CmdletBinding()]
param(
    [string]$Manifest = "",
    [switch]$Machine
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 can leave $PSScriptRoot empty in param() defaults, so resolve it here.
if (-not $Manifest) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $Manifest = Join-Path $scriptDir "..\build\Release\XR_APILAYER_SIM_qvfr.json"
}

$manifestPath = (Resolve-Path $Manifest).Path
if (-not (Test-Path (Join-Path (Split-Path $manifestPath) "sim-qvfr.dll"))) {
    throw "sim-qvfr.dll not found next to $manifestPath. Build first."
}
$userKey = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
$key = if ($Machine) { "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" } else { $userKey }
# Never New-Item -Force an existing key: on the registry provider it deletes and recreates the key, wiping
# every other layer's entry.
if (-not (Test-Path $key)) {
    New-Item -Path $key -Force | Out-Null  # -Force only creates missing parents here
}
# OpenXR convention: DWORD 0 means enabled.
New-ItemProperty -Path $key -Name $manifestPath -PropertyType DWord -Value 0 -Force | Out-Null
if ($Machine -and (Get-ItemProperty -Path $userKey -Name $manifestPath -ErrorAction SilentlyContinue)) {
    Remove-ItemProperty -Path $userKey -Name $manifestPath
    Write-Host "Removed the per-user entry (the layer would otherwise be listed twice)"
}
Write-Host "Registered ($(if ($Machine) { 'HKLM' } else { 'HKCU' }) implicit): $manifestPath"
Write-Host "Probe log: $(Join-Path (Split-Path $manifestPath) 'logs\sim-qvfr.log')"
