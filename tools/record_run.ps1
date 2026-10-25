# A windowed run recorded with ffmpeg, then cut into frames for the glitch census (docs/nvn/pc-quality/PLAN.md).
#
#   tools\record_run.ps1 -Name rec-course11 -Course11 -Seconds 330
#   tools\record_run.ps1 -Name rec-map -Save private\wonder\save-backup-mapa -Seconds 150 -Script "20:L:0.4;..."
#
# The run itself is tools\wonder_course.ps1 -Windowed (-Course11, or no -Script) or tools\wonder_run.ps1 with
# -Script (own input timeline). ffmpeg grabs the game window (gfxcapture, i.e. Windows.Graphics.Capture, so other
# windows on top do not matter; -Capture desktop: gdigrab of its screen area; gdigrab's title= is white for a
# Vulkan window), -Framerate fps, 960x540, x264 ultrafast, from
# the moment it appears and is stopped with 'q' just before the run ends. Then: a frame every 0.5 s, the
# near-black detector (tools\find_glitches.py, -Threshold per mille), the flat-lavender one (tools\find_lavender.py)
# and one contact sheet per -SheetEvery frames.
# -DumpAt "300,420" writes private\wonder\dump_frames ("40 1": the next 40 frames dumped, DEBUG-2 #77) that many
# seconds after the game started; the dumps land in the run folder's frames\ (as PNG).
#
# Output: <Out>\<Name>\: video.mp4, frames\NNNNN.png (frame n is at (n-1)/2 s of video), sheet-NN.png,
# glitches.txt, lavender.txt, meta.txt; the run's nx.log and summary.txt stay in private\wonder\runs\<Name>.
param(
    [Parameter(Mandatory)][string]$Name,
    [int]$Seconds = 330,
    [switch]$Course11,
    [string]$Script = "",                # an own NX_INPUT_SCRIPT (runs tools\wonder_run.ps1 directly)
    [string]$Save = "",                  # a save folder copied into private\wonder\user\save first (implies -KeepSave)
    [switch]$KeepSave,
    [string]$Env = "NX_MUTE=1",
    [string]$Exe = "private\wonder\build\wonder.exe",
    [string]$DumpAt = "",                # seconds after the game started, comma separated
    [int]$Framerate = 30,
    [string]$Capture = "window",         # window: the game window (gfxcapture); desktop: its area of the screen
    [int]$SheetEvery = 60,
    [double]$Threshold = 20,
    [string]$Out = "private\wonder\pc-quality\rec",
    [switch]$NoAnalysis
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if (Get-Process wonder -ErrorAction SilentlyContinue) { Write-Error "a wonder process is already running"; exit 2 }
$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) {
    $ffmpeg = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.1-full_build\bin\ffmpeg.exe"
}
$dir = Join-Path $root "$Out\$Name"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force $dir | Out-Null

if ($Save) {
    if (Test-Path "private\wonder\user\save") { Remove-Item -Recurse -Force "private\wonder\user\save" }
    New-Item -ItemType Directory -Force "private\wonder\user" | Out-Null
    Copy-Item -Recurse $Save "private\wonder\user\save"
    $KeepSave = $true
}

# The run, in its own PowerShell (it blocks until -Seconds).
if ($Script) {
    $file = "$PSScriptRoot\wonder_run.ps1"
    $args1 = @{ Name = $Name; Seconds = $Seconds; Script = $Script; Env = $Env; Exe = $Exe; MaxMemGB = 6 }
} else {
    $file = "$PSScriptRoot\wonder_course.ps1"
    # No periodic frame dumps: each one stalls a present by ~1 s (the 60-frame window after it drops to ~30 fps).
    $args1 = @{ Name = $Name; Seconds = $Seconds; Env = $Env; Exe = $Exe; Windowed = $true; DumpEvery = 0 }
    if ($Course11) { $args1.Course11 = $true }
}
if ($KeepSave) { $args1.KeepSave = $true }
$job = Start-Job -ScriptBlock { param($r, $f, $a) Set-Location $r; & $f @a 6>&1 } -ArgumentList $root, $file, $args1

