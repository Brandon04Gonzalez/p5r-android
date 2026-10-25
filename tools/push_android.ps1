# Copy your own game data to the phone (the app's external files folder), install the APK and start it.
#   tools\push_android.ps1 [-Lean] [-Resolution 2340x1080] [-SkipData]
# -Lean leaves out the fonts of languages other than English (~45 MB); everything else is needed by the game
# (Movies are the characters' Bink animations, Audio the FMOD banks, Win/Packages the 720p textures).
param([switch]$Lean, [string]$Resolution = "", [switch]$SkipData)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$adb = "C:\Android\sdk\platform-tools\adb.exe"
$pkg = "io.github.belmantegu.hadesrecomp"
$dst = "/sdcard/Android/data/$pkg/files"

if (-not (& $adb devices | Select-String "`tdevice$")) { throw "no device: connect the phone with USB debugging on" }
& $adb install -r "$root\private\apk\hades.apk"
& $adb shell mkdir -p "$dst/analysis" "$dst/extract" "$dst/user"

if (-not $SkipData) {
    & $adb push "$root\private\analysis\main.bin" "$root\private\analysis\sdk.bin" "$dst/analysis/"
    if ($Lean) {
        # Push folder by folder, skipping the other languages' fonts.
        & $adb shell mkdir -p "$dst/extract/romfs/Content/Win/Fonts"
        foreach ($d in Get-ChildItem "$root\private\extract\romfs" -Directory) {
            if ($d.Name -ne "Content") { & $adb push $d.FullName "$dst/extract/romfs/"; continue }
            foreach ($c in Get-ChildItem $d.FullName) {
                if ($c.Name -ne "Win") { & $adb push $c.FullName "$dst/extract/romfs/Content/"; continue }
                foreach ($w in Get-ChildItem $c.FullName) {
                    if ($w.Name -ne "Fonts") { & $adb push $w.FullName "$dst/extract/romfs/Content/Win/"; continue }
                    & $adb push "$($w.FullName)\en" "$dst/extract/romfs/Content/Win/Fonts/"
                }
            }
        }
    } else {
        & $adb push "$root\private\extract\romfs" "$dst/extract/"
    }
}
if ($Resolution) {
    Set-Content -Path "$env:TEMP\hades_config.txt" -Value "resolution=$Resolution" -Encoding ascii
    & $adb push "$env:TEMP\hades_config.txt" "$dst/config.txt"
}
& $adb shell am start -n "$pkg/android.app.NativeActivity"
Write-Host "Started. Log: adb logcat -s hades"
