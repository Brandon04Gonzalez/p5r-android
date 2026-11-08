# One P4AU run on the PC, for debugging the boot: games\p4au\tools\run_pc.ps1 -Name boot1 -Seconds 60 [-Headless]
# Writes private\p4au\runs\<Name>\nx.log (and frames\*.ppm with -DumpEvery). The process is killed after -Seconds.
param(
    [Parameter(Mandatory)][string]$Name,
    [int]$Seconds = 60,
    [switch]$Headless,
    [switch]$Docked,                     # 1080p; default is handheld 720p (the port's default)
    [int]$DumpEvery = 0,                 # NX_DUMP_FRAMES: every Nth presented frame to frames\NNNNN.ppm
    [string]$Env = "",                   # other switches: "NX_A=1;NX_B=2"
    [string]$Keys = "",                  # NX_INPUT_SCRIPT ("30:A:0.3;40:DOWN:0.3"): its ';' would split -Env
    [string]$Keys2 = "",                 # NX_INPUT_SCRIPT_P2: the same for player 2 (connects it)
    [string]$FrameKeys = "",             # NX_INPUT, by presented frame ("600:A,900:Right:4"; runtime/src/hle_hid.cpp)
    [string]$FrameKeys2 = "",            # NX_INPUT_P2: the same for player 2 (connects it)
    [int]$ProfileAt = 0,                 # after this many seconds, sample host pcs for 10 s (profile.txt in the run)
    [switch]$Deterministic,              # NX_DETERMINISTIC=1, the netplay mode (runtime/src/netplay/determinism.h)
    [int]$HashEvery = 0,                 # NX_DET_HASH_EVERY: log the state hash every N frames (10 with -Deterministic)
    [string]$CompareTo = "",             # afterwards, compare the state hashes with this run's, frame by frame
    [switch]$CompareOnly,                # only that comparison, of runs already made (-Name and -CompareTo)
    [string]$DataRoot = "",              # NX_DATA_ROOT (frames then go to <DataRoot>\frames)
    [string]$Exe = "private\p4au\build\p4au.exe"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\..\.."
Set-Location $root

# The "det: frame F state H (w0 w1 w2 w3)" lines of two runs, frame by frame. For P4AU w0..w3 is the round clock
# (games/p4au/game.cpp), so F - w2 is the frame the round's clock started on, and the first line after it with w1
# (frames left) at 0 is where the clock ran out. Exit code 1 when they differ.
function Compare-States([string]$a, [string]$b) {
    $read = {
        param($n)
        $h = @{}; $start = $null; $end = $null
        foreach ($l in Get-Content "private\p4au\runs\$n\nx.log") {
            if ($l -match "^det: frame (\d+) state ([0-9a-f]+) \((\d+) (\d+) (\d+) (\d+)\)") {
                $h[[int]$Matches[1]] = $Matches[2]
                if ($null -eq $start -and [int]$Matches[5] -gt 0) { $start = [int]$Matches[1] - [int]$Matches[5] }
                if ($null -ne $start -and $null -eq $end -and [int]$Matches[4] -eq 0) { $end = [int]$Matches[1] }
            }
        }
        , @($h, $start, $end)
    }
    $ra = & $read $a; $rb = & $read $b
    $frames = @($ra[0].Keys | Where-Object { $rb[0].ContainsKey($_) } | Sort-Object)
    $bad = @($frames | Where-Object { $ra[0][$_] -ne $rb[0][$_] })
    Write-Host "state check $a vs ${b}: $($frames.Count) frames compared ($($frames[0])-$($frames[-1])), $($bad.Count) differ; round clock started at frame $($ra[1]) vs $($rb[1]), ran out at $($ra[2]) vs $($rb[2])"
    if ($frames.Count -eq 0) { return 1 }
    if ($bad.Count) { Write-Host "  first difference at frame $($bad[0]): $($ra[0][$bad[0]]) vs $($rb[0][$bad[0]])"; return 1 }
    if ($ra[1] -ne $rb[1] -or $ra[2] -ne $rb[2]) { return 1 }
    return 0
}
if ($CompareOnly) { exit (Compare-States $Name $CompareTo) }

if (Get-Process p4au -ErrorAction SilentlyContinue) { Write-Error "a p4au process is already running"; exit 2 }
$run = "private\p4au\runs\$Name"
$frames = if ($DataRoot) { "$DataRoot\frames" } else { "private\p4au\frames" }  # where the runtime writes them (<data root>/frames)
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
New-Item -ItemType Directory -Force "$run\frames" | Out-Null
if (Test-Path $frames) { Remove-Item -Recurse -Force $frames }

# Only this call's switches turn the netplay mode and its test knobs on: not one left in the environment by an earlier
# run made from the same PowerShell session (NX_DETERMINISTIC, NX_DET_*, NX_FS_DELAY_US, NX_NET_*, NX_DATA_ROOT).
Get-ChildItem env: | Where-Object { $_.Name -match "^NX_(DET|FS_DELAY|NET_|DATA_ROOT)" } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$vars = @{ NX_LOG = "$run\nx.log"; NX_MEM_LIMIT_GB = "10"; NX_GPU = "discrete"; NX_FPS_CAP = "60" }
if (-not $Docked) { $vars.NX_HANDHELD = "1" }
if ($Headless) { $vars.NX_HEADLESS = "1"; $vars.NX_MUTE = "1" }
if ($DumpEvery) { $vars.NX_DUMP_FRAMES = "$DumpEvery" }
foreach ($kv in ($Env -split ";")) { if ($kv -match "^([^=]+)=(.*)$") { $vars[$Matches[1]] = $Matches[2] } }
if ($Keys) { $vars.NX_INPUT_SCRIPT = $Keys }
if ($Keys2) { $vars.NX_INPUT_SCRIPT_P2 = $Keys2 }
if ($FrameKeys) { $vars.NX_INPUT = $FrameKeys }
if ($FrameKeys2) { $vars.NX_INPUT_P2 = $FrameKeys2 }
if ($Deterministic) { $vars.NX_DETERMINISTIC = "1"; if (-not $HashEvery) { $HashEvery = 10 } }
if ($HashEvery) { $vars.NX_DET_HASH_EVERY = "$HashEvery" }
if ($DataRoot) { $vars.NX_DATA_ROOT = $DataRoot }
foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardError "$run\stderr.txt" -RedirectStandardOutput "$run\stdout.txt"
$waited = 0
if ($ProfileAt -gt 0 -and $ProfileAt -lt $Seconds) {
    if (-not $p.WaitForExit($ProfileAt * 1000)) { Set-Content -Path "private\p4au\profile" -Value "10" }
    $waited = $ProfileAt
}
$exited = $p.WaitForExit(($Seconds - $waited) * 1000)
if (-not $exited) { Stop-Process -Id $p.Id -Force; Write-Host "killed after $Seconds s" } else { Write-Host "exited with $($p.ExitCode)" }
Start-Sleep -Milliseconds 500
if (Test-Path $frames) { Get-ChildItem $frames | Move-Item -Destination "$run\frames" }
if (Test-Path "private\p4au\profile.txt") { Move-Item "private\p4au\profile.txt" "$run\profile.txt" -Force }
$log = Get-Item "$run\nx.log" -ErrorAction SilentlyContinue
if ($log) { Write-Host "nx.log: $($log.Length) bytes, $((Get-Content $log.FullName | Measure-Object -Line).Lines) lines" }
Write-Host "frames: $((Get-ChildItem "$run\frames").Count)"
if ($CompareTo) { exit (Compare-States $Name $CompareTo) }
