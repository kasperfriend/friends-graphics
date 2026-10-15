# Shared dynamic wind

Wind is an **internal world-space value**, not a particle effect or a visible volume.
One frame-boundary simulation drives cloud drift and water waves. Plants, trees,
grass, precipitation tilt and local fog are **not connected** by this change.

## F7 controls

Open **F7 → WIND**. Settings use the existing location mechanism: global defaults
in `[Wind]` in `GraphicsEffects.ini`, with verified zone/subarea overrides in
`LocationGraphics.ini`. On unverified clients F7 edits the global file. F12 reloads
settings without resetting the wind, its gust phases, or the effect phases.

| Control / INI key | Default | Meaning |
| --- | --- | --- |
| Shared wind / `Enabled` | 1 | Connect the shared wind. Off restores legacy authored water/cloud motion; it does not disable those effects. |
| Time variation / `Dynamic` | 1 | Slowly choose new compass targets and vary strength over time. Off holds the selected direction and removes gust variation; weather can still change strength. |
| Base direction / `Direction` | 1 (NE) | `0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW`. The slider displays names. In dynamic mode this is the starting direction; editing it requests a smooth turn. |
| Base strength / `StrengthPercent` | 100 | 0–200%. 100% is a reference breeze; 0 remains calm even in a storm. Live strength is bounded to 300% after weather/gust response. |
| Gust variation / `GustPercent` | 35 | 0–100% bounded time variation about the weather-adjusted baseline. 0 removes gusts without stopping direction changes. |
| Gust period / `GustSeconds` | 18 | 2–120 seconds for the principal gust cycle; slower/faster components avoid a single repeating pulse. |
| Direction interval / `DirectionChangeSeconds` | 120 | 15–600 seconds between compass targets. Each choice is an adjacent or quarter turn. |
| Response time / `TransitionSeconds` | 30 | 1–120 seconds. Strength reaches about 95% of a fixed rising target in this time; clearing is 1.5× slower. Also limits heading changes to 45 degrees per interval of this duration. |
| Weather response / `WeatherResponsePercent` | 100 | 0–200%. Rain, snow and sand increase the target strength; 0 ignores weather. |
| Drive waves / `AffectWaves` | 1 | Apply the shared direction and strength to water. Off restores its original direction/native-weather response. |
| Drive clouds / `AffectClouds` | 1 | Apply the shared direction and strength to cloud shapes, detail, weather map and height shear. Off restores the cloud section's legacy wind controls. |
| Wind debug view / `DebugView` | 0 | Optional text HUD, including when F7 is closed. No world-space wind effect. |

**Directions mean travel toward**, not the meteorological “wind from” convention.
WoW's horizontal axes are **+X north, −Y east**; +Z is up. Targets use eight compass
sectors, with a normalized direction smoothly interpolated between them rather
than snapping the scene in 45-degree steps. The debug HUD reports the current
heading, nearest sector, target sector, time to the next target, current/target
strength, XY velocity, native weather input and link status.

For a steady test wind, set **Time variation OFF**, **Weather response 0**, select
a direction and strength, and allow the response to settle. At 0% strength clouds
stop wind-driven drift/lean; independent cloud evolution and weather-driven
coverage can continue. Water eases down to residual swell, with no wind-driven
foam. Wave speed 0 stops wave travel/turning (energy can still respond to wind);
Water's wave switch removes the waves entirely.

## Existing effect controls

- `[VolumetricClouds] WindSpeed` is cloud drift speed in world yards/second at
  shared strength 100% (still default 26). It multiplies shared strength, so
  `WindSpeed=0` stops wind advection. `WindShearPercent` is likewise a local gain.
  `WindAngle` is ignored while linked. When unlinked it retains its legacy
  **sampling** angle: sampling `p + offset` travels opposite the offset.
- `[Water] WindStrengthPercent` remains the body's foam/wind bias; 0 still
  suppresses wind-driven crest/shoal foam. It is not a second global wind.
- `[Water] WeatherResponsePercent` controls how strongly that body reacts to
  departures of shared strength from 100%. At 0 its authored energy is retained;
  direction can still follow wind. When unlinked it reverts to the original
  direct native-weather response. A storm is **not counted twice**.
- Zone water exposure, wave size/height, wave speed, geometry-wave switches,
  foam limits and the existing sea-state easing remain in effect. Waves have
  additional directional inertia; the HUD calls this out rather than claiming
  that a heavy sea turns instantaneously with the air.
- Cloud/water rendering switches and F10 do not stop the wind simulation. F11
  still gates the graphics package. Weather detection is independent of native
  particle replacement and texture availability.

Weather is the existing **held native observation**, including its F7/focus-gap
protection. The native type and authored local intensity are known, not a
verified server-side severity or forecast. The zone intensity multiplier is
applied once before the common [0,1] clamp. At full local intensity and 100%
weather response, target strength multipliers are 1.8 for rain, 1.4 for snow and
2.1 for sand, before gusts and the 300% ceiling. “Time variation” means elapsed
simulation time; no unverified in-game time-of-day memory is read.

## Integration contract

