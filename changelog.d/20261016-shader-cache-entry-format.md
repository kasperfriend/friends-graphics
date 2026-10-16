---
area: renderer
type: fix
audience: players
title: Shader cache entries now match the format the reader validates
---
Cached shader files were written without the size and magic header the reader validates, so every launch treated the cache as empty and recompiled everything. Entries now store the header, so a second launch on the same machine reads the cached bytecode back as intended.
