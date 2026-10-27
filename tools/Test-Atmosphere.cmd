@echo off
setlocal
cd /d "%~dp0.."
rem Load the Visual Studio C++ toolchain when cl.exe is not already on PATH.
where cl >nul 2>&1 || for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars32.bat`) do call "%%i" >nul
where cl >nul 2>&1 || (echo cl.exe not found. Install the Visual Studio C++ tools or run this from a Developer Command Prompt. & exit /b 1)
if not exist build\Release mkdir build\Release
if not exist build\atmosphere-test-obj mkdir build\atmosphere-test-obj
echo Building atmosphere and cloud regression...
cl /nologo /EHsc /std:c++20 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\AtmosphereRegression.cpp src\Effects\DirectionalVolumetricLighting.cpp src\Effects\LocalLightingRenderer.cpp src\Effects\VolumetricClouds.cpp src\Lighting\LocalLightManager.cpp src\D3D9\CelestialTracker.cpp src\D3D9\DepthCapture.cpp src\Core\FrameContext.cpp src\Core\ShaderCache.cpp src\Diagnostics\RendererDiagnostics.cpp src\Diagnostics\PerformanceProfiler.cpp /Febuild\Release\AtmosphereRegression.exe /Fobuild\atmosphere-test-obj\ /link user32.lib ole32.lib d3d9.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
echo Running atmosphere and cloud regression...
build\Release\AtmosphereRegression.exe
set "result=%errorlevel%"
echo Atmosphere regression process exited with code %result%.
exit /b %result%
