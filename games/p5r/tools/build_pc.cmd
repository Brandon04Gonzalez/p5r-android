@echo off
rem Windows build of P5R for debugging on the PC: MSVC environment + the NDK's clang-cl (the VS install has no
rem "C++ Clang tools"). Output: private\p5r\build\p5r.exe. Usage: build_pc.cmd [ninja args...]
setlocal
set "NDK_BIN=%LOCALAPPDATA%\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%~dp0..\..\..\private\venv\Scripts;%PATH%"
cd /d "%~dp0..\..\.."
if not exist private\p5r\build\build.ninja (
    cmake -S . -B private\p5r\build -G Ninja -DNX_GAME=p5r -DCMAKE_BUILD_TYPE=Release ^
        "-DCMAKE_C_COMPILER=%NDK_BIN%\clang-cl.exe" "-DCMAKE_CXX_COMPILER=%NDK_BIN%\clang-cl.exe" ^
        "-DNX_VULKAN_SDK=%CD%\private\third_party\Vulkan-Headers-1.4.357" || exit /b 1
)
ninja -C private\p5r\build %*
