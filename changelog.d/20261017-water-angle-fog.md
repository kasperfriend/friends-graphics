---
area: renderer
type: fixed
audience: players
title: Guard against water fading to a flat color at certain camera angles
---
An angle-selected water shader variant may use the expected fog constant differently. The replacement now validates that input and uses an unfogged fallback when it is malformed, preventing invalid fog values from flattening the water surface.
