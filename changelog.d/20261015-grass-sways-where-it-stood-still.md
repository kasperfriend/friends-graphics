---
area: renderer
type: fixed
audience: players
title: Grass sways where it used to stand still
---
Some blade shaders keep their own lighting in the registers the bend works in, and those shaders were left exactly as the client drew them, so the field stood still in those places. The bend now moves to registers the shader is not using, and the log names them.
