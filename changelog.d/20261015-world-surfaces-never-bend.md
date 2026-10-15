---
area: renderer
type: fixed
audience: players
title: Scenery drawn through the grass shader is never bent
---
A shader bent for the grass is also how the client draws pieces of the world, and one bent shader replaced every draw made with it. A batch whose own vertex count is a terrain tile's or a wall's is now left exactly as the client drew it, counted as `big` on the blade row.