- `src/Environment/WindSystem.h` is portable, device-free simulation policy.
  `WindState` exposes a unit horizontal `direction`, normalized `strength`,
  velocity in **world yards/second** (`12 × strength`), target, weather and
  bounded frame delta. Future vegetation should read this state, not create
  its own random wind, clock or weather reader. Read only on the render thread
  or copy the snapshot before using it on another thread.
- `ModernWoWRenderer.cpp` updates wind once, **after all rendering**, including
  the Present fallback compositor, and after native-weather observation. It
  publishes `FrameContext::wind` for the next frame and prepares that frame's
  sea state. Frame/weather observation hooks remain installed independently
  of visual-backend readiness. Water draws and clouds cannot see different wind
  revisions depending on whether clouds composited before UI or at Present.
- Clouds use the published delta and revision; retrying a cloud pass cannot
  advance its phases twice. All three noise layers integrate offsets, and
  both the visible deck and its shadow/light field use those same offsets.
  Sampling velocity is the negative of travel velocity, including top shear.
- `src/Water/WaveMotion.h` integrates each dispersion/shear/UV phase separately
  in double precision and wraps it in its own period. Changing speed, shear or
  wind never recalculates displacement from `session age × new speed`.
- A turning wave lattice is re-anchored at a local world pivot once per frame.
  The swell, warp and group phases are compensated together. This prevents a
  tiny turn from sweeping a sea thousands of yards away around world zero.
  Each draw still uses its **own** rigid model/view anchor, including reflected
  cameras. Moving the camera alone does not move the field. With no valid
  camera/pivot, turns wait rather than rotating around a fabricated origin.
- GPU resets drop only GPU resources/obsolete camera anchors, not wind, sea
  state or accumulated phases. Invalid/negative/non-finite deltas advance
  nothing; long pauses advance at most 0.1 seconds. No catch-up storm after
  Alt+Tab. Configuration values and shader-bound outputs are bounded/finite.
- The wind debug HUD is isolated by the existing device **and tracked-state**
  transaction and has no input hook. Disabled debug performs no device work.

The existing cloud/water offline preview scenes remain **fixed, unlinked
reference scenes**, not simulations of a live dynamic wind history. The wind
regression exercises the actual shared wind, cloud policy and wave integrator.
The wind link itself required no water/cloud shader ABI change; later water
performance work may change where an already-linked wave is evaluated without
changing its shared-wind input.

## Coupling audit (2026-10-15)

The Booty Bay water-performance pass re-traced both consumers rather than
assuming the WIND HUD implied linkage:

1. `ModernWoWRenderer.cpp` advances `WindSystem` once after rendering and
   publishes the resulting immutable sequence in `FrameContext` for the next
   frame. `ResetForNewFrame` deliberately retains that snapshot.
2. `VolumetricClouds::Render` calls `ApplyCloudWind` only for a new sequence.
   The shared direction/strength advances base, detail and weather-map offsets;
   `shear` separately offsets the top layer. The shadow/light field uses those
   same offsets, so it cannot drift away from the visible deck.
3. `watereffect::Present` reads the same snapshot. `WaterSeaState` converts its
   strength and weather into wave energy, future phase speed, cross-current and
   foam wind; `WaveMotion` consumes its direction and advances all long swell,
   short swell, warp, grouping, chop and UV phases once.
4. `AffectClouds=0` and `AffectWaves=0` independently select each effect's legacy
   path. They do not mutate the shared snapshot or accidentally disable the
   other consumer.

`tools/WindRegression.cpp` pins all eight world-axis travel signs for both
consumers, cloud strength/shear, wave direction and energy, sequence-once
advancement, reload/disable continuity, frame publication order and the actual
production call sites. Both coupling links are therefore present and synchronized;
a live-client all-heading check remains the visual acceptance test.

## Validation

Portable tests (from repository root):

```sh
mkdir -p build
g++ -O2 -std=c++20 -Wall -Wextra -Werror -pedantic tools/WindRegression.cpp -o build/wind-test
./build/wind-test
```

`tools/Test-Wind.cmd` additionally checks real Windows global/zone/subarea INI
persistence and reload. `tools/Test-OverlayState.cmd` checks the actual debug
HUD with hooked D3D9 setters, both with F7 open and closed. Both are wired into
Windows CI; portable wind tests are wired into Linux CI. Water, cloud, frame
clock, shader-source and overlay-layout regressions remain required.

In-client acceptance checks (require Windows and the Ascension client):

1. Use fixed wind with weather response 0. Select all eight headings, allowing
   wind and sea inertia to settle. Compare motion against the minimap compass,
   not camera-facing left/right; turn the camera and visit distant coordinates.
2. Enable time variation. Confirm target changes, smooth actual heading and
   strength rising **and falling** in the HUD. Watch cloud shadows move with
   the deck and wave geometry stay aligned with its shading.
3. Observe native rain/snow/sand onset and clearing, with precipitation
   replacement both enabled and disabled. No duplicate storm amplification.
4. Change speed, direction interval and strength after a long session. No
   phase teleport. Test F7 close/open, F12, location overrides, loading and
   Alt+Tab/device reset. Confirm debug/graphics state does not leak.
5. Disable each link and the wind master, then re-enable. Legacy effect
   controls remain usable; the shared simulation is not reset by F7 edits.
