# F7 menu audit

A full review of the F7 tuning menu: `TuningOverlay.h` (input, drawing),
`src/Environment/OverlayItems.h` (the 107 controls), `OverlayLayout.h` (geometry and
gestures), `OverlayFont.h`, `MenuDiagnostics.h`, `LocationTuning.h` (where a value is
written) and the call sites in `ModernWoWRenderer.cpp`. Every finding below was checked
against the code that consumes the same setting, so a control's range, default and target
file are read from both sides of the INI.

## Fixed in this pass

| Issue | Why it was wrong | Fix |
| --- | --- | --- |
| `GLINT STRENGTH` could be dragged to 500 | `WaterEffect.h` clamps `SunGlintPercent` to `0..400`, so the last fifth of the slider did nothing: the value was displayed, saved, and had no effect | `src/Environment/OverlayItems.h` bound is now 400, with the reason in a comment |
| `tools/Test-Overlay.cmd` could not build or run | `tools/OverlayRegression.cpp` still used the deleted three-column API (`PanelX`, `PanelY`, `panelHeight`, `columnWidth`, `ItemColumn`, `ColumnStarts`, `controlRow`, `ControlsY`, and `HitItem` as a free function). The device-level overlay test has been dead since the tab rewrite, while `docs/ENVIRONMENT-SYSTEM-RU.md` still lists it as part of verification | The hit-test sweep now walks tabs and scroll offsets like the portable test, adds a HAL skip (exit 77), and is wired into the Windows job of the Build workflow; the `.cmd` uses `vswhere` instead of one machine's Insiders path |
| `docs/f7-overlay-testing.md` said "nine tabs / 69 controls" | The menu has ten tabs and the control count in `OverlayItems.h` (`OverlayStateRegression` prints "all ten tabs") | Text updated, and the device test documented |

## Open findings

### 1. FOG DENSITY shows 5 while the legacy atmosphere path uses 4 (low)

`OverlayItems.h:19` ships `FOG DENSITY` = 5 and `DirectionalVolumetricLighting.cpp:550`
reads `DensityPermille` with a default of 5, but the legacy path in
`VolumeIntegration.h:287` uses `4`. The shipped `GraphicsEffects.ini` has no
`DensityPermille` key (only `HeightDensityPermille`), so both fallbacks are live at once:
on a frame that still goes through the legacy volume path, the fog density is 4 permille
while the menu shows 5, and only touching the slider makes them agree. One default should
move so the two paths and the menu cannot disagree.

### 2. The menu hit-tests against the D3D focus window, not the device window (low)

`Draw` captures `gameWindow = creation.hFocusWindow` (`TuningOverlay.h:252`) and every
mouse path then uses it: `GetCursorPos`/`ScreenToClient`, `GetClientRect`, the `WNDPROC`
hook, and the DPI query. D3D9 fills in both `hFocusWindow` and `hDeviceWindow`; it is
`hDeviceWindow` that owns the client area the menu is drawn over, and that is the handle
the mouse coordinates have to be translated against. (`Present`'s destination-window
argument is not a substitute: it is null whenever the game lets the runtime use the device
window.) A client that names a separate focus window
(a hidden message window is a common pattern) would get a panel whose hit tests and cursor
mapping are offset. The Ascension client appears to pass one window for both, which is why
this has not been reported. Suggested fix: prefer `creation.hDeviceWindow`, fall back to
`hFocusWindow`, and verify the process id matches before installing the hook.

### 3. Every slider step rewrites the INI and reloads every subsystem (low, unproven)

`Update` returns "changed" as soon as one value differs, and `ModernWoWRenderer.cpp:227`
reacts by re-reading the INI into every subsystem:

```cpp
if(tuningoverlay::Update()||locationTuningChanged){ReloadWindTuning();...weathervisuals::ReloadTuning();}
```

During a drag the value can change on every frame, so a fast drag performs up to ~60
`WritePrivateProfileStringW` calls (each rewriting the whole file), ~10 subsystem reloads,
and two appended log lines per step: `MaterialCacheManager::ReloadTuning`
(`src/Materials/MaterialCacheManager.cpp:107`) and `WaterEffect::ReloadTuning`
(`WaterEffect.h:880`) both open a file and write on every call. This is not proven to be
visible in game, but it is the most likely source of the menu feeling heavy while a slider
is dragged. Suggested fix: coalesce saves and reloads (for example save on release or at a
~100 ms interval), or let `Save` report "unchanged" so only real edits reload.

