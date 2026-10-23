# Build a game's Android APK (NativeActivity, no Gradle): cmake (NDK) -> lib<game>.so -> aapt2 -> zipalign -> apksigner.
# Usage, in a dev shell (. .\dev.ps1):  android\build_apk.ps1 [-Game <game>] [-Install]   (a folder under games\, default hades)
param([string]$Game = "hades", [switch]$Install)
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
$out = "$priv\apk"
$build = "$priv\ba"

if (-not (Test-Path "$build\build.ninja")) {
    cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static "-DNX_GAME=$Game"
}
cmake --build $build --target $Game
if ($LASTEXITCODE) { throw "native build failed" }

Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\lib\arm64-v8a", "$out\res" | Out-Null
& "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-unneeded -o "$out\lib\arm64-v8a\lib$Game.so" "$build\lib$Game.so"

& "$bt\aapt2.exe" compile --dir "$PSScriptRoot\res" -o "$out\res.zip"
# The game's own launcher icon, when the player made it from their dump (private\<game>\icon\res, e.g.
# games\p5r\tools\nsp_icon.py): compiled with the rest, and the manifest copy names it.
$manifest = "$root\games\$Game\AndroidManifest.xml"
$resZips = @("$out\res.zip")
if (Test-Path "$priv\icon\res") {
    & "$bt\aapt2.exe" compile --dir "$priv\icon\res" -o "$out\icon.zip"
    $resZips += "$out\icon.zip"
    $text = [System.IO.File]::ReadAllText($manifest) -replace '<application', '<application android:icon="@mipmap/ic_launcher"'
    $manifest = "$out\AndroidManifest.xml"
    [System.IO.File]::WriteAllText($manifest, $text)
}
& "$bt\aapt2.exe" link -o "$out\base.apk" -I $jar --manifest $manifest @resZips
if ($LASTEXITCODE) { throw "aapt2 link failed" }

# Add the native library (stored uncompressed would need page alignment; compressed + extractNativeLibs is fine).
Push-Location $out
Compress-Archive -Path "lib" -DestinationPath "$out\lib.zip" -Force
Pop-Location
Add-Type -AssemblyName System.IO.Compression.FileSystem
$apk = [System.IO.Compression.ZipFile]::Open("$out\base.apk", "Update")
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($apk, "$out\lib\arm64-v8a\lib$Game.so", "lib/arm64-v8a/lib$Game.so", "Optimal") | Out-Null
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
    & "$jdk\bin\javac.exe" -source 1.8 -target 1.8 -Xlint:-options -bootclasspath $jar -d "$out\classes" @sources
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
