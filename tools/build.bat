@echo off
REM Configure/build/test helper. Sets up the MSVC environment and VCPKG_ROOT,
REM then runs the requested CMake step. Usage:
REM   tools\build.bat configure [preset]
REM   tools\build.bat build     [preset]
REM   tools\build.bat test      [preset]
REM   tools\build.bat all       [preset]
REM Default preset: windows-release

setlocal
if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=C:\dev\vcpkg
set STEP=%1
if "%STEP%"=="" set STEP=all
set PRESET=%2
if "%PRESET%"=="" set PRESET=windows-release

REM Pin vcpkg to the same toolset CMake links with. VS18 (MSVC 14.51) is also
REM installed, and vcpkg otherwise picks the newest VS it finds -- it would
REM rebuild Catch2 with 14.51, whose STL calls __std_find_last_not_ch_pos_1 and
REM other helpers absent from 14.44 msvcprt.lib, so linking immune_tests.exe
REM fails with LNK2019 on exactly those symbols.
set "VCPKG_VISUAL_STUDIO_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCPKG_PLATFORM_TOOLSET_VERSION=14.44.35207"

set VSCMD_VER=
set LIB=
set INCLUDE=
set LIBPATH=
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0.."

if "%STEP%"=="configure" goto :configure
if "%STEP%"=="build"     goto :build
if "%STEP%"=="test"      goto :test
if "%STEP%"=="all"       goto :configure
echo unknown step: %STEP%
exit /b 2

:configure
cmake --preset %PRESET% || exit /b 1
if not "%STEP%"=="all" exit /b 0

:build
cmake --build --preset %PRESET% || exit /b 1
if not "%STEP%"=="all" exit /b 0

:test
ctest --preset %PRESET% || exit /b 1
exit /b 0
