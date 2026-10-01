@echo off
rem Configure + build with the Visual Studio toolchain (x64, or x86 for the x86-* presets).
rem Usage: build.bat [debug|release|clang-debug|x86-debug|x86-release]   (default: release)
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
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"

rem x86-* presets build the 32-bit parts (hook32, inject32) with the x64-hosted x86 compiler.
set "VCVARS=vcvars64.bat"
if /i "%PRESET:~0,4%"=="x86-" set "VCVARS=vcvarsamd64_x86.bat"
call "%VSINSTALL%\VC\Auxiliary\Build\%VCVARS%" >nul || exit /b 1

rem Always build the project this script lives in, whatever the current directory is.
pushd "%~dp0" || exit /b 1
cmake --preset %PRESET% || (popd & exit /b 1)
cmake --build --preset %PRESET% || (popd & exit /b 1)
popd
