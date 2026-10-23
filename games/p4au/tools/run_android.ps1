# One P4AU run on the Android device: writes config.txt (the port's defaults plus -Env switches), starts the game,
# waits -Seconds and copies nx.log to private\p4au\runs\<Name>\. Usage:
#   games\p4au\tools\run_android.ps1 -Name a1 -Seconds 60 [-Env "NX_X=1;NX_Y=2"] [-Serial emulator-5560] [-Keep]
# -Keep leaves the game running.
param(
    [Parameter(Mandatory)][string]$Name,
    [int]$Seconds = 60,
    [string]$Env = "",
    [string]$Serial = "",
    [switch]$Keep
)
$ErrorActionPreference = "Continue"
$root = Resolve-Path "$PSScriptRoot\..\..\.."
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$dev = if ($Serial) { @("-s", $Serial) } else { @() }
$pkg = "io.github.p4aurecomp"
$data = "/storage/emulated/0/Persona 4 Arena Ultimax"
$run = "$root\private\p4au\runs\$Name"
New-Item -ItemType Directory -Force $run | Out-Null

# -Env first: the runtime keeps the first value of a key (setenv without overwrite), so -Env wins over the defaults.
$lines = @($Env -split ";" | Where-Object { $_ -match "=" }) + @("NX_FPS_CAP=60", "NX_HANDHELD=1")
$cfg = "$run\config.txt"
[System.IO.File]::WriteAllText($cfg, ($lines -join "`n") + "`n")
& $adb @dev shell am force-stop $pkg 2>$null
& $adb @dev push $cfg "$data/config.txt" 2>$null | Out-Null
& $adb @dev logcat -c 2>$null
& $adb @dev shell am start -n "$pkg/android.app.NativeActivity" 2>$null | Out-Null
Start-Sleep -Seconds $Seconds
$alive = & $adb @dev shell pidof $pkg 2>$null
& $adb @dev pull "$data/nx.log" "$run\nx.log" 2>$null | Out-Null
& $adb @dev logcat -d 2>$null | Select-String -Pattern "Adreno|kgsl|DEBUG|F libc|$pkg|CRASH|backtrace" | Out-File -Encoding utf8 "$run\logcat.txt"
if (-not $Keep) { & $adb @dev shell am force-stop $pkg 2>$null }
$n = if (Test-Path "$run\nx.log") { (Get-Content "$run\nx.log" | Measure-Object -Line).Lines } else { 0 }
Write-Host "$Name : process $(if ($alive) { 'alive' } else { 'gone' }), nx.log $n lines"
