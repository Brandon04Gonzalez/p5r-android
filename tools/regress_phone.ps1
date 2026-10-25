# Phone half of the regression suite (docs/REGRESSION.md): the scripted course 1-1 run on an Android phone over adb,
# from Windows (tools/wonder_phone_course.sh is the Mac's, with profiling and memory watches; this is the measured
# subset). Results go to private\wonder\regress\<label>\phone.json, compared by tools\regress.ps1 -Compare.
#
#   tools\regress_phone.ps1 -Label base   [-Apk <file.apk>]   (-Apk: installs it first; the data stays on the phone)
#
# Waits until the battery is below -CoolC (default 34 C) before each run, so both builds start equally cool.
param(
    [Parameter(Mandatory)][string]$Label,
    [string]$Serial = "",
    [string]$Apk = "",
    [int]$Repeat = 2,
    [int]$Seconds = 300,
    [double]$CoolC = 34.0,
    [string[]]$Extra = @()
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$adb = "C:\Android\sdk\platform-tools\adb.exe"
if (-not $Serial) { $Serial = ((& $adb devices) | Select-String '^(\S+)\s+device$' | Select-Object -First 1).Matches[0].Groups[1].Value }
$pkg = "io.github.belmantegu.wonderrecomp"
$data = "/storage/emulated/0/Super Mario Bros. Wonder"
$dir = "private\wonder\regress\$Label"
New-Item -ItemType Directory -Force $dir | Out-Null
function A { & $adb -s $Serial @args }

if ($Apk) { A install -r $Apk | Select-Object -Last 1 }

# The blind course script of tools/wonder_phone_course.sh (handheld, the phone's menu timing).
$sc = "25:A:0.5;40:A:1.0;60:A:1.0;95:RIGHT:0.3;105:A:1.0;118:A:1.0;150:A:3.0;160:A:0.1;160.5:A:0.1;161:A:0.1;" +
      "161.5:A:0.1;162:A:0.1;170:A:0.05;175:A:0.3;176:A:0.3;177:A:0.3;"
for ($t = 185; $t -le 380; $t += 3) { $sc += "${t}:A:0.3;" }
$sc += "185:RIGHT:195;"

function BatteryC { [double]((A shell dumpsys battery | Select-String 'temperature: (\d+)').Matches[0].Groups[1].Value) / 10 }
function Percentile($values, $p) { $s = @($values | Sort-Object); if (-not $s.Count) { return 0 }; $s[[int][math]::Floor(($s.Count - 1) * $p)] }

# The owner's save on the phone is kept aside and put back at the end (the runs start a new game).
$saveBackup = "private\wonder\regress\phone-save-backup"
if (-not (Test-Path $saveBackup)) {
    New-Item -ItemType Directory -Force $saveBackup | Out-Null
    A pull "$data/user/save/." "$saveBackup\" | Out-Null
}

$runs = @()
for ($k = 1; $k -le $Repeat; $k++) {
    $run = "$dir\phone-$k"
    if (Test-Path $run) { Remove-Item -Recurse -Force $run }
    New-Item -ItemType Directory -Force "$run\frames" | Out-Null
    A shell am force-stop $pkg
    for ($w = 0; $w -lt 40 -and (BatteryC) -gt $CoolC; $w++) { Start-Sleep 15 }
    $startC = BatteryC
    $cfg = @("# regression run ($Label $k)", "NX_HANDHELD=1", "NX_MEM_LIMIT_GB=5", "NX_DUMP_FRAMES=600",
             "NX_SHOW_WHEN_LOCKED=1", "NX_INPUT_SCRIPT=$sc") + $Extra
    $cfg | Set-Content -Encoding ascii "$run\config.txt"
    A push "$run\config.txt" "$data/config.txt" | Out-Null
    A shell rm -rf "'$data/frames'" "'$data/user/save'" | Out-Null  # a new game: the script expects the first menus
    A shell mkdir -p "'$data/frames'" | Out-Null
    A shell input keyevent KEYCODE_WAKEUP
    A shell am start -n "$pkg/android.app.NativeActivity" | Out-Null
    Start-Sleep $Seconds
    $alive = [bool](A shell pidof $pkg)
    A pull "$data/nx.log" "$run\nx.log" | Out-Null
    A pull "$data/frames/." "$run\frames\" | Out-Null
    A shell am force-stop $pkg
    if (Get-ChildItem "$run\frames" -Filter *.ppm -ErrorAction SilentlyContinue) {
        & private\venv\Scripts\python tools\frames.py png "$run\frames" "$run\frames" | Out-Null
        Get-ChildItem "$run\frames" -Filter *.ppm | Remove-Item
    }
    $log = if (Test-Path "$run\nx.log") { Get-Content "$run\nx.log" } else { @() }
    $fps = @($log | Select-String 'per frame over 60: ([\d.]+) fps' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } |
        Where-Object { $_ -gt 0 } | Select-Object -Skip 20)
    $replay = @($log | Select-String 'replay ([\d.]+) ms' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Select-Object -Skip 20)
    $mem = @($log | Select-String 'memory: ([\d.]+) GB' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
    $g = & private\venv\Scripts\python tools\find_glitches.py "$run\frames" 20 | Select-String '(\d+) frames flagged'
    $runs += [ordered]@{
        fps_median = Percentile $fps 0.5; fps_p10 = Percentile $fps 0.1; replay_ms_median = Percentile $replay 0.5
        mem_peak_gb = if ($mem.Count) { ($mem | Measure-Object -Maximum).Maximum } else { 0 }
        crashes = @($log | Select-String 'CRASH|GPU hang|over the .* limit').Count + [int](-not $alive)
        vk_errors = @($log | Select-String 'nvn/vk: .* failed \(').Count
        glitch_frames = if ($g) { [int]$g.Matches[0].Groups[1].Value } else { 0 }
        windows = $fps.Count; battery_c_start = $startC; battery_c_end = BatteryC; alive_at_end = $alive
    }
    "phone run ${k}: fps median $($runs[-1].fps_median), p10 $($runs[-1].fps_p10), alive $alive, $startC C -> $($runs[-1].battery_c_end) C"
}
if (Get-ChildItem $saveBackup -File -ErrorAction SilentlyContinue) {  # the owner's save back
    A shell rm -rf "'$data/user/save'" | Out-Null
    A shell mkdir -p "'$data/user/save'" | Out-Null
    foreach ($f in Get-ChildItem $saveBackup -File) { A push $f.FullName "$data/user/save/$($f.Name)" | Out-Null }
}
$res = [ordered]@{
    label = $Label; serial = $Serial
    fps_median = Percentile ($runs | ForEach-Object { $_.fps_median }) 0.5
    fps_p10 = ($runs | ForEach-Object { $_.fps_p10 } | Measure-Object -Minimum).Minimum
    replay_ms_median = Percentile ($runs | ForEach-Object { $_.replay_ms_median }) 0.5
    mem_peak_gb = ($runs | ForEach-Object { $_.mem_peak_gb } | Measure-Object -Maximum).Maximum
    crashes = ($runs | ForEach-Object { $_.crashes } | Measure-Object -Sum).Sum
    vk_errors = ($runs | ForEach-Object { $_.vk_errors } | Measure-Object -Sum).Sum
    glitch_frames = ($runs | ForEach-Object { $_.glitch_frames } | Measure-Object -Sum).Sum
    runs = $runs
}
$res | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "$dir\phone.json"
"phone $Label done: $dir\phone.json"
