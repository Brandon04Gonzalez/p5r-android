#!/usr/bin/env bash
# Android build of P5R, native execution (the bash port of android/build_apk.ps1 -Game p5r -Nce): cmake (NDK) ->
# libp5r.so + libp5rimport.so + libspirvharness.so -> aapt2 -> javac/d8 -> zipalign -> apksigner ->
# private/p5r/apk_nce/p5r.apk.
#
# Needs the Android SDK 35 (ANDROID_HOME or ANDROID_SDK_ROOT) with NDK 28.2.13676358 and build-tools 35.0.0, a JDK
# (17+), cmake, ninja, zip, the Vulkan headers 1.4.357 (VULKAN_SDK, or private/third_party/Vulkan-Headers-1.4.357), and
# the shader translator built once from the repository root (games/p5r/tools/build_harness_android.sh):
#   docker run --rm -v "$PWD:/src" -w /src mcr.microsoft.com/dotnet/sdk:10.0 sh games/p5r/tools/build_harness_android.sh
#
# Signing uses ~/.android/debug.keystore (android/android) unless P5R_KEYSTORE (and optionally
# P5R_KEYSTORE_PASSWORD, P5R_KEY_ALIAS, P5R_KEY_PASSWORD) is set. Usage: build_android.sh [--install]
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
install=0
while [ $# -gt 0 ]; do
    case "$1" in
        --install) install=1; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
ndk="$sdk/ndk/28.2.13676358"
bt="$sdk/build-tools/35.0.0"
jar="$sdk/platforms/android-35/android.jar"
for p in "$ndk" "$bt" "$jar"; do [ -e "$p" ] || { echo "missing $p" >&2; exit 1; }; done

priv="$root/private/p5r"
test_shoo="${TEST_SHOO:-0}"
if [ "$test_shoo" = 1 ]; then
    out="$priv/apk_testshoo"
    manifest="$root/games/p5r/AndroidManifest.testshoo.xml"
    apk_name="p5r-testshoo"
else
    out="$priv/apk_nce"
    manifest="$root/games/p5r/AndroidManifest.xml"
    apk_name="p5r"
fi
build="$priv/ba_nce"
harness="$root/private/build/spirv_harness_android/libspirvharness.so"
if [ ! -f "$harness" ]; then
    echo "missing $harness" >&2
    echo "build it once from the repository root: docker run --rm -v \"$root:/src\" -w /src mcr.microsoft.com/dotnet/sdk:10.0 sh games/p5r/tools/build_harness_android.sh" >&2
    exit 1
fi

export VULKAN_SDK="${VULKAN_SDK:-$root/private/third_party/Vulkan-Headers-1.4.357}"
# CMake asks for Include/ (the Windows SDK layout); the Vulkan-Headers checkout has include/.
if [ ! -d "$VULKAN_SDK/Include" ] && [ -d "$VULKAN_SDK/include" ]; then
    vulkan_stage="$priv/vulkan-sdk"
    mkdir -p "$vulkan_stage"
    ln -sfn "$VULKAN_SDK/include" "$vulkan_stage/Include"
    export VULKAN_SDK="$vulkan_stage"
fi

if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
        -DNX_GAME=p5r -DNX_NCE=ON
fi
cmake --build "$build" --target p5r p5rimport

host="$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64"
llvm="$ndk/toolchains/llvm/prebuilt/$host/bin"
[ -d "$llvm" ] || { echo "missing NDK toolchain $llvm" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/lib/arm64-v8a" "$out/res"
"$llvm/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/libp5r.so" "$build/libp5r.so"
"$llvm/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/libp5rimport.so" "$build/libp5rimport.so"
"$llvm/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/libspirvharness.so" "$harness"

"$bt/aapt2" compile --dir "$root/android/res" -o "$out/res.zip"
res_zips=("$out/res.zip")
icon_res="$root/games/p5r/icon/res"
[ -d "$icon_res" ] || icon_res="$priv/icon/res"
if [ -d "$icon_res" ]; then
    "$bt/aapt2" compile --dir "$icon_res" -o "$out/icon.zip"
    res_zips+=("$out/icon.zip")
    sed 's|<application|<application android:icon="@mipmap/ic_launcher"|' "$manifest" > "$out/AndroidManifest.xml"
    manifest="$out/AndroidManifest.xml"
fi
echo "Linking APK with manifest: $manifest"
"$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$manifest" "${res_zips[@]}"

(cd "$out" && zip -q -r -9 base.apk lib)

java="$root/games/p5r/java"
if [ -d "$java" ]; then
    mkdir -p "$out/classes" "$out/dex"
    find "$java" -name '*.java' > "$out/sources.txt"
    javac -encoding UTF-8 -source 1.8 -target 1.8 -Xlint:-options -bootclasspath "$jar" -d "$out/classes" \
        @"$out/sources.txt"
    find "$out/classes" -name '*.class' > "$out/classes.txt"
    "$bt/d8" --release --min-api 30 --lib "$jar" --output "$out/dex" @"$out/classes.txt"
    (cd "$out/dex" && zip -q -j "$out/base.apk" classes.dex)
fi

"$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
ks="${P5R_KEYSTORE:-$HOME/.android/debug.keystore}"
ks_pass="${P5R_KEYSTORE_PASSWORD:-android}"
key_alias="${P5R_KEY_ALIAS:-androiddebugkey}"
key_pass="${P5R_KEY_PASSWORD:-$ks_pass}"
[ -f "$ks" ] || { echo "missing keystore $ks" >&2; exit 1; }
"$bt/apksigner" sign --ks "$ks" --ks-key-alias "$key_alias" --ks-pass "pass:$ks_pass" \
    --key-pass "pass:$key_pass" --out "$out/$apk_name.apk" "$out/aligned.apk"
"$bt/apksigner" verify "$out/$apk_name.apk"
if [ "$test_shoo" = 1 ]; then
    badging="$("$bt/aapt2" dump badging "$out/$apk_name.apk")"
    actual_package="$(printf '%s\n' "$badging" | sed -n "s/^package: name='\([^']*\)'.*/\1/p")"
    if [ "$actual_package" != "io.github.p5rrecomp.testshoo" ]; then
        echo "Test Shoo APK package is '$actual_package', expected 'io.github.p5rrecomp.testshoo'" >&2
        exit 1
    fi
    echo "Verified Test Shoo package: $actual_package"
fi
echo "APK: $out/$apk_name.apk ($(du -h "$out/$apk_name.apk" | cut -f1))"

if [ "$install" = 1 ]; then "$sdk/platform-tools/adb" install -r "$out/$apk_name.apk"; fi
