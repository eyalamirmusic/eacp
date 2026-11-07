@echo off
rem Runs a command inside the MSVC environment for this machines architecture.
rem Usage: Scripts\with-msvc.cmd "C:\Program Files\Git\bin\bash.exe" Scripts/run-process-tests
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "vsroot=%%i"
set "arch=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "arch=arm64"
call "%vsroot%\VC\Auxiliary\Build\vcvarsall.bat" %arch% >nul || exit /b 1
%*
