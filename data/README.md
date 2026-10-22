# Bundled Ascension area database

`areas.txt` was exported from the running Ascension client and verified against
its `patch-M.MPQ` tables (2849 areas, 374 maps). The executable SHA-256 is
`e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6`.
The AreaTable/Map structure and IDs matched; Area 1's live name is Dun Morogh.
See `docs/ENVIRONMENT-SYSTEM-RU.md` for the original verification record.

Rechecked offline on 2026-10-03: executable fingerprint and location getter/setter
instructions still match. This is the installed client version, not a claim
that every launcher channel uses the same executable or tables.

The runtime also recognizes the supplied ArdenWoW SHA-256 as a 3.3.5a client
and uses the shared location RVAs. `areas.txt` remains Ascension-derived; if an
Arden server has custom AreaTable/Map IDs, export and supply its own authoritative
database before relying on zone-specific presets. Other unknown executables do
not opt into the bundled offsets and require independent location verification.
