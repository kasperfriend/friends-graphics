# F7 effect fade: GPU state versus tracked state

## Cause

`tuningoverlay::Draw` saved/restored `D3DSBT_ALL`, but that only restored the
D3D9 device. Its calls to `SetVertexShader`, `SetPixelShader`, `SetFVF` and
`SetRenderState` also passed through the proxy hooks, overwriting
`g_trackedState` with the menu's fixed-function shaders, alpha blend and
disabled depth flags. `IDirect3DStateBlock9::Apply` normally restores state
internally, without replaying the hooked device setters.

The device and software mirror consequently disagreed after each F7 draw.
A client need not reissue cached setters every frame, so later world draws
could be rejected or misclassified until the game changed those states again.
`ObserveTorchDraw` requires depth enabled, alpha blending and a confirmed
shader hash. Missing observations preserve an attached emitter for 1 second,
then fade it until the 3.5-second expiry. This explains why the symptom can
be gradual rather than an immediate light toggle. Camera capture, weather,
ground capture and draw classification also consult the same mirror.

## Correction

The menu now uses `ScopedStateBlock`, which captures GPU state and the complete
tracked mirror together. Restoration applies the device block **first**, then
restores shader/declaration identities, FVF, shader hashes and depth/blend/test
flags in the mirror. Explicit restoration is idempotent; RAII also handles early
returns and exceptions. Failed capture leaves drawing untouched.

No light expiration constants were extended and no world effects are paused
while the menu is open. Existing F7 input capture and effect toggles are unchanged.

## Regression checks

- `TrackedRenderStateRegression.cpp` runs portably, including in Linux CI.
  It simulates 600 menu frames without redundant world setters, verifies every
  tracked field and torch eligibility, and covers nested scopes, early returns,
  exceptions and repeated restoration.
- `Test-OverlayState.cmd` renders the real F7 menu on a system D3D9 device with
  the five relevant setters instrumented like the proxy. It checks the actual
  device **and** mirror after open/closed/reopened draws, without reasserting
  world state each frame. An additional early-return check covers the combined
  state guard. Windows CI runs it; no D3D9 HAL is an explicit skip (exit 77),
  not a successful GPU test.

In-game confirmation: stand by an equipped torch and a world lamp, keep F7 open
for at least ten seconds, switch tabs without editing values, then close/reopen.
Lighting, weather and atmosphere should remain stable. An actual effect toggle
must still take effect while the menu stays open.

## Why an enabled effect could still draw nothing until a reload (2026-10-19)

### The report

Every effect is on in F7, and the world draws with none of them. A teleport (or any loading
screen) makes them all appear, and they stay on afterwards.

### The loop

The capture path was a cycle that could close on itself:

1. `table[39/40/43]` (`SetDepthStencilSurface`, `GetDepthStencilSurface`, `Clear`) were hooked
   **only** when `volume::enabled || watereffect::enabled || watereffect::compiling` at the
   moment the device was hooked or reset.
2. Only `HookedClear` could start a depth capture, so without those hooks no INTZ surface ever
   existed.
3. `volume::BeforeDraw` attempted `CaptureCamera` only when that surface existed **and**
   `owner == device`, where `owner` is published by the same capture. No capture meant no
   camera attempt at all.
4. `FrameContext::cameraValid` is published only by `CameraCapture`, so clouds, shafts, local
   lights, ambient occlusion, reflections and the underwater pass all had no camera.
5. The only event that re-ran step 1 with different flags was a device reset - a loading
   screen or a teleport. That is the "reload" the player found.

A second latch sat on the same path: if anything in the effect frame threw before
`volume::Present` reached its `Finish`, the capture owner stayed set for the rest of the
session and every later clear was refused. A third: a failed `CreatePixelShader` for the water
replacement kept native water "until device reset".

### The fixes

- The depth hooks are installed unconditionally, on every device and after every Reset. Each
  hook keeps its own live gates, so an install with all effects off costs one branch per clear.
- `CaptureCamera` no longer requires a captured depth surface. The camera's 27 constants, the
  viewport and the projection come from the draw; only the *passes* need depth. A camera
  captured on a refused-depth frame (multisampling, an exotic backbuffer format) is used by the
  pose-only passes immediately, and the composite is skipped in favour of the image pass
  (`PostProcessWithoutCamera`), because there is no scene copy to composite onto.
- The `owner != device` early-out in `BeforeDraw` now only refuses a *foreign* owner, not a
  frame with no capture yet.
- The effect frame's teardown (`waterreflection::Finish`, `volume::Finish`) also runs in the
  `Present` catch, so a thrown frame cannot latch the capture.
- Water's shader-creation failure retries after `kOptionalPassRetryMs`, like the cloud deck's,
  instead of keeping native water until a device reset.

### Verification

`tools/DeviceHealthRegression.cpp` pins all five: no flag-gated hook install, both hook passes
installing the Clear hook, the camera attempt without a depth surface, the teardown in the
catch, and the shader-creation cooldown.
