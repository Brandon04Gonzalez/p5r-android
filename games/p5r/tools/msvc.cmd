@echo off
rem MSVC x64 + the repo's venv (cmake, ninja, python) for host tools of the P5R port. Usage: msvc.cmd <command...>
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%~dp0..\..\..\private\venv\Scripts;%PATH%"
cd /d "%~dp0..\..\.."
%*
