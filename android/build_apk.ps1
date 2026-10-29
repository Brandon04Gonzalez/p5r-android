# Build a game's Android APK (NativeActivity, no Gradle): cmake (NDK) -> lib<game>.so -> aapt2 -> zipalign -> apksigner.
# Usage, in a dev shell (. .\dev.ps1):  android\build_apk.ps1 [-Game <game>] [-Install]   (a folder under games\, default hades)
#   -Nce          native execution (CMake NX_NCE): the APK holds no game code, it runs the exefs of the player's dump
#                 (build in private\<game>\ba_nce, APK in private\<game>\apk_nce)
#   -TestPackage  installs next to the normal build: package <package>.nce, label "<label> (NCE)"
#   -Release      the public build (CMake NX_RELEASE: no nx.log file, no file-triggered diagnostics; logcat only);
#                 without it the build directory keeps the setting it has
param([string]$Game = "p5r", [switch]$Install, [switch]$Nce, [switch]$TestPackage, [switch]$Release)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
# The SDK: ANDROID_HOME, else C:\Android\sdk, else Android Studio's default (%LOCALAPPDATA%\Android\Sdk).
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif (Test-Path "C:\Android\sdk") { "C:\Android\sdk" } else { "$env:LOCALAPPDATA\Android\Sdk" }
# apksigner and d8 need a JDK: JAVA_HOME, else Android Studio's own.
if (-not $env:JAVA_HOME -and (Test-Path "C:\Program Files\Android\Android Studio\jbr")) { $env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr" }
if ($env:JAVA_HOME) { $env:PATH = "$env:JAVA_HOME\bin;$env:PATH" }
$ndk = "$sdk\ndk\28.2.13676358"
$bt = "$sdk\build-tools\35.0.0"
$jar = "$sdk\platforms\android-35\android.jar"
# Hades keeps the layout from before there were other games (private\apk, private\ba).
$priv = if ($Game -eq "hades") { "$root\private" } else { "$root\private\$Game" }
$out = if ($Nce) { "$priv\apk_nce" } else { "$priv\apk" }
$build = if ($Nce) { "$priv\ba_nce" } else { "$priv\ba" }
$nceFlag = if ($Nce) { "ON" } else { "OFF" }

if (-not (Test-Path "$build\build.ninja")) {
    cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static "-DNX_GAME=$Game" "-DNX_NCE=$nceFlag"
}
if ($Release) { cmake -B $build -DNX_RELEASE=ON | Out-Null }
# Libraries packed with lib<game>.so: the P5R native build prepares the game on the phone (libp5rimport.so) with the
# shader translator (libspirvharness.so, built in Docker by games/p5r/tools/build_harness_android.sh).
$extraLibs = @()
$targets = @($Game)
if ($Nce -and $Game -eq "p5r") {
    $harness = "$root\private\build\spirv_harness_android\libspirvharness.so"
    if (-not (Test-Path $harness)) {
        throw "missing $harness. Build it once, with Docker Desktop running, from the repository root: docker run --rm -v ""${root}:/src"" -w /src mcr.microsoft.com/dotnet/sdk:10.0 sh games/p5r/tools/build_harness_android.sh"
    }
    $targets += "p5rimport"
    $extraLibs += @("$build\libp5rimport.so", $harness)
}
if ($Nce -and $Game -eq "jjk") {
    $targets += "jjkimport"
    $extraLibs += "$build\libjjkimport.so"
}
cmake --build $build --target @targets
if ($LASTEXITCODE) { throw "native build failed" }

Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\lib\arm64-v8a", "$out\res" | Out-Null
& "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-unneeded -o "$out\lib\arm64-v8a\lib$Game.so" "$build\lib$Game.so"
foreach ($lib in $extraLibs) {
    & "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-unneeded -o "$out\lib\arm64-v8a\$(Split-Path $lib -Leaf)" $lib
}

& "$bt\aapt2.exe" compile --dir "$PSScriptRoot\res" -o "$out\res.zip"
# The launcher icon: the project's own (games\<game>\icon\res, e.g. games\p5r\tools\make_icon.py), else the game's,
# when the player made it from their dump (private\<game>\icon\res, games\p5r\tools\nsp_icon.py): compiled with the
# rest, and the manifest copy names it.
$manifest = "$root\games\$Game\AndroidManifest.xml"
$resZips = @("$out\res.zip")
$iconRes = if (Test-Path "$root\games\$Game\icon\res") { "$root\games\$Game\icon\res" } else { "$priv\icon\res" }
if (Test-Path $iconRes) {
    & "$bt\aapt2.exe" compile --dir $iconRes -o "$out\icon.zip"
    $resZips += "$out\icon.zip"
    $text = [System.IO.File]::ReadAllText($manifest) -replace '<application', '<application android:icon="@mipmap/ic_launcher"'
    $manifest = "$out\AndroidManifest.xml"
    [System.IO.File]::WriteAllText($manifest, $text)
}
$linkArgs = @()
if ($TestPackage) {
    $text = [System.IO.File]::ReadAllText($manifest) -replace 'android:label="([^"]*)"', 'android:label="$1 (NCE)"'
    $manifest = "$out\AndroidManifest.test.xml"
    [System.IO.File]::WriteAllText($manifest, $text)
    $package = ([regex]::Match($text, 'package="([^"]+)"')).Groups[1].Value
    $linkArgs += @("--rename-manifest-package", "$package.nce")
}
& "$bt\aapt2.exe" link -o "$out\base.apk" -I $jar --manifest $manifest @linkArgs @resZips
if ($LASTEXITCODE) { throw "aapt2 link failed" }

# Add the native library (stored uncompressed would need page alignment; compressed + extractNativeLibs is fine).
Push-Location $out
Compress-Archive -Path "lib" -DestinationPath "$out\lib.zip" -Force
Pop-Location
Add-Type -AssemblyName System.IO.Compression.FileSystem
$apk = [System.IO.Compression.ZipFile]::Open("$out\base.apk", "Update")
foreach ($so in Get-ChildItem "$out\lib\arm64-v8a" -Filter *.so) {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($apk, $so.FullName, "lib/arm64-v8a/$($so.Name)", "Optimal") | Out-Null
}
# Java of the game's own (games\<game>\java, e.g. Wonder's home screen): javac against android.jar, d8 to classes.dex.
$java = "$root\games\$Game\java"
if (Test-Path $java) {
    $jdk = (Get-ChildItem "C:\Program Files\Microsoft\jdk-*", "C:\Program Files\Java\jdk-*" -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -Last 1).FullName
    # else Android Studio's own runtime, which is a full JDK (javac included)
    if (-not $jdk -and $env:JAVA_HOME -and (Test-Path (Join-Path $env:JAVA_HOME "bin\javac.exe"))) { $jdk = $env:JAVA_HOME }
    if (-not $jdk) { throw "a JDK is needed for games\$Game\java (winget install Microsoft.OpenJDK.17)" }
    New-Item -ItemType Directory -Force "$out\classes", "$out\dex" | Out-Null
    $sources = @((Get-ChildItem $java -Recurse -Filter *.java).FullName)
    & "$jdk\bin\javac.exe" -encoding UTF-8 -source 1.8 -target 1.8 -Xlint:-options -bootclasspath $jar -d "$out\classes" @sources
    if ($LASTEXITCODE) { throw "javac failed" }
    $classes = @((Get-ChildItem "$out\classes" -Recurse -Filter *.class).FullName)
    $env:JAVA_HOME = $jdk
    & "$bt\d8.bat" --release --min-api 30 --lib $jar --output "$out\dex" @classes
    if ($LASTEXITCODE) { throw "d8 failed" }
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($apk, "$out\dex\classes.dex", "classes.dex", "Optimal") | Out-Null
}
$apk.Dispose()

& "$bt\zipalign.exe" -f -p 4 "$out\base.apk" "$out\aligned.apk"
& "$bt\apksigner.bat" sign --ks "$env:USERPROFILE\.android\debug.keystore" --ks-pass pass:android --key-pass pass:android `
    --out "$out\$Game.apk" "$out\aligned.apk"
if ($LASTEXITCODE) { throw "apksigner failed" }
$size = [math]::Round((Get-Item "$out\$Game.apk").Length / 1MB, 1)
Write-Host "APK: $out\$Game.apk ($size MB)"

if ($Install) { & "$sdk\platform-tools\adb.exe" install -r "$out\$Game.apk" }
