# Compares two copies of a game's graphics settings (game-settings.txt in a launch's run folder, game-settings-<time>.txt
# in a report's zip; src/launcher/game_settings_snapshot.h): the settings whose values differ, each with both values and
# their names. Settings only one copy has are listed too.
#   powershell -File tools\compare_game_settings.ps1 <ours.txt> <theirs.txt>
param(
    [Parameter(Mandatory = $true)][string]$Ours,
    [Parameter(Mandatory = $true)][string]$Theirs
)

function Read-Settings([string]$path) {
    $out = [ordered]@{}
    $section = ''
    foreach ($line in [System.IO.File]::ReadAllLines($path, [System.Text.Encoding]::UTF8)) {
        if ($line.StartsWith(';') -or $line.Trim() -eq '') { continue }
        if ($line.StartsWith('[')) { $section = $line; continue }
        $eq = $line.IndexOf('=')
        if ($eq -lt 0) { continue }
        $out["$section $($line.Substring(0, $eq))"] = $line.Substring($eq + 1)
    }
    return $out
}

$a = Read-Settings $Ours
$b = Read-Settings $Theirs
$differ = 0
foreach ($key in @($a.Keys) + @($b.Keys | Where-Object { -not $a.Contains($_) })) {
    $va = if ($a.Contains($key)) { $a[$key] } else { '(not in this copy)' }
    $vb = if ($b.Contains($key)) { $b[$key] } else { '(not in this copy)' }
    if ($va -ne $vb) {
        $differ++
        Write-Output $key
        Write-Output "    ours:   $va"
        Write-Output "    theirs: $vb"
    }
}
Write-Output "$differ setting(s) differ"
