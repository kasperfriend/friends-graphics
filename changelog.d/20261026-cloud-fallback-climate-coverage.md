---
area: renderer
type: fixed
audience: players
title: Keep upward-view clouds coherent
---
Sky-only recovery no longer projects the world weather map across screen directions, which made broad grey patches. It now uses zone/native-weather-resolved coverage and type at a medium angular scale; normal world clouds retain the full map and fresh-eye gate.
