---
area: renderer
type: fixed
audience: players
title: The underwater effect no longer drops out while swimming along
---
Diving no longer periodically loses the underwater fog, light beams and colour tint mid-swim. The renderer was giving up on the remembered surface whenever you swam on without it in view; it now only lets go when the frame could actually have seen that you are out.
