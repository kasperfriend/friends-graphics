---
area: renderer
type: fixed
audience: players
title: Busy harbours render water with less lag
---
Water now avoids repeated wave calculations, unused reflection and lighting work, and per-tile profile reads while preserving the existing sea, foam, reflection, and wind-driven motion.
