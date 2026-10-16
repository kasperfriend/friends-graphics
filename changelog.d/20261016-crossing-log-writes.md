---
area: renderer
type: fixed
audience: players
title: Border crossings no longer write logs on frames that change nothing
---
Walking a border makes the client re-confirm your place several times, and each confirmation used to open, write and close the settings report in the game folder even when nothing downstream changed. Those lines now wait for the one open each frame already makes.
