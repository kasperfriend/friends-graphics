@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0.."
if not exist build\environment-test-obj mkdir build\environment-test-obj
cl /nologo /EHsc /std:c++20 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\EnvironmentRegression.cpp src\Environment\EnvironmentProfileManager.cpp src\Environment\EnvironmentProfile.cpp src\Environment\EnvironmentDatabase.cpp src\Game\WoWClientContext.cpp src\Game\WoWLocationProvider.cpp /Febuild\Release\EnvironmentRegression.exe /Fobuild\environment-test-obj\ /link user32.lib bcrypt.lib
if errorlevel 1 exit /b 1
build\Release\EnvironmentRegression.exe
