---
area: renderer
type: fixed
audience: players
title: The sun and moon no longer shine through the ground
---
Looking up from under a floor or roof could let the moon be mistaken for that surface, so its glow showed through it. The moon now counts as a sprite only where the depth buffer holds one flat, camera-facing surface at its distance, and an unverified camera pose publishes no sun or moon position.
