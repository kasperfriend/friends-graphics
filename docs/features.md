# Features

## Atmosphere and volumetric lighting

- Sun and moon god rays are a whole-frame, depth-aware gather toward the tracked celestial source: sky shows as beams, while trees, characters and buildings cast visible shadows along them and clouds break them into gaps. The math lives in `src/Effects/GodRayCore.h`; `tools/GodRayPreview.cpp` renders a reference frame and `tools/GodRayRegression.cpp` checks it.
- Height fog, aerial haze, and ground mist are integrated in world space. The animated terrain-relative local fog layer is integrated the same way but ships switched off; `Enabled=1` under `[LocalFog]` in GraphicsEffects.ini turns it on.
- When enabled, local fog follows terrain, continues to evolve while the player is standing still, and reacts to player movement with a configurable wake.
- Temporal accumulation and edge-aware upsampling reduce noise without drawing over the interface.

## Volumetric clouds

- A raymarched cloud deck replaces the flat sky: Perlin-Worley shapes placed by a weather map, lit by the tracked sun or moon, self-shadowed, and drifting with the wind.
- Eight cloud climates across 88 verified zones: fair-weather patches, sparse desert skies, coastal sheets, cold decks, mountain towers, lush billows, dark overcast and open alien skies. Unknown zones fall back safely.
- Native rain, snow and sandstorms gradually close the sky; clearing opens it again. Cloud bodies and edges evolve over time, even without wind, while shadows and rays follow the same field.
- The deck is a spherical shell, so it meets the horizon with a curve and can be flown into and above instead of being a slab pushed out of the camera's way.
- Clouds are clipped by the scene rather than drawn only on sky pixels, so the layer is still there when you fly over it and a peak inside the deck is fogged by the cloud it stands in.
- Distance hazes clouds out, the deck casts moving shadows on the world, and the layer is integrated into the scene before the atmosphere, so haze, the sun halo and god rays react to the clouds above them.
- Coverage thickens automatically while native rain or snow is falling, and the slider runs from scattered puffs to a solid overcast lid.
- Cloud-filtered sunlight forms rays through moving gaps; world-anchored ground shadows no longer change with camera height or off-screen sun tracking.
- Coverage, altitude, thickness, drift gain, haze, lighting, cloud-ray strength, independent resolution (20–100%) and a four-step **march** quality level are on F7 and in `[VolumetricClouds]`. Changing quality now actually changes steps without overriding resolution. See the [volumetric cloud guide](volumetric-clouds.md).

![Volumetric clouds](../Screenshots/volumetric-clouds.png)

*Scattered, backlit broken, overcast and the deck from above - rendered by `tools/CloudPreview.cpp`, the CPU mirror
of the cloud shader, at the shipped defaults.*

## Dynamic environment lighting

- Trusted native point and spot lights can illuminate nearby world surfaces.
- Strong local lights contribute to volumetric scattering in fog and mist.
- Held torches and lanterns retain their light while the player briefly occludes the source from the camera.
- Screen-space occlusion and depth-derived normals keep the effect attached to visible geometry.

## Ambient occlusion

- Screen-space ambient occlusion from the captured scene depth seats objects on the ground and deepens creases, corners and cavities.
- It is the first effect applied to the scene, so it darkens world geometry only: clouds, local light, fog, glare and the interface all composite over the result untouched, and sky and water are excluded outright.
- Tuned to be felt rather than seen: at the shipped radius every open surface - floor, wall face, rooftop - keeps its brightness exactly, while a right-angled corner still darkens by a quarter. Raising `AO RADIUS` reaches further into architecture and starts darkening whole surfaces; `AO STRENGTH` alone never does.
- A depth-aware blur keeps occlusion from smearing across an object edge onto the background behind it, and the effect retires with distance instead of sparkling.
- **F7 → AMBIENT OCCLUSION** holds the switch, strength, radius, distance fade and a debug view. See [how it works and why the defaults are what they are](ambient-occlusion.md).

![Ambient occlusion off and on](../Screenshots/ambient-occlusion.png)

*Off, on at the shipped defaults, on at twice the shipped strength, and the raw occlusion term - rendered by
`tools/AmbientOcclusionPreview.cpp`, which runs the same math the GPU runs over the same depth buffer.*

## Weather

- Rain uses a depth-aware field of layered, resolution-aware streaks: longer sparse foreground drops, finer distant rain, tapered highlights and scene-adaptive contrast. Gravity pulls them down, shared world wind moves them downwind, and camera movement gives the layers smoothly eased parallax without changing the wind direction. Native rain is hidden once the new pass is ready. F7 offers speed, density, length, width, opacity, wind influence, camera parallax and mask/direction debug views.
- Snow and sand retain native particles plus a small continuity layer; their native direction remains with the game.
- Weather state survives intermittent native particle batches, missing UI draws, opening F7 and returning from Alt+Tab. Clouds, wind, waves and the continuity layer all use the detected native rain/snow/sand type; the old visual-only weather hint is ignored.
- Optional rain lens droplets and subtle snow mist are available; this setting does not also retune the volumetric atmosphere.

