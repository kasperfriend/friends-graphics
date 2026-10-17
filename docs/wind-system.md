# Shared dynamic wind

Wind is an **internal world-space value**, not a particle effect or a visible volume.
One frame-boundary simulation drives cloud drift and water waves; the optional
camera-relative rain field and snow/sand continuity also follow it. Native snow
and sand particles still use the game's own motion, and nothing in the world geometry
moves with it - no grass, plant, bush or tree sway. Local fog is not connected.

## F7 controls

Open **F7 → WIND**. Settings use the existing location mechanism: global defaults
in `[Wind]` in `GraphicsEffects.ini`, with verified zone/subarea overrides in
`LocationGraphics.ini`. On unverified clients F7 edits the global file. F12 reloads
settings without resetting the wind, its gust phases, or the effect phases.

| Control / INI key | Default | Meaning |
| --- | --- | --- |
| Shared wind / `Enabled` | 1 | Connect the shared wind. Off restores legacy authored water/cloud motion and removes added rain drift; it does not disable the effects. |
| Time variation / `Dynamic` | 1 | Slowly choose new compass targets and vary strength over time. Off holds the selected direction and removes gust variation; weather can still change strength. |
| Base direction / `Direction` | 1 (NE) | Compass bearing the wind comes **from**: `0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW`. The slider displays names. In dynamic mode this is the starting bearing; editing it requests a smooth turn. |
| Base strength / `StrengthPercent` | 100 | 0–200%. 100% is a reference breeze; 0 remains calm even in a storm. Live strength is bounded to 300% after weather/gust response. |
| Gust variation / `GustPercent` | 35 | 0–100% bounded time variation about the weather-adjusted baseline. 0 removes gusts without stopping direction changes. |
| Gust period / `GustSeconds` | 18 | 2–120 seconds for the principal gust cycle; slower/faster components avoid a single repeating pulse. |
| Direction interval / `DirectionChangeSeconds` | 120 | 15–600 seconds between compass targets. Each choice is an adjacent or quarter turn. |
| Response time / `TransitionSeconds` | 30 | 1–120 seconds. Strength reaches about 95% of a fixed rising target in this time; clearing is 1.5× slower. Also limits heading changes to 45 degrees per interval of this duration. |
| Weather response / `WeatherResponsePercent` | 100 | 0–200%. Rain, snow and sand increase the target strength; 0 ignores weather. |
| Drive waves / `AffectWaves` | 1 | Apply the shared direction and strength to water. Off restores its original direction/native-weather response. |
| Drive clouds / `AffectClouds` | 1 | Apply the shared direction and strength to cloud shapes, detail, weather map and height shear. Off restores the cloud section's legacy wind controls. |
| Wind debug view / `DebugView` | 0 | Optional text HUD, including when F7 is closed. No world-space wind effect. |

**Direction is the compass bearing the wind comes FROM** (the meteorological
convention), not the direction it travels. WoW map axes are **+X north, +Y west**
(+Z up), so a north wind travels south and an east wind travels west. The eight
source bearings are smoothly interpolated rather than snapping in 45-degree
steps. The debug HUD reports the current source bearing, nearest sector, target
bearing, time to the next target, current/target strength, downwind XY velocity,
native weather input and the wave/cloud/rain link status.

For a steady test wind, set **Time variation OFF**, **Weather response 0**, select
a direction and strength, and allow the response to settle. At 0% strength clouds
stop wind-driven drift/lean; independent cloud evolution and weather-driven
coverage can continue. Water eases down to residual swell, with no wind-driven
foam (even if its sea-energy response is 0); rain still falls but
stops drifting. Wave speed 0 stops wave travel/turning (energy can still respond
to wind); Water's wave switch removes the waves entirely.

## Existing effect controls

- `[VolumetricClouds] WindSpeed` is cloud drift speed in world yards/second at
  shared strength 100% (still default 26). It multiplies shared strength, so
  `WindSpeed=0` stops wind advection. `WindShearPercent` is likewise a local gain.
  Optional `WindAngle` is ignored while linked and no longer shipped in the
  INI. When unlinked it retains its legacy default (25-degree **sampling**
  angle): sampling `p + offset` travels opposite the offset.
- `[Water] WindStrengthPercent` remains the body's foam/wind bias; 0 still
  suppresses wind-driven crest/shoal foam. It is not a second global wind.
- `[Water] WeatherResponsePercent` controls how strongly that body's **wave
  energy** reacts to departures of shared strength from 100%. At 0 its authored
  swell is retained; direction and foam/roughness still follow actual wind, so
  zero wind cannot produce whitecaps. When unlinked the energy response uses
  native weather directly. A storm is **not counted twice**.
- Zone water exposure, wave size/height, wave speed, geometry-wave switches,
  foam limits and the existing sea-state easing remain in effect. Waves have
  additional directional inertia; the HUD calls this out rather than claiming
  that a heavy sea turns instantaneously with the air.
