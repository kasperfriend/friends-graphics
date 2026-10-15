---
area: renderer
type: fixed
audience: developers
title: Per-draw and per-frame hot path cleanup
---
Three renderer hot paths no longer pay for work that cannot change their outcome:

- The native shadow softness/strength scope checked the verified receiver
  shader whitelist in memory before crossing the D3D9 driver. Every
  back-buffer draw that is not a shadow receiver previously paid a
  GetPixelShader check, a four-cascade texture probe (several
  GetTexture/GetLevelDesc/Release calls) and a render-state query; it now
  costs a dozen comparisons. The whitelist stays the authoritative safety
  gate, and discovery of new receiver variants survives the pre-gate: each
  unknown pixel shader is still probed once per session and logged when the
  native cascades are bound, instead of probing on every draw.
- Native precipitation detection no longer locks and hashes the entire
  bound texture on every alpha-blended depth-tested draw. Large UI and
  spell atlases hit that stage constantly, and the first sight of a pointer
  cost a multi-millisecond LockRect/hash stall. The texture is hashed only
  when every other match condition already holds (which bounds it to the
  tiny precipitation textures) or when the bounded candidate log still has
  particle-sized entries to identify.
- All ten hotkeys (F6-F12) read one foreground-window sample taken at the
  top of Present, stored in the per-frame context, instead of each calling
  GetForegroundWindow/GetWindowThreadProcessId. A focus change landing
  mid-frame could previously fire some toggles and swallow others, and the
  F7 overlay now measures the same window the focus decision was made
  from.

No visible output changes: the shadow scope only runs the same verified
whitelisted receivers, weather detection matches the identical exact
texture/shader pairings, and the hotkeys keep their exact key and focus
gates. The offline regression harness (LocationTuning cache, wind, water,
vegetation) stays green; the non-Windows windows.h fixture gains the time
and foreground-window APIs the diagnostic headers use so the environment
regression can move to Linux CI next.
