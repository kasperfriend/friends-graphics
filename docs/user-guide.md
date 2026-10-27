# User guide

Modern WoW Renderer is developed and tested for the **Project Ascension client**. It is not currently tested with a stock World of Warcraft 3.3.5 client.

## Installation

1. Download the newest ZIP from [GitHub Releases](https://github.com/Corfirean/modern-wow-renderer/releases).
2. Extract its contents into the Ascension client directory, next to the game executable.
3. Confirm that `d3d9.dll`, `ModernWoWRenderer.ini`, `GraphicsEffects.ini`, `EnvironmentProfiles.ini`, and `LocationGraphics.ini` are in that directory. Keep the packaged `textures`, `MaterialCache`, and `data` directories beside it; `data/areas.txt` is what lets the renderer recognise the zone you are standing in.
4. Start the client normally. Use `F7` to tune the effects and `F12` after editing `GraphicsEffects.ini` or `LocationGraphics.ini` by hand.

Example layout:

```text
Ascension/
├── d3d9.dll
├── ModernWoWRenderer.ini
├── GraphicsEffects.ini
├── EnvironmentProfiles.ini
├── LocationGraphics.ini
├── data/
│   └── areas.txt
├── MaterialCache/
│   ├── local_light_manifest.json
│   └── LocalLightReview.json
└── textures/
    └── weather/
```

Remove or rename `d3d9.dll` to disable the proxy completely.

## Controls

| Key | Action |
| --- | --- |
| `F7` | Open or close the tabbed in-game tuning menu. |
| `F9` | Capture one diagnostic frame when celestial diagnostics are enabled. |
| `F10` | Toggle weather visuals. |
| `F11` | Toggle all injected graphics effects. |
| `F12` | Reload `GraphicsEffects.ini` without restarting the client. |

### Diagnostic captures

`F8` captures one diagnostic frame when `[Diagnostics] Enabled=1` in
`ModernWoWRenderer.ini`. F12 reloads that section; `AutoCapture=1` captures a
frame after reload without F8. Capture status and file errors appear in
`WaterEffect.log`. This is troubleshooting only.

### F7 settings menu

F7 uses a left-hand category list: select a tab to open its related settings. Drag the title bar to move the window, or drag the bottom-right corner to resize it. Longer tabs scroll with the mouse wheel over the settings pane, the scrollbar, or the up/down buttons. The window stays on screen; its size, position, selected tab and per-tab scroll positions are retained while the client is running. Changes save automatically.

### Location scopes

F7 saves atmospheric, lighting, water and weather settings separately for each location. The `Settings for` row has three scopes: `Entire zone`, `This subarea`, and `Global - entire game`. In verified locations, a Global value normally applies where a setting opts in through `Inherit Global`; otherwise the zone or area keeps its own value. Simply opening Global does not change anything: only settings you edit become overrides. Global shows the shared baseline for untouched settings, while zone/subarea editors normally show their own values.

The `ALL TO GLOBAL` switch temporarily masks every Map/Zone/Area override and uses Global values or the shared `GraphicsEffects.ini` baseline. It does not rewrite or delete location values; turn it off to restore them. While the switch is on, editing any scope writes to Global. Global overrides are saved as `[Global.<effect section>]` in `LocationGraphics.ini`. In Global, `CLEAR OVERRIDES` removes overrides for the selected category so saved location settings apply again; other categories and location presets are preserved. `RESET DEFAULTS` instead saves explicit defaults for every setting in the selected category. Global remains available without a verified location; zone and subarea selection require verification. The `IMAGE & COLOR (GLOBAL)` category remains shared through `GraphicsEffects.ini` in every scope, including the post-process toggle; old local color overrides are ignored.

The F7 Atmosphere, Volumetric Clouds and Water tabs include manual RGB, alpha and brightness controls. Alpha 0 preserves the inherited zone colour; raising it blends in your chosen colour. These controls can override the water, fog and cloud palettes supplied by the current zone.

### Location detection and troubleshooting

Location presets automatically recognize the verified Ascension executable SHA-256 `e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6`, stock 3.3.5a build 12340, and the supplied ArdenWoW SHA-256 `ed62bd406dbe92cabe4b38759c630df25a8d3b02a2646e1b77f62cb7be273c1d` when `[LocationProvider] ExeSHA256` is blank. Arden uses the shared 3.3.5a location RVA preset. Releases include `data/areas.txt`, exported from Ascension's loaded tables; custom Arden area IDs still need a matching database. Keep `data` next to the DLL. Other executable versions need [environment setup and validation](ENVIRONMENT-SYSTEM-RU.md). Unknown clients keep local presets locked; global image/color remains editable. Existing explicit client configurations are respected.

If the header says `Location unavailable`, the reason follows it on the same line: `not configured` means `EnvironmentProfiles.ini` is not next to `d3d9.dll`, `unrecognized executable` means your client build has no verified location addresses yet (its SHA-256 is recorded in `ModernWoWRenderer.log`), `ids fail area database` means `data\areas.txt` is missing or was not exported from the client you are running, and `no world frame yet` clears itself once the world is drawn. `F12` retries after you fix a file.

If F7 is locked, unresponsive, or fails to save, open F7 and reproduce the problem, then send `MenuDiagnostics.log` from the client directory with a screenshot. It records the build, executable hash, location validation, database status, viewport/client sizes, and save/render failures. It rotates at 1 MiB to `MenuDiagnostics.log.previous`. If no log exists, include `ModernWoWRenderer.log`. F12 reloads both settings and the area database; installing a new DLL requires restarting the game.

## Configuration

`ModernWoWRenderer.ini` contains proxy-wide switches, depth-capture preferences, and diagnostic controls. Most users should leave it unchanged. The dead flat `[Atmosphere]` section was removed: live fog, shafts and clouds are controlled by `GraphicsEffects.ini`, while `[Volume] Enabled=0` still intentionally disables the whole volumetric backend.

`GraphicsEffects.ini` contains the live visual settings:

| Section | Purpose |
| --- | --- |
| `[Atmosphere]` | Volumetric quality, fog density and colour, shafts, temporal filtering, and debug views. |
| `[LocalFog]` | Terrain-relative fog height, density, flow, and player wake. Ships off; `Enabled=1` turns it on. |
| `[DynamicLighting]` | Direct local-light pools, volumetric-scattering scale, quality, smoothing and debug views. Direct pools need `[Volume] Enabled=1` but no other effect; volumetric scattering uses the active atmosphere/local-fog medium. |
| `[WeatherVisuals]` | Native-weather intensity (also used by wind/cloud/sea response), rain particle speed, density, length, width, opacity, wind influence and debug views, plus occlusion and lens droplets; rain follows `[Wind]`. Rendering off/F10 does not disable weather detection. |
| `[Wind]` | Shared eight-direction wind, strength/gust timing, weather response, wave/cloud links, continuity-layer drift and diagnostic HUD. |
| `[VolumetricClouds]` | Cloud shape, coverage, drift gain, manual colour, lighting, shadows, rays and quality. |
| `[Water]` | Manual colour, reflections, refraction, ripples, foam, absorption, Fresnel, glint, and how hard the zone and the weather push on all of it. |
| `[NativeShadows]` | Enhancement of the client's native shadow softness and strength. |
| `[PostProcess]` | Global enable switch, brightness, contrast, gamma, and sharpness. Always saved in `GraphicsEffects.ini`; location overrides are ignored. |
| `[DistanceFog]` | One F7 enable/distance/power control for world-edge fog in the volume compositor and its native-shader fallback; neutral reset is 100/100. |

The F7 overlay writes supported values back to `GraphicsEffects.ini`. Manual edits can be applied with `F12`.

`LocationGraphics.ini` holds the per-location overrides, and ships empty: every example in it is commented out, so it changes nothing until you fill a value in. A setting is read from the most specific scope that holds it - the subarea, then the zone, then the map, and finally the `GraphicsEffects.ini` baseline. The whole-game `[Global.<Section>]` scope applies where a location opts in through its `[Map.<map>.Zone.<zone>.InheritGlobal]` section, unless F7's `ALL TO GLOBAL` switch temporarily masks location scopes. The switch is persisted as `[Overlay] AllToGlobal=1`; turning it off restores all saved location values. The file documents the scope names, the sections you may use and how to find the current Map/Zone/Area IDs. F7 saves the overlay's own changes into this same file, and it rewrites the section it writes, so keep notes above the first section header.

`EnvironmentProfiles.ini` holds the authored environment table: `[Default]`, the named climates in `[Environment.<Name>]`, and the per-place multipliers in `[Map.<id>]`, `[Zone.<id>]` and `[Area.<id>]`, resolved as `Default` then `Map` then `Zone` then `Area`. It also carries the `[LocationProvider]` block that tells the renderer which client build it may read location ids from, and without a verified client and `data/areas.txt` the renderer cannot tell which zone you are in - so none of the zone settings apply.

Water uses the live `[Water] Enabled` setting above, including location overrides.
An old water `Enabled` entry in `ModernWoWRenderer.ini` is ignored; it no longer
prevents F7 from enabling water. F11 still controls the whole graphics package.

## Known issues and limitations

- **Water in Orgrimmar and Dalaran:** enhanced water is intentionally disabled in these capitals. Their city maps exist in isolated “void” spaces, so the renderer cannot produce convincing surrounding water there. Other places with similar layouts may need the same treatment.
- **Death Knight starting-zone clouds:** volumetric clouds are intentionally disabled because the zone does not appear to expose the depth needed to composite them reliably. After extensive investigation, no stable workaround was found.
- **Capital cities:** capitals such as Orgrimmar and Dalaran use special map layouts. Effects can therefore behave unpredictably there by design; the renderer guards the known cases, but visual glitches or instability may remain.

### General limitations

- Native-shadow enhancement only works when the Ascension client creates its own shadow maps.
- Ambient occlusion only sees what is on screen: off-screen geometry cannot occlude, contact shading is weaker than it should be where a normal is reconstructed across a crease, and occluders thinner than about a yard can be stepped over. Run it inside the atmosphere composite, so it needs `[Volume] Enabled=1`.
- Dynamic lights and weather detection rely on observed Ascension render patterns; unusual zones, spells, addons, or future client updates can expose missed cases.
- D3D9 device resets, Alt+Tab transitions, uncommon depth formats, and third-party overlays remain compatibility-sensitive. Device-loss handling pauses the effects while the device cannot draw and resumes when it is restored, but the exclusive-fullscreen and window-switch paths have not yet been verified in the Ascension client.
- This mod is in active development. Bugs are expected; include `ModernWoWRenderer.log`, the zone, and reproduction steps when reporting one.

## Reporting bugs

Open a GitHub issue with:

- the Ascension client build and zone;
- the effect and settings involved;
- exact reproduction steps, including whether Alt+Tab, F7, a mount, or a device reset is involved;
- `ModernWoWRenderer.log` and a screenshot or short video when available.

Reports from stock 3.3.5 clients are still useful, but that client is currently outside the tested compatibility target.