- The new rain field projects the shared downwind velocity into the camera
  basis and adds gravity. `RainFallSpeedPercent` controls fall speed;
  `RainWindInfluencePercent` scales wind without introducing another bearing.
  F7 also exposes density, streak shape, opacity and mask/direction debug
  views. Motion is integrated, so edits do not teleport particles. Native
  rain draws are hidden only once the replacement can render. Old
  `[WeatherVisuals] WindAngle` / `WindStrengthPercent` settings are ignored.
- Cloud/water rendering switches and F10 do not stop the wind simulation. F11
  still gates the graphics package. Weather detection is independent of native
  particle replacement and texture availability. `WeatherVisuals.Enabled` and
  F10 affect **rendering**, not detection or shared wind/cloud/wave response.

Weather is the existing **held native observation**, including its F7/focus-gap
protection. Rain/snow/sand type is shared by the continuity layer, clouds,
wind and sea; the old `[WeatherVisuals] Mode` override is ignored because it
changed only the continuity layer. The native type and authored local intensity
are known, not a verified server-side severity or forecast. `[WeatherVisuals] IntensityPercent`
(the F7 storm-intensity slider) scales replacement particles **and** the shared
weather response, even when replacement rendering is off. The zone intensity
multiplier is applied once before the common [0,1] clamp used by clouds, wind
and unlinked water. At full local intensity and 100% weather response, target
strength multipliers are 1.8 for rain, 1.4 for snow and 2.1 for sand, before
gusts and the 300% ceiling. “Time variation” means elapsed
simulation time; no unverified in-game time-of-day memory is read.

## Integration contract

- `src/Environment/WindSystem.h` is portable, device-free simulation policy.
  `WindState` exposes a unit horizontal downwind `direction`, normalized `strength`,
  velocity in **world yards/second** (`12 × strength`), source-bearing target, weather and
  bounded frame delta. Every consumer reads this state and none creates its own
  random wind, clock or weather reader. Read only on the render thread or copy
  the snapshot before using it on another thread.
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
- `src/Effects/RainField.h` projects the same world velocity through the
  captured camera-right/up basis. Its double-precision rain motion integrates
  once per UI boundary; missing views keep gravity without inventing a heading.
  `PrecipitationMotion.h` retains the snow/sand continuity motion.
- GPU resets drop only GPU resources/obsolete camera anchors, not wind, sea,
  cloud or precipitation phase history. Invalid/negative/non-finite deltas
  advance nothing; long pauses advance at most 0.1 seconds. No catch-up storm
  after Alt+Tab. Configuration values and wind/sea/cloud shader outputs are
  bounded/finite.
- The wind debug HUD is isolated by the existing device **and tracked-state**
  transaction and has no input hook. Disabled debug performs no device work.

The existing cloud/water offline preview scenes remain **fixed, unlinked
reference scenes**, not simulations of a live dynamic wind history. The wind
regression exercises the actual shared wind, cloud policy, wave and
precipitation integrators. The wind link required no water/cloud shader ABI
change; the continuity weather shader now receives integrated displacement
instead of instantaneous wind multiplied by session time.

## Coupling audit (2026-10-15)

The Booty Bay water-performance pass re-traced clouds and water; this settings
audit also traced the precipitation continuity layer:

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
4. `WeatherVisuals::BeforeDraw` projects the same snapshot into camera right/up
   for the pre-UI gap-filling layer. Fall speed and projected wind are integrated
   rather than re-multiplied by elapsed session time. The old separate weather
   angle/strength settings no longer compete with shared wind.
5. `AffectClouds=0` and `AffectWaves=0` independently select each effect's legacy
   path. They do not mutate the shared snapshot, the other consumer or rain.

`tools/WindRegression.cpp` pins all eight world-axis travel signs for clouds,
water and projected precipitation, sea energy vs. wind-driven foam, phase
continuity, config uniqueness and production wiring. A live-client all-heading
check remains the visual acceptance test.

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
Windows CI; the portable wind test is wired into Linux CI. `tools/Test-Weather.cmd`
checks the changed precipitation shader phase on a D3D9 device. Water, cloud,
frame-clock, shader-source and overlay-layout regressions remain required.

In-client acceptance checks (require Windows and the Ascension client):

1. Use fixed wind with weather response 0. Select all eight source bearings,
   allowing wind and sea inertia to settle. On a north-up map, cloud and wave
   travel should point opposite the selected bearing; compare against the
   minimap compass, not camera-facing left/right. Turn the camera and visit
   distant coordinates.
2. Enable time variation. Confirm target changes, smooth actual heading and
   strength rising **and falling** in the HUD. Watch cloud shadows move with
   the deck and wave geometry stay aligned with its shading.
3. Observe native rain/snow/sand onset and clearing, with precipitation
   replacement both enabled and disabled. No duplicate storm amplification.
   Rain streaks should move downwind as the camera turns; native snow/sand
   particle motion remains the game's own.
4. Change wind speed, heading and rain fall speed after a long session. No
   cloud, wave or continuity-drop phase teleport. Test F7 close/open, F12,
   location overrides, loading and Alt+Tab/device reset; verify graphics
   state does not leak.
5. Disable each link and the wind master, then re-enable. Legacy effect
   controls remain usable; the shared simulation is not reset by F7 edits.
