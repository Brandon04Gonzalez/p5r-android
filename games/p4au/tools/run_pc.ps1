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
    [int]$ProfileAt = 0,                 # after this many seconds, sample host pcs for 10 s (profile.txt in the run)
    [string]$Exe = "private\p4au\build\p4au.exe"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\..\.."
Set-Location $root
if (Get-Process p4au -ErrorAction SilentlyContinue) { Write-Error "a p4au process is already running"; exit 2 }
$run = "private\p4au\runs\$Name"
$frames = "private\p4au\frames"          # where the runtime writes them (<data root>/frames)
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
New-Item -ItemType Directory -Force "$run\frames" | Out-Null
if (Test-Path $frames) { Remove-Item -Recurse -Force $frames }

$vars = @{ NX_LOG = "$run\nx.log"; NX_MEM_LIMIT_GB = "10"; NX_GPU = "discrete"; NX_FPS_CAP = "60" }
if (-not $Docked) { $vars.NX_HANDHELD = "1" }
if ($Headless) { $vars.NX_HEADLESS = "1"; $vars.NX_MUTE = "1" }
if ($DumpEvery) { $vars.NX_DUMP_FRAMES = "$DumpEvery" }
foreach ($kv in ($Env -split ";")) { if ($kv -match "^([^=]+)=(.*)$") { $vars[$Matches[1]] = $Matches[2] } }
if ($Keys) { $vars.NX_INPUT_SCRIPT = $Keys }
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