## Shared wind

- One invisible, world-space wind drives wave direction/sea strength and cloud drift/shear; the optional camera-relative precipitation continuity layer projects that same wind onto the screen.
- Eight meteorological compass bearings (the direction wind comes from), smooth turns, time-varying gusts and native-weather response. Strength rises during rain, snow and sandstorms and eases as weather clears.
- **F7 → WIND** controls direction, strength, gusts, timing, weather response and the wave/cloud links, with an optional diagnostic HUD. It does not alter the game's native particle motion.
- Motion phases survive tuning reloads and device resets. The same state drives waves, clouds and the precipitation continuity layer.
- Nothing in the world sways with it: grass, plants, bushes and trees are drawn exactly as the client drew them. Wind here is weather, not vegetation.
- See [wind controls, conventions and validation](wind-system.md).

## Water

See [water rendering, verification and troubleshooting](water-rendering.md), and
[the plan for the next water pass](water-forever-round2-plan.md) for what the shading
model is being moved to and which parts of it a `ps_3_0` shader can and cannot carry.

- Screen-space reflections against the pre-water scene copy, with a horizon-to-zenith sky and
  the sun or moon mirrored in it as the fallback.
- Refraction that bends the bottom further the deeper the column is, per-channel depth
  absorption with light scattering back out of the water, shoreline/crest/shoal foam, ripples,
  Schlick Fresnel response, and a sun glint that broadens as the source drops toward the horizon.
- The surface keeps the light a disc confirmed while that disc is off screen, so pitching the
  camera up above the horizon no longer takes the sun road, the mirrored disc and most of the
  sea's brightness with it in a single frame. The direction travels in world space and is
  expressed per draw, which also lights the client's own mirrored reflection pass in the camera
  it is rendered from.
- The long swell is analytic and shared with the vertex stage, so a displaced crest and the
  light on it are the same wave; only the chop fades when a pixel cannot resolve it. A mid-wave
  band between the swell and the chop carries the texture a player reads as "waves", and above
  WATER QUALITY 8 the mesh lifts it too.
- `[Water] Quality` (F7: WATER QUALITY, 0-10) is the one water setting that is a cost rather than
  a look: it trades the chop octaves, both ray marches, the mirrored emitters, the displaced mid
  wave, the foam lace and the shore back flow at once. 0 is a flat sheet, 6 is the sea that shipped
  before the slider existed, 7 is the default, 10 is everything - and no level changes what any
  other water slider means.
- Shorelines are read off the captured depth buffer as a bed slope in world yards, so the same
  shore is the same shore at any resolution and distance: a beach gets a wide swash zone with
  sheets running up it and draining back out, a rock wall gets a narrow bright contact line.
- Foam is a lace of bright filaments over dark water-filled gaps at three scales, coloured by its
  own coverage rather than painted one flat white, and it glows when a low sun is behind it.
- Waves are evaluated on a world-anchored lattice, not on a tile's texture coordinates, so
  one continuous surface is never framed by the squares of a repeated or degenerate pattern.
- Texture-signature matching covers liquid shader variants without splitting a continuous water surface.
- Per-zone sea state and live weather response: an exposed coast carries more water than a
  sheltered pond, and stronger shared wind roughens and quickens both without counting weather twice.
  Sea strength eases over `[Water] WeatherEaseSeconds`; unlinked water retains its legacy weather response. Turning sea-energy response to zero retains authored swell but does not invent wind-driven foam in calm air.
- Explicit sea states cover all 414 roots in the verified Ascension patch-M tables. This is a
  custom WotLK 3.3.5-based dataset, including Ascension and expansion-derived map IDs, not the
  stock Wrath zone catalog; the [full zone audit](water-zone-profile-audit.md) documents scope.

![Water: a river, an open sea at a low sun, a windward coast and a moonlit lake](../Screenshots/water-forever-look.png)

*The four bodies the look was tuned against - rendered by `tools/WaterPreview.cpp`, the CPU mirror of the water
shader, at the shipped defaults.*

![The same sea four times: a sheltered zone and an exposed coast, each in calm air and in a rainstorm](../Screenshots/water-zone-weather.png)

*One sea, four sea states - `build/waterpreview --matrix --scene sea --grid 2 --supersample 2`. The columns are the
sky, the rows are what the zone asks for, and every panel shares a clock and a camera, so the difference is the model.*

## Native shadows and post-processing

- Softness and strength controls enhance the client-created cascaded shadow maps. The receivers are recognised from the shader's own bytecode (it samples the four cascade slots, and the constants it adds to a UV are its kernel), so a client update that recompiles its material shaders does not silently turn the sliders off.
- Optional brightness, contrast, gamma, and sharpness post-processing.
- Native distance-fog controls remain available for compatibility with the client's own fog path.

For installation, controls, configuration, and troubleshooting, see the [user guide](user-guide.md).
