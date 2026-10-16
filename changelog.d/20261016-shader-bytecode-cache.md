---
area: renderer
type: changed
audience: players
title: Every shader compiles at most once per machine: on-disk bytecode cache
---
Every launch recompiled all of the renderer's shaders, even when nothing had changed. Bytecode now caches on disk next to the module, revalidated against its source, so a machine that ran this build once skips the compiler at startup. A first load still compiles; loads after it do not.