# Puts the port on an Android device: installs private\p4au\apk\p4au.apk, copies the game data (analysis\, spirv\,
# extract\romfs\: ~15.5 GB, needs that much free space) to Internal storage/Persona 4 Arena Ultimax and grants
# all-files access. Usage: games\p4au\tools\push_device.ps1 [-Serial HA269JK2] [-SkipData]
param([string]$Serial = "", [switch]$SkipData)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\..\.."
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$dev = if ($Serial) { @("-s", $Serial) } else { @() }
$data = "/storage/emulated/0/Persona 4 Arena Ultimax"
& $adb @dev install -r "$root\private\p4au\apk\p4au.apk"
& $adb @dev shell appops set io.github.p4aurecomp MANAGE_EXTERNAL_STORAGE allow
if (-not $SkipData) {
    & $adb @dev shell "mkdir -p '$data/extract'"
    foreach ($d in "analysis", "spirv") { & $adb @dev push "$root\private\p4au\$d" "$data/" }
    & $adb @dev push "$root\private\p4au\extract\romfs" "$data/extract/"
}
Write-Host "done: open P4AU from the launcher"
