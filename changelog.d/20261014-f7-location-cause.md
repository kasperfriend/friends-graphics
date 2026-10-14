---
area: renderer
type: fixed
audience: players
title: F7 now reports the real reason location sensing is unavailable
---
The location line always claimed the provider was "not configured", even when it was configured and the actual problem was an unrecognized game build or unread memory addresses. It now names the true cause, and a configured provider waiting for a world frame says so instead.