**Follow-up (2026-10-15, the location half).** The same chain has a second trigger, and this
one was proven in the field: `locationTuningChanged` fired on every alternation of the place
across a boundary. A real session's log holds `Location changed: Map=1 Zone=215 Area=0` and
`Area=223` alternating for as long as the player walked the edge, each alternation running all
nine `Reload*Tuning()` calls plus `tuningoverlay::Configure` (one `ReadEditorInt` per menu row)
on the spot - several full re-reads inside one second, which is the "big lag for a second" a
crossing was reported as. Three changes: the change is applied once the place has held still for
300 ms and at most once per second (`kLocationSettleMs`/`kLocationReloadGapMs` in
`HookedPresent`); a scoped read answers out of the identity-checked section cache
(`ProfileSection`, `ScopedValuesFor` in `LocationTuning.h`) instead of probing each level with
its own `GetPrivateProfileStringW` per key; and a place that resolves to the values already in
force is skipped outright (`AppliedTuningStillInForce`), so a border walked back and forth
costs a hash rather than nine reloads. The key-set memo the first version of this used had a
ceiling of 96 scopes, and one apply - nine tuning reloads plus the F7 menu's hundred-odd rows,
at every level of every section - walks more than that: the ceiling was hit in the middle of
the apply, the whole memo was dropped, and every read after it paid a whole-file parse, one per
scope per level. That is the 46-78 ms the reload line showed for a crossing. A hand edit to
`LocationGraphics.ini` is picked up by F12, which drops the cache; the global
`GraphicsEffects.ini` read is unchanged, so ordinary edits are still visible immediately.

**Follow-up (2026-10-15, the stall half).** The reload line now carries what the reads behind
it actually did - `answered <R> read(s) from memory with <P> whole-file parse(s)`, with
`<P>` the number that decides whether a crossing costs anything - and a frame that stalls is
written to the log by itself: `frame stall: <N> ms between presents (session average ...) -
<place>, the place changed <N> ms ago, the frame that took this gap drew <D> draw(s) against
<A> a frame for the session, and the last tuning reload cost <N> ms`. The gap is
measured on the performance counter before any of this project's work runs for the frame, so
the line separates the two possible owners of a hitch: the client loading the place it just
walked into, and this module's own work on the frame that noticed. A 2 s gap next to a 63 us
reload is the client's loader, and the line says so instead of leaving it to an argument. The
draw load is the second half of that answer: `<D>` is the difference between two reads of the
session's own draw count (the one number every draw hook feeds), so a frame that drew the
crossed border again - new terrain, new WMOs, new doodads, every one of them through these
hooks - comes out as a multiple of `<A>` instead of as a mystery.

### 4. Wheel scrolling also reacts outside the settings pane (very low)

The comment says a wheel is consumed "only over the settings pane", but the condition is
`px>=layout.Content().x && layout.panel.Contains(px,py)` (`TuningOverlay.h:178`), which
also matches the footer strip and the scope row above the pane. Harmless today (the scroll
is clamped), but a wheel over the footer scrolls the list. Use `Content().Contains(px,py)`
extended to the scrollbar strip if the comment is the intent.

### 5. Smaller items

* `RESET DEFAULTS`, the `X` close button and the two scroll buttons have no hover state;
  rows and tabs do (`hover` only ever holds a row index, `TuningOverlay.h:206`).
* `HitScope(px,py)` is evaluated twice for one press (`TuningOverlay.h:190`). No side
  effects, so this is cosmetic.
* If the GDI font atlas cannot be created, the bitmap fallback only has glyphs for
  `A-Z 0-9 ^ - / &`; punctuation in messages such as
  `Save failed (Windows 2). Send MenuDiagnostics.log` disappears. Debug-only path.
* `TuningOverlay.h` puts `#include "src/Environment/LocationTuning.h"` before
  `#pragma once`; move it below so the header reads like the rest of the tree.
* `Screenshots/f7-location-settings.png` (used by the README table) still shows the old
  three-column menu, and `docs/ENVIRONMENT-SYSTEM-RU.md` still states that F7 "has three
  columns without pages or scrolling". Both predate the tab rewrite and should be
  re-captured/updated.

## Verified clean

Checked in the same pass, no defect found:

* Every one of the 107 controls is reachable by mouse at every tab, every scroll offset and
  twelve resolution/DPI combinations (portable test, ASan/UBSan clean); each widget stays
  inside its own row, and the panel fits both the logical screen and the real backbuffer.
  The same sweep now runs against the real overlay on a D3D9 device in the Windows CI job
  and passes at 1920x1080, 2560x1440 and 3840x2160.
