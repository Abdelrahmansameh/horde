@echo off
setlocal
REM See tools\build.bat: clear inherited VS env so vcvars64 actually re-initializes.
set VSCMD_VER=
set LIB=
set INCLUDE=
set LIBPATH=
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --build --preset windows-release
