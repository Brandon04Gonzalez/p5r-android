# Captures the game window again and again (PrintWindow, as tools\capture.ps1), for glitches that only show in a
# running window (docs/nvn/DEBUG-2.md #77). Usage: tools\capture_burst.ps1 -Out <dir> [-Count 200] [-DelayMs 120]
# [-WaitSeconds 0]. Waits for a wonder.exe window first.
param([Parameter(Mandatory)][string]$Out, [int]$Count = 200, [int]$DelayMs = 120, [int]$WaitSeconds = 0)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Burst {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
    public struct R { public int L, T, Rt, B; }
}
"@
New-Item -ItemType Directory -Force $Out | Out-Null
$h = [IntPtr]::Zero
for ($k = 0; $k -lt 120 -and $h -eq [IntPtr]::Zero; $k++) {
    $p = Get-Process wonder -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { $h = [IntPtr]$p.MainWindowHandle } else { Start-Sleep 1 }
}
if ($h -eq [IntPtr]::Zero) { Write-Error "no wonder window"; exit 1 }
if ($WaitSeconds) { Start-Sleep $WaitSeconds }
for ($k = 1; $k -le $Count; $k++) {
    $r = New-Object Burst+R
    [Burst]::GetClientRect($h, [ref]$r) | Out-Null
    if ($r.Rt -le 0) { break }
    $bmp = New-Object System.Drawing.Bitmap $r.Rt, $r.B
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [Burst]::PrintWindow($h, $dc, 3) | Out-Null  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $bmp.Save((Join-Path $Out ("b{0:D4}.png" -f $k)))
    $g.Dispose(); $bmp.Dispose()
    Start-Sleep -Milliseconds $DelayMs
}
"$((Get-ChildItem $Out -Filter *.png).Count) captures in $Out"
