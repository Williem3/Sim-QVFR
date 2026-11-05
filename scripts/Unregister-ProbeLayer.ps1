# Removes the implicit registration written by Register-ProbeLayer.ps1: per-user (HKCU) and, when present,
# machine-wide (HKLM; removing that one needs an administrator).
[CmdletBinding()]
param(
    [string]$Manifest = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $Manifest) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $Manifest = Join-Path $scriptDir "..\build\Release\XR_APILAYER_SIM_qvfr.json"
}

$manifestPath = [System.IO.Path]::GetFullPath($Manifest)
foreach ($key in "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit", "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit") {
    if (Get-ItemProperty -Path $key -Name $manifestPath -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -Path $key -Name $manifestPath
        Write-Host "Unregistered ($key): $manifestPath"
    } else {
        Write-Host "Not registered ($key): $manifestPath"
    }
}
