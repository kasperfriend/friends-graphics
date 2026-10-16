---
area: renderer
type: fixed
audience: players
title: State-block passes no longer leak their shaders and blend flags into world classification
---
Effect passes that save the device into a state block used to leave their own pixel shader and blend flags behind in the state the world-draw classification reads, so a draw could be judged by the wrong shader. The passes now nest on a block stack and restore exactly what they took.