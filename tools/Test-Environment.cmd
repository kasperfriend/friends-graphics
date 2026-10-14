@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio locator not found: "%VSWHERE%"
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo Visual Studio with MSBuild was not found.
  exit /b 1
)
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
  echo 32-bit compiler setup not found: "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\environment-test-obj mkdir build\environment-test-obj
cl /nologo /EHsc /std:c++20 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\EnvironmentRegression.cpp src\Environment\EnvironmentProfileManager.cpp src\Environment\EnvironmentProfile.cpp src\Environment\EnvironmentDatabase.cpp src\Game\WoWClientContext.cpp src\Game\WoWLocationProvider.cpp /Febuild\Release\EnvironmentRegression.exe /Fobuild\environment-test-obj\ /link user32.lib bcrypt.lib
if errorlevel 1 exit /b 1
build\Release\EnvironmentRegression.exe
