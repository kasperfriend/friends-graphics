---
area: renderer
type: changed
audience: players
title: Crossing zone borders no longer hitches
---
Walking across a border re-applies only the settings that actually changed, and the renderer stops re-checking the tuning files on every read. Crossings that used to stutter for a couple of hundred milliseconds are now a few.
