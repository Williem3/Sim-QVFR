# Import-VsEnvironment: loads the x64 MSVC build environment (cl, link, rc, the Windows SDK, and the CMake and Ninja
# that come with Visual Studio) into this PowerShell process, for build.ps1 and package.ps1. Dot-source this file,
# then call it. Inside an x64 developer shell it does nothing.
# vcvars64.bat takes about 3 s, so what it changes (only that, never the rest of the environment) is kept in
# %LOCALAPPDATA%\sim-qvfr-dev\vs-environment.txt and read again only when Visual Studio or its MSVC version changes.
function Import-VsEnvironment {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue) -and
        (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
        return
    }
    $help = 'Install Visual Studio 2026 or its Build Tools with "Desktop development with C++", including "C++ CMake tools for Windows" (CONTRIBUTING.md, Build).'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "No Visual Studio installer (vswhere.exe). $help" }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
    if (-not $vs) { throw "No Visual Studio with the MSVC x64 tools and the C++ CMake tools. $help" }
    $vs = "$vs".Trim()
    $tools = (Get-Content -LiteralPath (Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') | Select-Object -First 1).Trim()
    $key = "$vs|$tools"
    $cache = Join-Path $env:LOCALAPPDATA 'sim-qvfr-dev\vs-environment.txt'

    $changes = $null
    if (Test-Path -LiteralPath $cache) {
        $cached = @(Get-Content -LiteralPath $cache)
        if ($cached.Count -gt 1 -and $cached[0] -eq $key) { $changes = $cached[1..($cached.Count - 1)] }
    }
    if (-not $changes) {
        $before = @{}
        Get-ChildItem Env: | ForEach-Object { $before[$_.Name] = $_.Value }
        $oldPath = $env:PATH -split ';'
        $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
        $after = cmd /c "`"$vcvars`" >nul 2>nul && set"
        if ($LASTEXITCODE -ne 0) { throw "$vcvars failed (exit $LASTEXITCODE)" }
        $changes = foreach ($line in $after) {
            if ($line -notmatch '^([^=]+)=(.*)$') { continue }
            $name = $Matches[1]
            $value = $Matches[2]
            if ($name -eq 'Path') {
                'PATH+=' + (($value -split ';' | Where-Object { $_ -and $oldPath -notcontains $_ }) -join ';')
            } elseif ($before[$name] -ne $value) {
                "$name=$value"
            }
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $cache) | Out-Null
        Set-Content -LiteralPath $cache -Value (@($key) + $changes) -Encoding UTF8
    }
    foreach ($change in $changes) {
        if ($change.StartsWith('PATH+=')) {
            $env:PATH = $change.Substring(6) + ';' + $env:PATH
        } elseif ($change -match '^([^=]+)=(.*)$') {
            Set-Item -LiteralPath "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
    foreach ($tool in 'cl.exe', 'ninja.exe') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $cache -ErrorAction SilentlyContinue  # read vcvars64.bat again next time
            throw "$tool is not in Visual Studio's build environment ($vs). $help"
        }
    }
}
