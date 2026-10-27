---
area: renderer
type: fixed
audience: players
title: Keep volumetric clouds visible across camera pitches
---
Near-horizon world clouds no longer fade by pitch: feature-bounded marching preserves distant coverage, and erosion fades in through the lower deck instead of erasing thin bases. Clouds fade only at finite range; the sky-only fallback stays angle-independent and night lighting remains cool.