# The window: wait for its title, then grab it.
$game = $null
for ($k = 0; $k -lt 120 -and -not $game; $k++) {
    Start-Sleep -Milliseconds 500
    $game = Get-Process wonder -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -match 'F11' } | Select-Object -First 1
    if ($job.State -ne 'Running') { break }
}
if (-not $game) { Receive-Job $job -Wait; Write-Error "no game window"; exit 1 }
$title = $game.MainWindowTitle
$started = $game.StartTime
# For -Capture desktop: the window's client area on the screen, with the game window brought to the front.
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class RecWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref PT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
Start-Sleep -Seconds 1
[RecWin]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
$rc = New-Object RecWin+RECT; [RecWin]::GetClientRect($game.MainWindowHandle, [ref]$rc) | Out-Null
$pt = New-Object RecWin+PT; [RecWin]::ClientToScreen($game.MainWindowHandle, [ref]$pt) | Out-Null
$w = $rc.R - $rc.R % 2; $h = $rc.B - $rc.B % 2
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ffmpeg
$enc = "-c:v libx264 -preset ultrafast -crf 30 -pix_fmt yuv420p `"$dir\video.mp4`""
if ($Capture -eq "desktop") {
    $psi.Arguments = "-hide_banner -loglevel error -nostats -y -f gdigrab -framerate $Framerate -draw_mouse 0 " +
        "-offset_x $($pt.X) -offset_y $($pt.Y) -video_size ${w}x${h} -i desktop -vf scale=960:540 $enc"
} else {
    # Windows.Graphics.Capture of the game window itself: other windows on top do not get into the video.
    $psi.Arguments = "-hide_banner -loglevel error -nostats -y -f lavfi " +
        "-i `"gfxcapture=hwnd=$($game.MainWindowHandle.ToInt64()):max_framerate=$($Framerate):capture_cursor=0`" " +
        "-vf `"hwdownload,format=bgra,fps=$Framerate,scale=960:540`" $enc"
}
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.CreateNoWindow = $true
$ff = [System.Diagnostics.Process]::Start($psi)
$recFrom = ((Get-Date) - $started).TotalSeconds
Write-Host ("recording '{0}' from {1:N1} s after the game started" -f $title, $recFrom)

# Until just before the run's end: on-demand frame dumps, then 'q' so the mp4 is closed properly.
$dumps = @($DumpAt -split ',' | Where-Object { $_.Trim() } | ForEach-Object { [double]$_ })
$done = @{}
while ($job.State -eq 'Running') {
    $t = ((Get-Date) - $started).TotalSeconds
    foreach ($d in $dumps) {
        if (-not $done[$d] -and $t -ge $d) {
            Set-Content -Encoding ascii "private\wonder\dump_frames" "40 1"
            $done[$d] = $true
            Write-Host ("dump_frames at {0:N0} s (video {1:N0} s)" -f $t, ($t - $recFrom))
        }
    }
    if ($t -ge $Seconds - 3 -and -not $ff.HasExited) {
        try { $ff.StandardInput.Write("q"); $ff.StandardInput.Flush() } catch { }
    }
    Start-Sleep -Milliseconds 250
}
if (-not $ff.WaitForExit(20000)) { $ff.Kill() }
$runOut = Receive-Job $job -Wait
Remove-Job $job
$runOut | ForEach-Object { Write-Host $_ }
@("title      $title", "exe        $Exe", ("video from {0:N1} s after the game process started" -f $recFrom),
  "run        private\wonder\runs\$Name", "dump_at    $DumpAt") | Set-Content -Encoding utf8 "$dir\meta.txt"
if ($NoAnalysis -or -not (Test-Path "$dir\video.mp4")) { exit 0 }

# A frame every 0.5 s, the detector, contact sheets.
New-Item -ItemType Directory -Force "$dir\frames" | Out-Null
& $ffmpeg -hide_banner -loglevel error -i "$dir\video.mp4" -vf fps=2 "$dir\frames\%05d.png"
$py = "private\venv\Scripts\python"
$g = & $py tools\find_glitches.py "$dir\frames" $Threshold
$g | Set-Content -Encoding utf8 "$dir\glitches.txt"
$flagged = @($g | Select-String '^(\d+)\.png: .*<--' | ForEach-Object { "{0:N1}" -f (([int]$_.Matches[0].Groups[1].Value - 1) / 2) })
Write-Host ("frames: {0}; near-black flagged at video s: {1}" -f (Get-ChildItem "$dir\frames").Count, ($flagged -join ' '))
$lv = & $py tools\find_lavender.py "$dir\frames"
$lv | Set-Content -Encoding utf8 "$dir\lavender.txt"
$lf = @($lv | Select-String '\(\s*([\d.]+) s\).*<--' | ForEach-Object { $_.Matches[0].Groups[1].Value })
Write-Host ("lavender flagged at video s: {0}" -f ($lf -join ' '))
$all = @(Get-ChildItem "$dir\frames" -Filter *.png | Sort-Object Name)
for ($k = 0; $k * $SheetEvery -lt $all.Count; $k++) {
    $tmp = "$dir\sheet-tmp"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $all | Select-Object -Skip ($k * $SheetEvery) -First $SheetEvery | ForEach-Object { Copy-Item $_.FullName $tmp }
    & $py tools\frames.py sheet $tmp ("$dir\sheet-{0:D2}.png" -f $k) | Out-Null
    Remove-Item -Recurse -Force $tmp
}
Write-Host "sheets: $dir\sheet-*.png ($SheetEvery frames, $($SheetEvery / 2) s each)"
