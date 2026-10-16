---
area: renderer
type: improved
audience: players
title: Dynamic light pools are smoother and steadier, with a small driver-call reduction
---
The surface pass estimated each receiver's surface normal with a
one-sided gradient: for each axis it compared the two neighbor taps and
kept the side whose depth was closer to the receiver's own. On textured
ground the two sides disagree by more than noise most of the time, so
the normal picked a fresh corner of the tap window every pixel and the
light pool dappled and shimmered with the ground texture underneath it.
The pass now uses a central difference - averaging both sides of the
tap - which is the stable normal on any smooth surface. To keep cliff
faces and object bases lighting up from the correct direction, the
central difference is used only while both sides are within a small
depth step of the receiver (0.12 m, growing to 0.8 % of the distance);
the instant one side shows a real step the pass falls back to the near
side. The fallback triggers at the cliff line itself, where the
geometry actually turns, instead of mid-pool where the old depth-equal
switch landed.

The change is deliberately small in shader terms: all four neighbor
world positions and the same depth samples the old ternary already
evaluated are reused, so the static instruction count grows by roughly
ten to twenty instructions (estimated ~210 of the ps_3_0 budget of
256). Silhouettes, falloff, intensity, the water gate and the quality
0/1/2 mapping are untouched.

F7 "LIGHT DEBUG" mode 1 is restored to its documented purpose: it now
shows the raw light source contribution (attenuation only, before the
surface term) instead of duplicating mode 2's final sum. The accumulator
it was feeding already existed in the shader but had no mode that
showed it.

Two driver calls per frame are gone: the eight position, eight color
and eight direction vectors of the light table are one contiguous
c8..c31 range in the surface shader and now upload in a single
SetPixelShaderConstantF instead of three, and the composite's three
one-vector uploads (texel, debug, projection) collapse into one.

Verification: all 13 portable regression suites pass, including the
embedded-shader-source check that extracts both edited HLSL literals;
line-aware brace/paren balance on the edited file and both shader
literals; a numeric simulation of the new branch (flat ground and
slopes up to 50 % grade stay central, half-metre and larger steps fall
back to the near side, and the distance-scaled edge behaves correctly
at 2 m and 100 m); and a register-layout audit of the merged constant
uploads. The loop's local falloff variable was renamed so the new
outer-scope edge variable cannot shadow it in older fxc.
