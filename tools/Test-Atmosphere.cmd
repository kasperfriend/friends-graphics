@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0.."
if not exist build\atmosphere-test-obj mkdir build\atmosphere-test-obj
cl /nologo /EHsc /std:c++20 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\AtmosphereRegression.cpp src\Effects\DirectionalVolumetricLighting.cpp src\Effects\LocalLightingRenderer.cpp src\Effects\VolumetricClouds.cpp src\Lighting\LocalLightManager.cpp src\D3D9\DepthCapture.cpp src\Core\FrameContext.cpp src\Core\ShaderCache.cpp src\Diagnostics\RendererDiagnostics.cpp src\Diagnostics\PerformanceProfiler.cpp /Febuild\Release\AtmosphereRegression.exe /Fobuild\atmosphere-test-obj\ /link user32.lib ole32.lib d3d9.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
build\Release\AtmosphereRegression.exe
