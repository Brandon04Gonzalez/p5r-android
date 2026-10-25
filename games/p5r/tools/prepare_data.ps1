# Prepares the data the Android port needs from YOUR OWN dump of Persona 5 Royal (Windows 10/11, x86-64):
#   exefs\          the game's own code (main, sdk, ...): the APK runs it natively on the phone's CPU
#   extract\romfs\  the game's files
#   spirv\          the game's shaders (Maxwell, from ALL_USEU.CPK and main) translated to SPIR-V for Vulkan
# Copy those three folders to the phone, into Internal storage\Persona 5 Royal (about 15 GB).
#
# Usage, from the repository root, in a PowerShell where the MSVC compiler works ("Developer PowerShell for VS 2022",
# or . .\dev.ps1):
#   games\p5r\tools\prepare_data.ps1 -Nsp "<base game>.nsp" -Keys "<prod.keys>" [-TitleKeys <title.keys>] [-Jobs 8]
# The base game only: title 01005CA01580E000, version 1.0.0 (build 708BCACA873EA32B). Not the update NSP: the port
# is made for 1.0.0, and the APK refuses another exefs/main.
#
# Needs: Visual Studio 2022 Build Tools ("Desktop development with C++"), CMake and Ninja on PATH, Python 3.11+, the
# .NET SDK 10 (dotnet), and curl and tar (part of Windows 10/11). Everything it builds or downloads goes to private\
# (never committed). The keys are read by the importer only, never copied or logged.
param(
    [string]$Nsp = "",
    [string]$Keys = "",
    [string]$TitleKeys = "",
    [string]$Out = "private\p5r\data",
    [int]$Jobs = 4,
    [switch]$SkipImport  # the import is done already (exefs, analysis and extract are in -Out): shaders only
)
$ErrorActionPreference = "Stop"
if (-not $SkipImport -and (-not $Nsp -or -not $Keys)) { throw "-Nsp and -Keys are needed (or -SkipImport)" }
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
Set-Location $root
if (-not $SkipImport) {
    $Nsp = (Resolve-Path $Nsp).Path
    $Keys = (Resolve-Path $Keys).Path
}
if ($TitleKeys) { $TitleKeys = (Resolve-Path $TitleKeys).Path }
if (-not ([System.IO.Path]::GetFullPath((Join-Path $root $Out))).StartsWith((Join-Path $root "private"))) {
    throw "-Out must be under private\ (game data is never committed)"
}
New-Item -ItemType Directory -Force $Out | Out-Null

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }

# 1. Python (the shader extractor needs nothing outside the standard library).
Step "Python"
if (-not (Test-Path "private\venv\Scripts\python.exe")) { python -m venv private\venv }
$py = "$root\private\venv\Scripts\python.exe"

# 2. The importer (android/wonder/importer, the same code as the phone's): NSP + keys -> exefs, extract\romfs.
Step "Import (exefs and RomFS)"
$imp = "private\build\importer\nximport.exe"
if ($SkipImport) {
    Write-Host "skipped (-SkipImport)"
} else {
    if (-not (Test-Path $imp)) {
        cmake -S games\p5r\tools\win_compat -B private\build\importer -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build private\build\importer
        if ($LASTEXITCODE) { throw "the importer did not build (is the MSVC compiler on PATH?)" }
    }
    $impArgs = @("--out", $Out, "--title", "01005CA01580E000", "--exefs", $Keys)
    if ($TitleKeys) { $impArgs += $TitleKeys }
    $impArgs += $Nsp
    & $imp @impArgs
    if ($LASTEXITCODE) { throw "import failed" }
}
$main = Join-Path $Out "exefs\main"
$id = [System.BitConverter]::ToString([System.IO.File]::ReadAllBytes($main)[0x40..0x47]).Replace("-", "")
if ($id -ne "708BCACA873EA32B") { throw "exefs\main is build $id, not 708BCACA873EA32B (1.0.0): use the base game's NSP, without the update" }

# 3. The shaders: every HNSH record of the GSC caches in ALL_USEU.CPK, and the blocks built into main.
Step "Shaders: extract"
& $py games\p5r\tools\gsc_shaders.py --cpk "$Out\extract\romfs\CPK\ALL_USEU.CPK" --main "$Out\analysis\main.bin" `
    --out private\p5r\shaders_extracted
if ($LASTEXITCODE) { throw "shader extraction failed" }

# 4. The translator: Ryujinx's shader translator (MIT, the Ryubing fork) with this repository's patches
#    (tools/maxwell/spirv_harness, docs/nvn/TRANSLATOR.md).
Step "Shaders: translator"
$T = "private\third_party"
if (-not (Test-Path "$T\ryubing\src\Ryujinx.Graphics.Shader")) {
    New-Item -ItemType Directory -Force "$T\ryubing" | Out-Null
    curl.exe -L -o "$T\ryubing.tar.gz" https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz
    tar -xzf "$T\ryubing.tar.gz" -C "$T\ryubing" --strip-components=1
    Remove-Item "$T\ryubing.tar.gz"
}
$harness = "private\build\spirv_harness\bin\Release\net10.0\spirv_harness.dll"
if (-not (Test-Path $harness)) {
    dotnet build tools\maxwell\spirv_harness\SpirvHarness.csproj -c Release -m:2 "-p:PatchPython=$py"
    if ($LASTEXITCODE) { throw "the translator did not build (is the .NET 10 SDK installed?)" }
}

# 5. Translate (~30,000 shaders: 10-30 minutes).
Step "Shaders: translate"
dotnet $harness --batch private\p5r\shaders_extracted --out "$Out\spirv" --jobs $Jobs --no-val
if ($LASTEXITCODE) { throw "translation failed" }

Step "Done"
Write-Host "Copy these folders of $Out to the phone, into Internal storage\Persona 5 Royal:"
Write-Host "  exefs\  extract\  spirv\"
Write-Host "(analysis\ is only for the PC build; the APK does not need it.)"
