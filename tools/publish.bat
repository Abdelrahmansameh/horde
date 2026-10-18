@echo off
REM Build/test the self-contained Windows executable. Does not upload a release.
REM Usage: tools\publish.bat [output-directory]
REM Default output: %%LOCALAPPDATA%%\horde-build\windows-standalone\publish\IMMUNE.exe
setlocal
cd /d "%~dp0.."

if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=C:\dev\vcpkg"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
  echo Set VCPKG_ROOT to a vcpkg checkout before publishing.
  exit /b 1
)

REM Reuse an existing x64 developer prompt. Otherwise locate VS 2022 without
REM assuming a Windows username, Visual Studio edition, or installation path.
if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :developer_ready
set "IMMUNE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%IMMUNE_VSWHERE%" (
  echo Run from an x64 Visual Studio developer prompt, or install VS 2022 C++ tools.
  exit /b 1
)
set "IMMUNE_VS_PATH="
for /f "usebackq tokens=*" %%i in (`call "%IMMUNE_VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "IMMUNE_VS_PATH=%%i"
if not defined IMMUNE_VS_PATH (
  echo Visual Studio 2022 with x64 C++ tools was not found.
  exit /b 1
)
call "%IMMUNE_VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

:developer_ready
if defined VCToolsVersion set "VCPKG_PLATFORM_TOOLSET_VERSION=%VCToolsVersion%"
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=3"
set "IMMUNE_PUBLISH_DIR=%~1"
if not defined IMMUNE_PUBLISH_DIR set "IMMUNE_PUBLISH_DIR=%LOCALAPPDATA%\horde-build\windows-standalone\publish"

cmake --preset windows-standalone
if errorlevel 1 exit /b 1
cmake --build --preset windows-standalone
if errorlevel 1 exit /b 1
ctest --preset windows-standalone
if errorlevel 1 exit /b 1
python "%~dp0verify_standalone.py" "%LOCALAPPDATA%\horde-build\windows-standalone\bin\immune.exe"
if errorlevel 1 exit /b 1

if not exist "%IMMUNE_PUBLISH_DIR%" mkdir "%IMMUNE_PUBLISH_DIR%"
if errorlevel 1 exit /b 1
copy /y "%LOCALAPPDATA%\horde-build\windows-standalone\bin\immune.exe" "%IMMUNE_PUBLISH_DIR%\IMMUNE.exe" >nul
if errorlevel 1 exit /b 1
echo Standalone executable ready: %IMMUNE_PUBLISH_DIR%\IMMUNE.exe
exit /b 0
