---
area: renderer
type: fixed
audience: players
title: Zone water colours are no longer burnt out by a doubling in the shading
---
Water's per-zone colour was applied twice as strong as authored, so bright shallow water clipped to white and a zone's
own painted tint was washed out. The colour now multiplies exactly as authored: `0x808080` is neutral, and murkier or
deeper water keeps its hue instead of its brightness.
