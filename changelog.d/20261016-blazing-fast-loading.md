---
area: renderer
type: fixed
audience: players
title: Blazing-fast load: client hash, AI-material manifests and water shaders no longer block startup
---
The startup path paid three costs that either could not change their
outcome or belonged to a feature that is off by default:

- The location provider hashed the ENTIRE WoW.exe (hundreds of MB of BCrypt
  SHA256) on every launch, even though the common shipped configuration
  identifies a stock 3.3.5a client with three small code-signature reads
  and a blank fingerprint has nothing to compare a hash against. The hash
  is now computed only when the code signature cannot answer, and its
  result is cached in a small `ClientSha256.cache` file keyed by the
  module path plus PE timestamp and image size - a repeat launch of the
  same client reads a few dozen bytes instead of re-hashing, and any
  client swap misses the key and rehashes exactly once.

- The AI-material manifests (object_manifest.json alone is over 3 MB,
  roughly 4 MB of JSON in total) were parsed at startup even though the
  feature is off by default and its runtime lookup is gated on the same
  flag. They now load once, on first enable - at startup when the player
  has already turned the feature on, or live on the F7/F12 enable.

- The water replacement shaders (a ~540-slot pixel stage, the largest fxc
  cost in the proxy) were compiled on the loading thread. They now compile
  on a worker thread while the game is still loading; the first water
  frame is the earliest possible consumer, and water draws before the
  publish simply stay native and switch over on the next tile. The depth
  capture hooks are installed while the compile is in flight, so a device
  created mid-compile is never left without the capture path, and a
  failed compile leaves the hooks dormant (their per-frame gates check
  readiness live).

No visible output changes: the fingerprint rules are byte-identical (the
hash value itself is unchanged, only when it is computed and whether it
is re-read from disk), AI materials behave exactly as before once enabled,
and water renders exactly the same shaders - the first view of water is
the earliest frame that can possibly need them.

Also from the deep sweep: the native-shadow diagnostics no longer pay the
full state dump (16 stage-texture queries plus viewport/cull/stencil,
about 34 driver calls) on every candidate draw; that collection now runs
only when a signature is first seen, so the repeated shadow-caster draws
cost three render-state reads while every unique candidate still logs the
complete state exactly once.
