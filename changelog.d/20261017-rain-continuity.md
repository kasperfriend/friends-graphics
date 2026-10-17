---
area: renderer
type: fixed
audience: players
title: Rain no longer flickers off during steady showers
---
Small or intermittent native rain batches keep rainfall active, and frames without the usual UI draw still get a rain pass. Clear weather eventually releases the held rain state.
