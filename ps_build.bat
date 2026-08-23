@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
echo VCVARS_DONE
cmake --build --preset windows-release
echo BUILD_EXIT=%ERRORLEVEL%
