@echo off
cd /d "%~dp0.."
rem Load the Visual Studio C++ toolchain when cl.exe is not already on PATH.
where cl >nul 2>&1 || for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars32.bat`) do call "%%i" >nul
where cl >nul 2>&1 || (echo cl.exe not found. Install the Visual Studio C++ tools or run this from a Developer Command Prompt. & exit /b 1)
if not exist build\Release mkdir build\Release
if not exist build\devicehealth-test-obj mkdir build\devicehealth-test-obj
cl /nologo /EHsc /std:c++20 /O2 /W4 /WX tools\DeviceHealthRegression.cpp /Febuild\Release\DeviceHealthRegression.exe /Fobuild\devicehealth-test-obj\
if errorlevel 1 exit /b 1
build\Release\DeviceHealthRegression.exe
