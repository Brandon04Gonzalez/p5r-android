# Capture one JJK NCE run on an attached Android device. Pulls runtime.log and crash.log from the canonical data folder.
# Usage: games\jjk\tools\run_android.ps1 -Name boot1 [-Seconds 60] [-Env "NX_X=1;NX_Y=2"] [-Serial <adb serial>] [-Keep]
param(
    [Parameter(Mandatory)][string]$Name,
    [int]$Seconds = 60,
    [string]$Env = "",
    [string]$Serial = "",
    [switch]$Keep
)
$ErrorActionPreference = "Continue"
$root = Resolve-Path "$PSScriptRoot\..\..\.."
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif (Test-Path "C:\Android\sdk") { "C:\Android\sdk" } else { "$env:LOCALAPPDATA\Android\Sdk" }
$adb = "$sdk\platform-tools\adb.exe"
$dev = if ($Serial) { @("-s", $Serial) } else { @() }
$pkg = "io.github.jjkport"
$data = "/storage/emulated/0/Jujutsu Kaisen Cursed Clash"
$run = "$root\private\jjk\runs\$Name"
New-Item -ItemType Directory -Force $run | Out-Null

# Runtime config uses the first value for each key, so explicit -Env settings take precedence.
$lines = @($Env -split ";" | Where-Object { $_ -match "=" }) + @("NX_FPS_CAP=30", "NX_HANDHELD=1")
$cfg = "$run\config.txt"
[System.IO.File]::WriteAllText($cfg, ($lines -join "`n") + "`n")
& $adb @dev shell am force-stop $pkg 2>$null
& $adb @dev push $cfg "$data/config.txt" 2>$null | Out-Null
& $adb @dev logcat -c 2>$null
& $adb @dev shell am start -n "$pkg/android.app.NativeActivity" 2>$null | Out-Null
Start-Sleep -Seconds $Seconds
$alive = & $adb @dev shell pidof $pkg 2>$null
& $adb @dev pull "$data/runtime.log" "$run\runtime.log" 2>$null | Out-Null
& $adb @dev pull "$data/crash.log" "$run\crash.log" 2>$null | Out-Null
& $adb @dev logcat -d 2>$null | Select-String -Pattern "jjk|libc|CRASH|backtrace|FATAL EXCEPTION|kgsl|Adreno" | Out-File -Encoding utf8 "$run\logcat.txt"
if (-not $Keep) { & $adb @dev shell am force-stop $pkg 2>$null }
$n = if (Test-Path "$run\runtime.log") { (Get-Content "$run\runtime.log" | Measure-Object -Line).Lines } else { 0 }
Write-Host "$Name : process $(if ($alive) { 'alive' } else { 'gone' }), runtime.log $n lines"
