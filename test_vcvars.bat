@echo off
echo BEFORE_VCVARS
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
echo AFTER_VCVARS ERRORLEVEL=%ERRORLEVEL%
where cl.exe
echo DONE
