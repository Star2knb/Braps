@echo off
rem Configure + build with the Visual Studio x64 toolchain.
rem Usage: build.bat [debug|release|clang-debug]   (default: release)
setlocal EnableDelayedExpansion
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found - is Visual Studio installed?
    exit /b 1
)
rem Delayed expansion: the ")" in "Program Files (x86)" would otherwise close the for-block.
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo No Visual Studio install with the C++ x64 tools was found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"

cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
