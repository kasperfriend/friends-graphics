---
area: renderer
type: changed
audience: players
title: Blazing-fast load: startup no longer pays for work that cannot change its outcome
---
Startup no longer pays for three things that cannot change their outcome: the client hash is computed once per session, the AI-material manifests load only when the feature is first enabled, and the water shader compile holds up nothing else at start. Loads are quicker; the look is unchanged.