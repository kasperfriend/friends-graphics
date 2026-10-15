---
area: renderer
type: fixed
audience: players
title: Every shader compiles at most once per machine: on-disk bytecode cache
---
Every launch re-ran fxc for all of the renderer's shaders, even though
their source never changes and the output is deterministic: the water
replacement stage (the largest single compile, several seconds) on a
worker thread, the atmosphere pass's nine shaders, the legacy volume
pass's ten, the cloud deck's three, ambient occlusion, global
illumination, dynamic lights, the weather particles and the two debug
highlighters.

All of them now read a small `ShaderCache` directory next to the DLL
first. The entry name is a hash of the shader source plus entry point
plus profile, so an edited shader can never collide with an old entry.
Only the first launch on a machine pays the compile; every later launch
reads the bytecode from disk and skips fxc entirely.

The cache is a pure speedup: a miss, a truncated file or a bad header
falls back to compiling exactly as before, the driver's
CreatePixelShader/CreateVertexShader stays the final authority, and if
the driver ever refuses a cached blob the renderer recompiles once and
retries before falling back to native rendering. Saves are atomic (temp
file + rename) so an interrupted write can never leave a torn entry, and
a machine that has never run a given shader still behaves exactly as
before. No shader output, log or behavior changes - the same bytes are
just produced once instead of on every launch.
