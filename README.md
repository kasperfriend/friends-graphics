# Modern WoW Renderer

Modern WoW Renderer is a 32-bit Direct3D 9 proxy (`d3d9.dll`) that adds modern lighting, atmosphere, water, weather, and post-processing effects to the World of Warcraft client used by [Project Ascension](https://ascension.gg/).

> [!IMPORTANT]
> This project is developed and tested for the **Ascension client**. It is **not tested with a stock World of Warcraft 3.3.5 client**. The renderer is under active development, so visual glitches, compatibility problems, crashes, and performance regressions may still occur.

## Highlights

- Volumetric atmosphere, clouds, fog, sun and moon rays.
- Dynamic local lighting and screen-space ambient occlusion.
- Wind-driven rain, clouds, and zone-aware water.
- Reflections, refraction, waves, foam, shore effects, and underwater rendering.
- Native-shadow enhancements and optional post-processing.
- An in-game `F7` menu with global, zone, and subarea presets.

See the [full feature guide](docs/features.md) for details, controls, and effect-specific documentation.

## Screenshots

<table>
  <tr>
    <td><img src="Screenshots/WoWScrnShot_092826_215004.jpg" alt="Rain, fog and local lighting" /></td>
    <td><img src="Screenshots/WoWScrnShot_092826_215807.jpg" alt="Volumetric fog around a lantern" /></td>
  </tr>
  <tr>
    <td><img src="Screenshots/WoWScrnShot_092826_220551.jpg" alt="Local fire lighting and fog" /></td>
    <td><img src="Screenshots/WoWScrnShot_092826_222851.jpg" alt="Rain and volumetric lighting in a forest" /></td>
  </tr>
  <tr>
    <td><img src="Screenshots/WoWScrnShot_092626_222609.jpg" alt="Water and atmospheric lighting" /></td>
    <td><img src="Screenshots/WoWScrnShot_092626_232041.jpg" alt="Water reflections at night" /></td>
  </tr>
  <tr>
    <td><img src="Screenshots/WoWScrnShot_092626_232124.jpg" alt="Water rendering and distant atmosphere" /></td>
    <td><img src="Screenshots/WoWScrnShot_092626_232814.jpg" alt="Rain in Stormwind" /></td>
  </tr>
  <tr>
    <td><img src="Screenshots/WoWScrnShot_092726_000028.jpg" alt="Night reflections in Stormwind" /></td>
    <td><img src="Screenshots/WoWScrnShot_092726_000456.jpg" alt="Coastal water and ship" /></td>
  </tr>
  <tr>
    <td><img src="Screenshots/WoWScrnShot_092726_000647.jpg" alt="Night sky and environment lighting" /></td>
    <td><img src="Screenshots/WoWScrnShot_092726_000749.jpg" alt="Red environment lighting" /></td>
  </tr>
</table>

## Effect previews

![Volumetric cloud climates](Screenshots/volumetric-clouds.png)

*Volumetric cloud climates rendered at the shipped defaults.*

![A river, an open sea at a low sun, a windward coast, and a moonlit lake](Screenshots/water-forever-look.png)

*Four water bodies rendered at the shipped defaults.*

## Quick start

1. Download the newest ZIP from [GitHub Releases](https://github.com/Corfirean/modern-wow-renderer/releases).
2. Extract it into the Ascension client directory, next to the game executable.
3. Keep the packaged `data`, `MaterialCache`, and `textures` directories beside `d3d9.dll`.
4. Start the client normally and press `F7` to configure the effects.

| Key | Action |
| --- | --- |
| `F7` | Open or close the settings menu. |
| `F9` | Capture a celestial diagnostic frame when diagnostics are enabled. |
| `F10` | Toggle weather visuals. |
| `F11` | Toggle all injected graphics effects. |
| `F12` | Reload settings without restarting the client. |

See the [user guide](docs/user-guide.md) for the complete installation layout, configuration scopes, diagnostics, and troubleshooting.

## Known issues

- **Water in Orgrimmar and Dalaran:** enhanced water is intentionally disabled in these capitals. Their city maps exist in isolated “void” spaces, so the renderer cannot produce convincing surrounding water there. Other places with similar layouts may need the same treatment.
- **Death Knight starting-zone clouds:** volumetric clouds are intentionally disabled because the zone does not appear to expose the depth needed to composite them reliably. After extensive investigation, no stable workaround was found.
- **Capital cities:** capitals such as Orgrimmar and Dalaran use special map layouts. Effects can therefore behave unpredictably there by design; the renderer guards the known cases, but visual glitches or instability may remain.

Additional compatibility notes and reporting instructions are in [Known issues and limitations](docs/user-guide.md#known-issues-and-limitations).

## Documentation

- [User guide](docs/user-guide.md) — installation, controls, configuration, troubleshooting, and bug reports.
- [Feature guide](docs/features.md) — effect details and links to focused technical guides.
- [Development guide](docs/development.md) — local builds, CI, validation, and repository structure.
- [Documentation index](docs/README.md) — all user guides, implementation notes, audits, and plans.
