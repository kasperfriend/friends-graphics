---
area: renderer
type: fixed
audience: players
title: The F7 menu now says why it cannot tell where you are
---
"Location unavailable" in every zone was sometimes a missing `EnvironmentProfiles.ini`, which used
to switch the location system off silently. It now starts anyway, the menu names the reason, and a
stray space in a typed hash no longer breaks client recognition.
