---
area: renderer
type: fixed
audience: players
title: Clouds recover after temporary shader failures
---
Cloud shader creation is retried after temporary driver failures, so a transient error no longer leaves clouds disabled until the next device reset.
