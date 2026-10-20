---
area: renderer
type: fixed
audience: players
title: Clouds and dynamic lights no longer travel with the camera in cities
---
Looking up in Orgrimmar from street level no longer drags the cloud deck or shifts the dynamic lights with the mouse. The camera fallback used where no terrain draw can be captured now only accepts a transform that belongs to the current camera, or holds the last verified one.
