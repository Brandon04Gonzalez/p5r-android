#!/usr/bin/env pwsh
<#
Inspect a locally owned Jujutsu Kaisen: Cursed Clash Switch dump using the repository's shared importer.
This script never uploads game data or keys. It writes extracted files only to the output directory supplied by the user.

Example:
  .\tools\inspect_jjk_dump.ps1 -NxImport .\private\tools\nximport.exe -ProdKeys "$HOME\prod.keys" -GameFile "$HOME\JJK.xci" -OutDir "$HOME\jjk-inspect"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$NxImport,
    [Parameter(Mandatory = $true)][string]$ProdKeys,
    [Parameter(Mandatory = $true)][string]$GameFile,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$TitleKeys = ""
)

$ErrorActionPreference = "Stop"
$targetTitle = "01002FC012548000"

foreach ($path in @($NxImport, $ProdKeys, $GameFile)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

$nx = (Resolve-Path -LiteralPath $NxImport).Path
$keys = (Resolve-Path -LiteralPath $ProdKeys).Path
$game = (Resolve-Path -LiteralPath $GameFile).Path
$out = [System.IO.Path]::GetFullPath($OutDir)
if ($TitleKeys) {
    if (-not (Test-Path -LiteralPath $TitleKeys -PathType Leaf)) {
        throw "Title keys file not found: $TitleKeys"
    }
    $title = (Resolve-Path -LiteralPath $TitleKeys).Path
} else {
    $title = ""
}

New-Item -ItemType Directory -Force -Path $out | Out-Null
Write-Host "Target: Jujutsu Kaisen: Cursed Clash ($targetTitle)"
Write-Host "Phase 1/2: Inspecting the selected base game with the shared importer..."
$args = @("--out", $out, "--title", $targetTitle, "--exefs")
$args += $keys
if ($title) { $args += $title }
$args += $game

& $nx @args
if ($LASTEXITCODE -ne 0) {
    throw "Importer rejected the selected dump (exit $LASTEXITCODE). Check the importer log above; no compatibility claim can be made."
}

Write-Host "Phase 2/2: Locating the extracted ExeFS main executable..."
$main = Get-ChildItem -LiteralPath $out -Recurse -File |
    Where-Object { $_.Name -eq "main" } |
    Select-Object -First 1
if (-not $main) {
    throw "Import succeeded but no ExeFS 'main' file was found under $out. Inspect the output layout before proceeding."
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command python3 -ErrorAction SilentlyContinue }
if (-not $python) { throw "Python 3 is required to read the NSO Build ID." }

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildIdTool = Join-Path $repoRoot "tools/nso_build_id.py"
if (-not (Test-Path -LiteralPath $buildIdTool -PathType Leaf)) {
    throw "Build ID helper not found: $buildIdTool"
}
$buildIdFull = (& $python.Source $buildIdTool $main.FullName).Trim()
if ($LASTEXITCODE -ne 0 -or $buildIdFull -notmatch '^[0-9A-Fa-f]{64}$') {
    throw "Could not read a valid 32-byte NSO Build ID from $($main.FullName)."
}
$buildIdRuntime = $buildIdFull.Substring(0, 16).ToUpperInvariant()

$report = @"
Jujutsu Kaisen: Cursed Clash - local dump inspection
Target Title ID: $targetTitle
Detected Build ID field (32 bytes): $($buildIdFull.ToUpperInvariant())
Runtime main_build_id candidate (first 8 bytes): $buildIdRuntime
ExeFS main path: $($main.FullName)
Status: metadata inspection only; this does not establish runtime or renderer compatibility.
"@
$reportPath = Join-Path $out "jjk-inspection-report.txt"
Set-Content -LiteralPath $reportPath -Value $report -Encoding utf8

Write-Host ""
Write-Host $report
Write-Host "Report saved to: $reportPath"
