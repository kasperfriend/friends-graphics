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
