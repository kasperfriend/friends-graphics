---
area: renderer
type: fixed
audience: players
title: First-launch cloud generation moves off the render thread; ground-fog capture stops saving state it never touches
---
Two hot-path costs were removed without changing any visuals:

- On a first launch (no cloud noise cache yet), the volumetric cloud
  fields take roughly a second of CPU to generate. That work used to run
  on the first frame that would draw clouds - under a loading screen on a
  slow machine, but in live gameplay on a fast one. It now starts on a
  worker thread while the game is still loading, and the render thread
  simply skips a frame and retries while it is finishing. The cloud
  cache file is written atomically (temp file + rename), and a bounded
  five-second fallback to the old inline generation keeps first-time
  cloud display guaranteed even if the worker is pathologically slow.
  Later launches read the cache as before; the generators are
  deterministic, so both paths produce identical fields.

- The terrain height field used by the ground fog (the ground surface
  capture) ran before every world terrain and water draw. It saved and
  restored the entire D3D device state (~200 driver calls per draw) even
  though it only changes the render target, four textures, both shaders,
  nine vertex-constant registers and sixteen render states. It now saves
  and restores exactly that, removing more than half of the per-draw
  driver overhead while capturing the identical height field.
