# Development

## Requirements

- Visual Studio 2026 with the MSVC C++ x86/x64 tools and `v145` platform toolset.
- Windows SDK and MSBuild.
- A 32-bit `Win32` build target. The Ascension client is a 32-bit D3D9 process.

## Building locally

Build from a Visual Studio developer PowerShell:

```powershell
msbuild ModernWoWRenderer.sln /m /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145
```

The installable output is written to `build\Release\`. The post-build step copies both INI files, the local-light manifests, and weather textures alongside `d3d9.dll`.

## Continuous integration and releases

`.github/workflows/build.yml` builds Release for each pull request targeting `main` and each push to `main`. Feature-branch pushes are not built a second time alongside their PR. A manual Build run is also available for any branch; enable its `debug` option when a Debug build is needed.

Normal CI has two jobs: the Windows Release build (including shader validation, water/weather device tests, package verification and artifact upload), and the fast portable regressions on Linux. Changelog validation runs separately only when fragments or its workflow change. Tests for current renderer behavior are retained; duplicate runs and separate runner startups are removed.

Each successful Release build uploads an install-ready artifact containing:

- `d3d9.dll`
- `ModernWoWRenderer.ini`
- `GraphicsEffects.ini`
- `EnvironmentProfiles.ini` (location verification and environment configuration)
- `LocationGraphics.ini` (the per-location override file, shipped empty so it can be filled in by hand)
- required `MaterialCache` local-light manifests
- packaged weather textures

Artifacts are retained for 14 days, including PR and manual builds. Publishing a GitHub Release is separate: run the manual `Release` workflow to build, validate and publish the ZIP. Ordinary pushes do not publish releases.

## Validation

The repository includes focused regression and shader checks:

```powershell
tools\Test-DeviceHealth.cmd
tools\Test-FrameClock.cmd
tools\Test-Wind.cmd
tools\Test-Atmosphere.cmd
tools\Test-Water.cmd
tools\Test-Weather.cmd
powershell -ExecutionPolicy Bypass -File tools\ValidateShaders.ps1
```

The device-free suites (wind, clouds, water, underwater, ambient occlusion, local lighting, camera anchoring, location and settings caches, the overlay geometry and the tracked-state mirror) also build and run on any Linux/macOS machine in one command - the same battery CI runs on every commit:

```bash
tools/RunPortableRegressions.sh          # build all suites in parallel, run them
tools/RunPortableRegressions.sh --san    # every suite under ASan+UBSan as well
tools/RunPortableRegressions.sh --only water,underwater   # one suite while iterating
```

The tests cover eight-way wind/time/weather response, wind/cloud/wave phase continuity, F7 wind settings, fog-field coverage and motion, weather continuity, held-light lifetime, depth-capture policies, embedded shader compilation, the ambient-occlusion look (open surfaces untouched, contacts and corners darkened, fade with distance, blur not crossing edges), and the device-loss, frame-gap and retry rules used after Alt+Tab. Final visual verification still requires the Ascension client because these effects depend on its real draw order, shaders, textures, and device-reset behavior.

## Project structure

```text
ModernWoWRenderer.cpp        D3D9 proxy exports, hook dispatch, and frame orchestration
VolumeIntegration.h         Atmosphere and volumetric composition
WeatherVisuals.h            Wind-driven rain and snow/sand continuity rendering
WaterEffect.h               Water replacement and shading
NativeShadowDiagnostics.h   Native cascade discovery and enhancement
TuningOverlay.h             F7 in-game settings interface
src/Core/                   Shared frame state, math, and shader caches
src/D3D9/                   Camera/depth capture and render-state guards
src/Effects/                Volumetric, ambient-occlusion and local-light render passes
src/Environment/            Location profiles, shared wind and F7 controls
src/Water/                  Sea state, wave fields and integrated wind-driven motion
src/Lighting/               Trusted local-light collection and lifetime rules
src/Scene/                  Draw-call classification
src/Diagnostics/            Runtime logging, capture, and profiling
tools/                      Offline regression and shader validation tools
Screenshots/                README and documentation images
```

The proxy loads the system Direct3D 9 runtime, forwards the normal API, observes Ascension's render state, and injects its passes before the user interface. It does not replace game data files or network behavior.