* **Text does not degrade with UI scale.** The layout is in logical pixels and the font is
  `round(16*uiScale)` device pixels, while every drawn bound is also multiplied by
  `uiScale`, so the space available in em is `logical_width/16` at every scale: the value
  column holds 3.0-3.2 em against the ~2.75 em of the widest five-digit value (CLOUD
  HEIGHT/SIZE/THICKNESS), the tab buttons hold 9-12 em against the longest label's ~10.9 em,
  and the slider label column holds 17-28 em against the longest label's ~12 em. The figures
  use Segoe UI advance ratios (~0.55 em digits, ~0.62 em capitals); the device test's PPM
  frames are what would confirm the exact rasterization on Windows.
* No dead controls: all 106 keys are read by the renderer, and every control's section
  exists in the shipped `GraphicsEffects.ini` (the three keys absent from the file -
  `ShaftsEnabled`, `FogEnabled`, `UseEnvironmentFog` - are read with the same fallback the
  menu displays).
* Every remaining slider range matches the clamp on the read line that consumes it, and
  every toggle is read with a `!=0` test so writing 1/0 is correct. The defaults shown
  match the reader defaults except `DensityPermille` (finding 1).
* No state leaks: `ScopedStateBlock` captures `D3DSBT_ALL` around both the menu and the
  wind HUD, and the device-level regression compares the captured block, the tracked mirror
  and the real device state after 180 frames of open/closed/reopened menu.
* Save semantics: a verified location writes the per-area/per-zone scope in
  `LocationGraphics.ini`, an unverified client falls back to the global section instead of
  snapping back, `IMAGE & COLOR` is always global, and a failed write reports Windows'
  error, keeps the previous value visible and logs to `MenuDiagnostics.log`.
* Drag/gesture handling: title move, corner resize, thumb/track scroll, slider drag,
  release outside the panel, Alt+Tab (`WM_KILLFOCUS`/`WM_CANCELMODE`), resolution change,
  F7 pressed during a drag, and opening while the button is already held.
* The menu swallows mouse messages aimed at the game only while it is visible, and restores
  the original window procedure when it closes.
* No crash paths in the item/slider/glyph indexing: row iteration is clamped to the tab,
  `dragItem` is only ever a `HitItem` result, and the glyph table indices are bounded.

## Follow-up (2026-10-16, the sway is gone)

The wind-driven vegetation sway was removed from the renderer outright, on the
tree `origin/main` carried once the sway PR had been merged there: the
`[Vegetation]` section of `GraphicsEffects.ini`, the `[Wind] AffectVegetation`
link, the eleven menu rows that belonged to it (`VEGETATION SWAY`, `SWAY
STRENGTH`, `SWAY GUSTS`, `GRASS FLUTTER`, `MAX LEAN`, `GRASS & BUSH GAIN`, `TREE
GAIN`, `SWAY DISTANCE`, `SWAY ANIMATED MODELS`, `VEGETATION DEBUG VIEW` and
`DRIVE PLANTS & TREES`), the HUD that reported it (`Plants:` in the wind row and
the whole `VegetationDebugLine` block, which on this branch had grown the
`Plants gate:`, `Blades:` and `Blades gate:` rows) and the code behind all of it
- the sway and blade-wind headers, the vertex-shader constant hook, the tests,
the tools, the preview screenshot and the documentation. The menu is 107 controls
on the same tabs; the wind's own rows and `WIND DEBUG VIEW` are untouched,
because the wind still drives the waves, the clouds and the precipitation drift.
No world geometry moves with it any more.

## Follow-up (2026-10-16, global illumination is gone)

The screen-space bounce pass was removed from the renderer outright: the
`[GlobalIllumination]` section of `GraphicsEffects.ini` (eleven keys), the tab
and the eleven menu rows that belonged to it (`GI BOUNCE & AMBIENT`, `GI
INTENSITY`, `GI SKY LIGHT`, `GI BOUNCE`, `GI SHADOW POCKETS`, `GI LAMP BLEED`,
`GI RADIUS`, `GI CLOUD SHADOWS`, `GI STORM DIMMING`, `GI QUALITY` and `GI
DEBUG`) and the code behind all of it - `src/Effects/GlobalIllumination.{h,cpp}`,
its two embedded shader strings and their `tools/ValidateShaders.ps1` entries and
instruction budget, the `PerfStage::GlobalIllumination` /
`GpuPerfStage::GlobalIllumination` profile rows, the `GlobalIllumination.log`
sink, the project file entries, the
`tools/LocationTuningCrossingRegression.cpp` section list and the documentation.
`VolumetricClouds::OvercastFactor()` went with it, as the one cloud accessor only
this pass read. The menu is 96 controls on eleven tabs, and
`tools/OverlayLayoutRegression.cpp` pins that tab count; the sweep below it still
reaches every remaining control. The cloud shadow map, the dynamic light
selection, the location `LightingMultiplier` and the atmosphere are untouched,
because each of them serves other passes as well. Nothing in the world picks up
bounced sky or lamp light any more.
