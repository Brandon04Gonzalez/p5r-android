#!/bin/sh
# The shader translator (tools/maxwell/spirv_harness: Ryujinx's translator) as a native Android library, so the app
# translates the player's shaders on the phone: private/build/spirv_harness_android/libspirvharness.so (arm64).
# NativeAOT for linux-bionic needs a Linux host: run it in Docker from the repository root (Windows: Docker Desktop):
#   docker run --rm -v "%CD%:/src" -w /src mcr.microsoft.com/dotnet/sdk:10.0 sh games/p5r/tools/build_harness_android.sh
# It downloads the Linux Android NDK into private/third_party once. Needs the Ryubing source in
# private/third_party/ryubing (prepare_data.ps1 or docs/nvn/TRANSLATOR.md).
set -e
NDK_VER=r28c
NDK=/src/private/third_party/android-ndk-$NDK_VER-linux
if ! command -v patch >/dev/null || ! command -v unzip >/dev/null; then
    apt-get update -qq && apt-get install -y -qq patch unzip >/dev/null
fi
if [ ! -d "$NDK" ]; then
    curl -sSL -o /tmp/ndk.zip "https://dl.google.com/android/repository/android-ndk-$NDK_VER-linux.zip"
    unzip -q /tmp/ndk.zip -d /tmp/ndk && mv "/tmp/ndk/android-ndk-$NDK_VER" "$NDK" && rm -rf /tmp/ndk /tmp/ndk.zip
fi
export PATH="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
OUT=/src/private/build/spirv_harness_android
dotnet publish tools/maxwell/spirv_harness/SpirvHarness.csproj -c Release -r linux-bionic-arm64 \
    -p:NxBuildDir=spirv_harness_android -p:PublishAot=true -p:NativeLib=Shared -p:OutputType=Library \
    -p:DisableUnsupportedError=true -p:PublishAotUsingRuntimePack=true \
    -p:CppCompilerAndLinker=aarch64-linux-android29-clang -p:ObjCopyName=llvm-objcopy \
    -p:StripSymbols=true -o "$OUT/publish"
cp "$OUT/publish/spirv_harness.so" "$OUT/libspirvharness.so"
ls -la "$OUT/libspirvharness.so"
