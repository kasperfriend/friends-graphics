@echo off
cd /d "%~dp0.."
rem Load the Visual Studio C++ toolchain when cl.exe is not already on PATH.
where cl >nul 2>&1 || for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars32.bat`) do call "%%i" >nul
where cl >nul 2>&1 || (echo cl.exe not found. Install the Visual Studio C++ tools or run this from a Developer Command Prompt. & exit /b 1)
if not exist build\Release mkdir build\Release
if not exist build\water-test-obj mkdir build\water-test-obj
cl /nologo /EHsc /std:c++20 /O2 /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\WaterDeviceRegression.cpp src\Lighting\LocalLightManager.cpp src\D3D9\CelestialTracker.cpp src\Core\FrameContext.cpp src\Scene\DrawCallClassifier.cpp src\Core\ShaderCache.cpp /Febuild\Release\WaterDeviceRegression.exe /Fobuild\water-test-obj\ /link user32.lib d3d9.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
build\Release\WaterDeviceRegression.exe
rem Exit 77 explicitly reports a headless runner without D3D9 HAL, not a passing GPU test.
if errorlevel 78 exit /b 1
if errorlevel 77 exit /b 0
if errorlevel 1 exit /b 1
exit /b 0
