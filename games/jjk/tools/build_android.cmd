@echo off
rem Build the JJK Android NCE APK using the same native build, launcher, and signing pipeline as P5R.
rem Run from a Windows developer shell with Android SDK/NDK, JDK, CMake, Ninja, Vulkan headers, and Docker available.
rem Build the shared Android SPIR-V harness once first if it is missing:
rem   docker run --rm -v "%CD%:/src" -w /src mcr.microsoft.com/dotnet/sdk:10.0 sh games/p5r/tools/build_harness_android.sh
setlocal
cd /d "%~dp0..\..\.."
set "PATH=%CD%\private\venv\Scripts;%PATH%"
set "VULKAN_SDK=%CD%\private\third_party\Vulkan-Headers-1.4.357"
powershell -NoProfile -ExecutionPolicy Bypass -File android\build_apk.ps1 -Game jjk -Nce %*
if errorlevel 1 exit /b %errorlevel%
