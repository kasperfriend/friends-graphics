---
area: renderer
type: fixed
audience: players
title: Grass and plants sway on clients whose shaders cut their leaves out
---
A plant is drawn as a card cut out of a transparent texture, and that texture is now what decides it is foliage - so fields of grass and trees lean even on a client whose render state shows no alpha test at all.
