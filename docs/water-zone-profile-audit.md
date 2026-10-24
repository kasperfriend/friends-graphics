# Water zone profile audit

Generated from the authoritative `data/areas.txt` and `EnvironmentProfiles.ini` files.
The verified Ascension patch-M tables contain **374 maps** and **2849 area records**, including **414 map-root zones**. Every root has a direct `WaterAgitationMultiplier`; other areas retain the existing Map → Zone → Area inheritance order.

**Scope:** this is Ascension's custom, WotLK 3.3.5-based client table, not the stock Wrath DBC catalog. It includes Ascension map IDs and some expansion-derived names. Their presence in the loaded table makes them addressable by this supported client; it does not prove that the zone is currently playable or has rendered water. A stock 3.3.5 client is outside this renderer's tested target and will not auto-enable the verified Ascension location provider.

`1.00` is the neutral baseline. Lower values keep enclosed, sheltered or inland water calmer; higher values are reserved for exposed coasts and open seas. Neutral entries avoid inventing extra motion where a map has no known water or is a test/development space. Where a map has no rendered water, its multiplier is inert. These are authoring multipliers, not measurements of a live frame.

Automatic runtime application requires the exact supported Ascension executable (or another client whose location RVAs and table are explicitly verified). This audit checks every root ID, map association and profile resolution source; it cannot observe a running game process or confirm all zones in-world.

Regenerate or validate this list with `python3 tools/GenerateWaterZoneAudit.py` or `python3 tools/GenerateWaterZoneAudit.py --check`.

Colours are `WaterColour/DeepColour/AbsorptionColour`; `global` keeps the `GraphicsEffects.ini` set. Underwater is the swept zone tuple `clarity/turbidity/caustics/shafts/glow`, each a multiplier on the player dial; `player defaults` means the zone authors no underwater treatment of its own. Booty Bay (`[Area.35]`) overrides its Stranglethorn zone with azure harbour water.

| Map ID | Map | Zone ID | Root zone | Multiplier | Behavior | Colours (water/deep/absorption) | Underwater clarity/turbidity/caustics/shafts/glow |
| ---: | --- | ---: | --- | ---: | --- | --- | --- |
| 0 | Eastern Kingdoms | 1 | Dun Morogh | 0.50 | Sheltered / still water | 0x8FB4CC/0x0D2F4A/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 0 | Eastern Kingdoms | 3 | Badlands | 0.90 | Calm inland water | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 0 | Eastern Kingdoms | 4 | Blasted Lands | 0.95 | Calm inland water | 0x90705E/0x1E0C08/0xA88CD0 | 0.6/1.6/0.35/0.5/0.7 |
| 0 | Eastern Kingdoms | 8 | Swamp of Sorrows | 0.90 | Calm inland water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 0 | Eastern Kingdoms | 10 | Duskwood | 0.60 | Sheltered / still water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 0 | Eastern Kingdoms | 11 | Wetlands | 1.15 | Moderate movement | 0x6C7E70/0x0C1C14/0xA87CC8 | 0.5/2.1/0.3/0.45/0.6 |
| 0 | Eastern Kingdoms | 12 | Elwynn Forest | 0.45 | Sheltered / still water | 0x88ACBE/0x0F3348/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 0 | Eastern Kingdoms | 21 | Kul Tiras | 1.10 | Moderate movement | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 0 | Eastern Kingdoms | 25 | Blackrock Mountain | 0.35 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 28 | Western Plaguelands | 0.75 | Calm inland water | 0x66705E/0x0C1810/0xA87CC0 | 0.5/2/0.3/0.4/0.55 |
| 0 | Eastern Kingdoms | 30 | Nine | 1.00 | Neutral baseline | global | player defaults |
| 0 | Eastern Kingdoms | 33 | Stranglethorn Vale | 1.45 | Open sea / strongest movement | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 0 | Eastern Kingdoms | 36 | Alterac Mountains | 1.10 | Moderate movement | 0x8FB4CC/0x0D2F4A/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 0 | Eastern Kingdoms | 38 | Loch Modan | 0.65 | Sheltered / still water | 0x84A8C4/0x0E3049/0xE28A64 | 1.5/0.5/1.15/1.1/1.1 |
| 0 | Eastern Kingdoms | 40 | Westfall | 0.75 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 0 | Eastern Kingdoms | 41 | Deadwind Pass | 1.15 | Moderate movement | 0x5E6A62/0x08120E/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 0 | Eastern Kingdoms | 44 | Redridge Mountains | 0.70 | Calm inland water | 0x80A4BC/0x0D2C42/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 0 | Eastern Kingdoms | 45 | Arathi Highlands | 0.95 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 0 | Eastern Kingdoms | 46 | Burning Steppes | 1.00 | Neutral baseline | global | player defaults |
| 0 | Eastern Kingdoms | 47 | The Hinterlands | 0.85 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 0 | Eastern Kingdoms | 51 | Searing Gorge | 0.90 | Calm inland water | global | player defaults |
| 0 | Eastern Kingdoms | 85 | Tirisfal Glades | 0.55 | Sheltered / still water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 0 | Eastern Kingdoms | 130 | Silverpine Forest | 0.70 | Calm inland water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 0 | Eastern Kingdoms | 139 | Eastern Plaguelands | 0.80 | Calm inland water | 0x5E6A58/0x0A140C/0xA87CC0 | 0.5/2/0.3/0.4/0.55 |
| 0 | Eastern Kingdoms | 170 | Lordamere Lake | 1.30 | Exposed coast / island | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 0 | Eastern Kingdoms | 214 | The Great Sea | 1.50 | Open sea / strongest movement | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 0 | Eastern Kingdoms | 267 | Hillsbrad Foothills | 0.95 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 0 | Eastern Kingdoms | 269 | Dun Algaz | 0.45 | Sheltered / still water | 0x88ACBE/0x0F3348/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 0 | Eastern Kingdoms | 276 | UNUSED Stonewrought Pass | 1.00 | Neutral baseline | global | player defaults |
| 0 | Eastern Kingdoms | 293 | Thoradin's Wall | 0.45 | Sheltered / still water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 0 | Eastern Kingdoms | 296 | South Seas UNUSED | 1.00 | Neutral baseline | global | player defaults |
| 0 | Eastern Kingdoms | 308 | The Forbidding Sea | 1.50 | Open sea / strongest movement | 0x7AA2C0/0x082C46/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 0 | Eastern Kingdoms | 330 | Thandol Span | 0.90 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 0 | Eastern Kingdoms | 408 | Gillijim's Isle | 1.25 | Exposed coast / island | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 0 | Eastern Kingdoms | 409 | Island of Doctor Lapidis | 1.20 | Exposed coast / island | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 0 | Eastern Kingdoms | 1497 | Undercity | 0.30 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.3/2.8/0.15/0.2/0.4 |
| 0 | Eastern Kingdoms | 1519 | Stormwind City | 0.70 | Calm inland water | 0x768C78/0x0C2018/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 0 | Eastern Kingdoms | 1537 | Ironforge | 0.40 | Sheltered / still water | 0x8FB4CC/0x0D2F4A/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 0 | Eastern Kingdoms | 1579 | Unused The Deadmines 002 | 1.00 | Neutral baseline | global | player defaults |
| 0 | Eastern Kingdoms | 2037 | Quel'thalas | 0.90 | Calm inland water | 0x86B0D0/0x0C3E5E/0xEA9268 | 1.65/0.35/1.25/1.2/1.15 |
| 0 | Eastern Kingdoms | 2280 | DO NOT USE | 1.00 | Neutral baseline | global | player defaults |
| 0 | Eastern Kingdoms | 3459 | City | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 5614 | Westfall | 0.75 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 0 | Eastern Kingdoms | 5615 | Westfall | 0.75 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 0 | Eastern Kingdoms | 9998 | DEBUGGING_AREA_2 | 1.00 | Neutral baseline | global | player defaults |
| 0 | Eastern Kingdoms | 9999 | DEBUGGING_AREA | 1.00 | Neutral baseline | global | player defaults |
| 0 | Eastern Kingdoms | 10114 | Blackrock Mountain | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10115 | Blackrock Mountain | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10118 | Night Web's Hollow | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10119 | Gol'Bolar Quarry | 0.40 | Sheltered / still water | global | player defaults |
| 0 | Eastern Kingdoms | 10120 | Coldridge Pass | 0.45 | Sheltered / still water | 0x8FB4CC/0x0D2F4A/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 0 | Eastern Kingdoms | 10121 | The Grizzled Den | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10122 | Frostmane Hold | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10124 | Fargodeep Mine | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10125 | Fargodeep Mine | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10126 | Jasperlode Mine | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10130 | Echo Ridge Mine | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10135 | The Deadmines | 0.30 | Very calm / enclosed water | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 0 | Eastern Kingdoms | 10136 | Gold Coast Quarry | 0.35 | Very calm / enclosed water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 0 | Eastern Kingdoms | 10137 | Jangolode Mine | 0.30 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10138 | Northshire Valley | 0.55 | Sheltered / still water | 0x88ACBE/0x0F3348/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 0 | Eastern Kingdoms | 10139 | Coldridge Valley | 0.50 | Sheltered / still water | 0x8FB4CC/0x0D2F4A/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 0 | Eastern Kingdoms | 10140 | Deathknell | 0.55 | Sheltered / still water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 0 | Eastern Kingdoms | 10197 | Secret Inquisitorial Dungeon | 0.25 | Very calm / enclosed water | global | player defaults |
| 0 | Eastern Kingdoms | 10292 | Druk'Thar | 0.70 | Calm inland water | global | player defaults |
| 0 | Eastern Kingdoms | 10302 | Pale Reach | 1.10 | Moderate movement | 0x66849C/0x081E33/0xDC8660 | 1.45/0.4/0.9/1.1/1 |
| 0 | Eastern Kingdoms | 10305 | Shadewell | 0.40 | Sheltered / still water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 0 | Eastern Kingdoms | 10306 | Dun Kazad | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 14 | Durotar | 0.90 | Calm inland water | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 1 | Kalimdor | 15 | Dustwallow Marsh | 1.20 | Exposed coast / island | 0x707A5C/0x101C10/0xA87CC8 | 0.5/2.1/0.3/0.45/0.6 |
| 1 | Kalimdor | 16 | Azshara | 1.30 | Exposed coast / island | 0x82AEC8/0x0B3A56/0xEA9268 | 1.65/0.35/1.25/1.2/1.15 |
| 1 | Kalimdor | 17 | The Barrens | 0.90 | Calm inland water | 0x86AC9E/0x0F2E22/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 1 | Kalimdor | 141 | Teldrassil | 0.80 | Calm inland water | 0x7A7CA6/0x141434/0xDE8662 | 1.6/0.3/1.2/1.1/1.3 |
| 1 | Kalimdor | 148 | Darkshore | 1.35 | Exposed coast / island | 0x627488/0x081A2A/0xDC8660 | 0.8/1.2/0.55/0.7/0.75 |
| 1 | Kalimdor | 215 | Mulgore | 0.85 | Calm inland water | 0x88ACBE/0x0F3348/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 1 | Kalimdor | 257 | Shadowthread Cave | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 258 | Fel Rock | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 262 | Ban'ethil Barrow Den | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 331 | Ashenvale | 1.15 | Moderate movement | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 1 | Kalimdor | 332 | The Great Sea | 1.50 | Open sea / strongest movement | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 1 | Kalimdor | 357 | Feralas | 1.20 | Exposed coast / island | 0x6E8A68/0x0C2414/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 1 | Kalimdor | 361 | Felwood | 0.85 | Calm inland water | 0x5C7048/0x0A1A0A/0xB86ED8 | 0.5/1.9/0.35/0.5/0.8 |
| 1 | Kalimdor | 365 | Burning Blade Coven | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 371 | Dustwind Cave | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 377 | Southfury River | 1.10 | Moderate movement | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 1 | Kalimdor | 400 | Thousand Needles | 1.00 | Neutral baseline | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 1 | Kalimdor | 405 | Desolace | 1.00 | Neutral baseline | 0x627488/0x081A2A/0xDC8660 | 0.8/1.2/0.55/0.7/0.75 |
| 1 | Kalimdor | 406 | Stonetalon Mountains | 0.95 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 1 | Kalimdor | 440 | Tanaris | 1.05 | Moderate movement | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 1 | Kalimdor | 457 | The Veiled Sea | 1.40 | Open sea / strongest movement | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 1 | Kalimdor | 490 | Un'Goro Crater | 0.90 | Calm inland water | 0x6E8256/0x0E2010/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 1 | Kalimdor | 493 | Moonglade | 0.75 | Calm inland water | 0x7688B4/0x0A1E3D/0xDE8662 | 1.8/0.2/1.2/1.1/1.4 |
| 1 | Kalimdor | 616 | Hyjal | 0.85 | Calm inland water | 0x7688B4/0x0A1E3D/0xDE8662 | 1.9/0.2/1.2/1.15/1.45 |
| 1 | Kalimdor | 618 | Winterspring | 0.90 | Calm inland water | 0x92B4CC/0x10304C/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 1 | Kalimdor | 876 | GM Island | 1.00 | Neutral baseline | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 1 | Kalimdor | 1377 | Silithus | 1.05 | Moderate movement | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 1 | Kalimdor | 1637 | Orgrimmar | 0.35 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 1 | Kalimdor | 1638 | Thunder Bluff | 0.50 | Sheltered / still water | 0x88ACBE/0x0F3348/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 1 | Kalimdor | 1657 | Darnassus | 0.60 | Sheltered / still water | 0x7688B4/0x0A1E3D/0xDE8662 | 1.6/0.3/1.2/1.1/1.3 |
| 1 | Kalimdor | 1941 | Caverns of Time | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 2159 | Onyxia's Lair | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 2917 | Hall of Legends | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 3446 | Twilight's Run | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 3478 | Gates of Ahn'Qiraj | 0.50 | Sheltered / still water | global | player defaults |
| 1 | Kalimdor | 4017 | The Barrens | 0.90 | Calm inland water | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 1 | Kalimdor | 4331 | Ashenvale | 1.15 | Moderate movement | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 1 | Kalimdor | 10113 | Caverns of Time | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10116 | Maraudon | 0.30 | Very calm / enclosed water | 0x5E7A72/0x071812/0xC8788C | 0.75/1.2/0.08/0.1/0.25 |
| 1 | Kalimdor | 10123 | Skull Rock | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10128 | Palemane Rock | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10129 | The Venture Co. Mine | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10131 | The Noxious Lair | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10132 | The Gaping Chasm | 0.35 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10133 | Ban'ethil Barrow Den | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10134 | The Slithering Scar | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10143 | Shadowglen | 0.70 | Calm inland water | 0x7A7CA6/0x141434/0xDE8662 | 1.6/0.3/1.2/1.1/1.3 |
| 1 | Kalimdor | 10144 | Valley of Trials | 0.70 | Calm inland water | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 1 | Kalimdor | 10145 | Caverns of Time | 0.30 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10146 | Red Cloud Mesa | 0.65 | Sheltered / still water | 0x88ACBE/0x0F3348/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 1 | Kalimdor | 10205 | Sinister Lair | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10207 | Moonlit Ossuary | 0.25 | Very calm / enclosed water | global | player defaults |
| 1 | Kalimdor | 10239 | Depthbreaker's Hollow | 0.60 | Sheltered / still water | 0x627488/0x081A2A/0xDC8660 | 0.8/1.2/0.55/0.7/0.75 |
| 13 | Testing | 3817 | Testing | 1.00 | Neutral baseline | global | player defaults |
| 13 | Testing | 5003 | Open World | 1.00 | Neutral baseline | global | player defaults |
| 13 | Testing | 11211 | Ascension | 1.00 | Neutral baseline | global | player defaults |
| 30 | Alterac Valley | 2597 | Alterac Valley | 0.85 | Calm inland water | 0x8FB4CC/0x0D2F4A/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 33 | Shadowfang Keep | 209 | Shadowfang Keep | 0.30 | Very calm / enclosed water | global | player defaults |
| 34 | Stormwind Stockade | 717 | The Stockade | 0.30 | Very calm / enclosed water | global | player defaults |
| 34 | Stormwind Stockade | 4717 | Unused Monastery | 1.00 | Neutral baseline | global | player defaults |
| 34 | Stormwind Stockade | 7717 | Vardrheim | 0.30 | Very calm / enclosed water | global | player defaults |
| 36 | Deadmines | 207 | The Great Sea | 1.45 | Open sea / strongest movement | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 36 | Deadmines | 208 | Unused Ironcladcove | 1.00 | Neutral baseline | global | player defaults |
| 36 | Deadmines | 1581 | The Deadmines | 0.30 | Very calm / enclosed water | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 37 | Azshara Crater | 268 | Azshara Crater | 0.85 | Calm inland water | 0x86B0D0/0x0C3E5E/0xEA9268 | 1.65/0.35/1.25/1.2/1.15 |
| 43 | Wailing Caverns | 718 | Wailing Caverns | 0.30 | Very calm / enclosed water | 0x5E7A72/0x071812/0xC8788C | 0.75/1.2/0.08/0.1/0.25 |
| 47 | Razorfen Kraul | 491 | Razorfen Kraul | 0.30 | Very calm / enclosed water | global | player defaults |
| 48 | Blackfathom Deeps | 719 | Blackfathom Deeps | 0.55 | Sheltered / still water | 0x627488/0x081A2A/0xDC8660 | 0.8/1.2/0.55/0.7/0.75 |
| 70 | Uldaman | 1337 | Uldaman | 0.30 | Very calm / enclosed water | global | player defaults |
| 90 | Gnomeregan | 721 | Gnomeregan | 0.30 | Very calm / enclosed water | 0x74984E/0x0A2408/0xB86ED8 | 0.5/1.9/0.35/0.5/0.8 |
| 109 | Sunken Temple | 1417 | Sunken Temple | 0.45 | Sheltered / still water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 109 | Sunken Temple | 1477 | The Temple of Atal'Hakkar | 0.45 | Sheltered / still water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 129 | Razorfen Downs | 722 | Razorfen Downs | 0.30 | Very calm / enclosed water | global | player defaults |
| 169 | Emerald Dream | 956 | The Verdant Fields | 0.80 | Calm inland water | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 169 | Emerald Dream | 1397 | Emerald Forest | 0.80 | Calm inland water | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 189 | Scarlet Monastery | 796 | Scarlet Monastery | 0.30 | Very calm / enclosed water | global | player defaults |
| 209 | Zul'Farrak | 1176 | Zul'Farrak | 0.30 | Very calm / enclosed water | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 229 | Blackrock Spire | 1583 | Blackrock Spire | 0.30 | Very calm / enclosed water | global | player defaults |
| 230 | Blackrock Depths | 1584 | Blackrock Depths | 0.30 | Very calm / enclosed water | global | player defaults |
| 269 | Opening of the Dark Portal | 2366 | The Black Morass | 0.90 | Calm inland water | 0x707A5C/0x101C10/0xA87CC8 | 0.5/2.1/0.3/0.45/0.6 |
| 289 | Scholomance | 2057 | Scholomance | 0.30 | Very calm / enclosed water | global | player defaults |
| 309 | Zul'Gurub | 1977 | Zul'Gurub | 0.80 | Calm inland water | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 329 | Stratholme | 2017 | Stratholme | 0.30 | Very calm / enclosed water | global | player defaults |
| 349 | Maraudon | 2100 | Maraudon | 0.55 | Sheltered / still water | 0x5E7A72/0x071812/0xC8788C | 0.75/1.2/0.08/0.1/0.25 |
| 369 | Deeprun Tram | 2257 | Deeprun Tram | 0.55 | Sheltered / still water | 0x7AA2C0/0x082C46/0xEC9066 | 1.3/0.5/0/0/0.3 |
| 389 | Ragefire Chasm | 2437 | Ragefire Chasm | 0.30 | Very calm / enclosed water | global | player defaults |
| 400 | Siltithus Setis | 14000 | Silithus | 0.45 | Sheltered / still water | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 409 | Molten Core | 2717 | Molten Core | 0.30 | Very calm / enclosed water | global | player defaults |
| 429 | Dire Maul | 2557 | Dire Maul | 0.30 | Very calm / enclosed water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 449 | Alliance PVP Barracks | 2918 | Champions' Hall | 0.30 | Very calm / enclosed water | global | player defaults |
| 451 | Development Land | 22 | Programmer Isle | 1.00 | Neutral baseline | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 451 | Development Land | 151 | Designer Island | 1.00 | Neutral baseline | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 451 | Development Land | 3948 | Brian and Pat Test | 1.00 | Neutral baseline | global | player defaults |
| 451 | Development Land | 4019 | Development Land | 1.00 | Neutral baseline | global | player defaults |
| 469 | Blackwing Lair | 2677 | Blackwing Lair | 0.30 | Very calm / enclosed water | global | player defaults |
| 489 | Warsong Gulch | 3277 | Warsong Gulch | 0.80 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 509 | Ruins of Ahn'Qiraj | 3429 | Ruins of Ahn'Qiraj | 0.40 | Sheltered / still water | global | player defaults |
| 529 | Arathi Basin | 3358 | Arathi Basin | 0.95 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 530 | Outland | 3430 | Eversong Woods | 0.70 | Calm inland water | 0x86AEB8/0x0C3E4A/0xD48E78 | 1.65/0.35/1.25/1.2/1.15 |
| 530 | Outland | 3433 | Ghostlands | 0.70 | Calm inland water | 0x5A6A66/0x08141A/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 530 | Outland | 3455 | The North Sea | 1.45 | Open sea / strongest movement | 0x749CB8/0x0A2E47/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 530 | Outland | 3479 | The Veiled Sea | 1.40 | Open sea / strongest movement | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 530 | Outland | 3483 | Hellfire Peninsula | 0.95 | Calm inland water | 0x90705E/0x1E0C08/0xA88CD0 | 0.6/1.6/0.35/0.5/0.7 |
| 530 | Outland | 3487 | Silvermoon City | 0.60 | Sheltered / still water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 530 | Outland | 3518 | Nagrand | 0.90 | Calm inland water | 0x88ACBE/0x0F3348/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 530 | Outland | 3519 | Terokkar Forest | 0.90 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 530 | Outland | 3520 | Shadowmoon Valley | 0.90 | Calm inland water | 0x54684E/0x081408/0xB86ED8 | 0.5/1.9/0.35/0.5/0.8 |
| 530 | Outland | 3521 | Zangarmarsh | 1.00 | Neutral baseline | 0x6A7C5E/0x0E1E12/0x9C7CD8 | 0.32/2.7/0.22/0.35/0.55 |
| 530 | Outland | 3522 | Blade's Edge Mountains | 1.10 | Moderate movement | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 530 | Outland | 3523 | Netherstorm | 0.70 | Calm inland water | 0x8A7CA8/0x160E30/0x9CD8B0 | 1.35/0.5/1.1/1.1/1.25 |
| 530 | Outland | 3524 | Azuremyst Isle | 1.25 | Exposed coast / island | 0x86B0D0/0x0C3E5E/0xEA9268 | 1.65/0.35/1.25/1.2/1.15 |
| 530 | Outland | 3525 | Bloodmyst Isle | 1.20 | Exposed coast / island | 0xA88A8C/0x2A1420/0x6CC4E4 | 0.45/1.9/0.35/0.5/0.8 |
| 530 | Outland | 3540 | Twisting Nether | 1.30 | Exposed coast / island | global | player defaults |
| 530 | Outland | 3557 | The Exodar | 0.60 | Sheltered / still water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 530 | Outland | 3569 | Tides' Hollow | 0.80 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 530 | Outland | 3703 | Shattrath City | 0.70 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 530 | Outland | 3917 | Auchindoun | 0.25 | Very calm / enclosed water | global | player defaults |
| 530 | Outland | 4080 | Isle of Quel'Danas | 1.20 | Exposed coast / island | 0x86B0D0/0x0C3E5E/0xEA9268 | 1.65/0.35/1.25/1.2/1.15 |
| 530 | Outland | 10117 | Stillpine Hold | 0.65 | Sheltered / still water | global | player defaults |
| 530 | Outland | 10127 | Amani Catacombs | 0.25 | Very calm / enclosed water | global | player defaults |
| 530 | Outland | 10141 | Sunstrider Isle | 1.00 | Neutral baseline | 0x86B0D0/0x0C3E5E/0xEA9268 | 1.65/0.35/1.25/1.2/1.15 |
| 530 | Outland | 10142 | Ammen Vale | 0.85 | Calm inland water | 0x86B0D0/0x0C3E5E/0xEA9268 | 1.65/0.35/1.25/1.2/1.15 |
| 531 | Ahn'Qiraj Temple | 3428 | Ahn'Qiraj | 0.30 | Very calm / enclosed water | global | player defaults |
| 532 | Karazhan | 3457 | Karazhan | 0.30 | Very calm / enclosed water | global | player defaults |
| 533 | Naxxramas | 3456 | Naxxramas | 0.30 | Very calm / enclosed water | global | player defaults |
| 534 | The Battle for Mount Hyjal | 3606 | Hyjal Summit | 0.85 | Calm inland water | 0x7688B4/0x0A1E3D/0xDE8662 | 1.9/0.2/1.2/1.15/1.45 |
| 535 | Naxxramas | 3458 | Naxxramas | 0.30 | Very calm / enclosed water | global | player defaults |
| 540 | Hellfire Citadel: The Shattered Halls | 3535 | Hellfire Citadel | 0.30 | Very calm / enclosed water | global | player defaults |
| 540 | Hellfire Citadel: The Shattered Halls | 3714 | The Shattered Halls | 0.30 | Very calm / enclosed water | global | player defaults |
| 542 | Hellfire Citadel: The Blood Furnace | 3713 | The Blood Furnace | 0.30 | Very calm / enclosed water | global | player defaults |
| 543 | Hellfire Citadel: Ramparts | 3562 | Hellfire Ramparts | 0.30 | Very calm / enclosed water | global | player defaults |
| 544 | Magtheridon's Lair | 3836 | Magtheridon's Lair | 0.30 | Very calm / enclosed water | global | player defaults |
| 545 | Coilfang: The Steamvault | 3715 | The Steamvault | 0.80 | Calm inland water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 546 | Coilfang: The Underbog | 3716 | The Underbog | 0.85 | Calm inland water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 547 | Coilfang: The Slave Pens | 3717 | The Slave Pens | 0.80 | Calm inland water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 548 | Coilfang: Serpentshrine Cavern | 3607 | Serpentshrine Cavern | 0.80 | Calm inland water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.65/1.4/0.1/0.15/0.3 |
| 550 | Tempest Keep | 3845 | Tempest Keep | 0.30 | Very calm / enclosed water | global | player defaults |
| 552 | Tempest Keep: The Arcatraz | 3848 | The Arcatraz | 0.30 | Very calm / enclosed water | global | player defaults |
| 553 | Tempest Keep: The Botanica | 3847 | The Botanica | 0.30 | Very calm / enclosed water | global | player defaults |
| 554 | Tempest Keep: The Mechanar | 3849 | The Mechanar | 0.30 | Very calm / enclosed water | global | player defaults |
| 555 | Auchindoun: Shadow Labyrinth | 3789 | Shadow Labyrinth | 0.30 | Very calm / enclosed water | global | player defaults |
| 556 | Auchindoun: Sethekk Halls | 3791 | Sethekk Halls | 0.30 | Very calm / enclosed water | global | player defaults |
| 557 | Auchindoun: Mana-Tombs | 3792 | Mana-Tombs | 0.30 | Very calm / enclosed water | global | player defaults |
| 558 | Auchindoun: Auchenai Crypts | 3790 | Auchenai Crypts | 0.30 | Very calm / enclosed water | global | player defaults |
| 559 | Nagrand Arena | 3698 | Nagrand Arena | 0.65 | Sheltered / still water | global | player defaults |
| 560 | The Escape From Durnholde | 2367 | Old Hillsbrad Foothills | 0.85 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 560 | The Escape From Durnholde | 3605 | Hyjal Past | 0.85 | Calm inland water | 0x7688B4/0x0A1E3D/0xDE8662 | 1.9/0.2/1.2/1.15/1.45 |
| 562 | Blade's Edge Arena | 3702 | Blade's Edge Arena | 0.65 | Sheltered / still water | global | player defaults |
| 564 | Black Temple | 3959 | Black Temple | 0.30 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 565 | Gruul's Lair | 3923 | Gruul's Lair | 0.30 | Very calm / enclosed water | global | player defaults |
| 566 | Eye of the Storm | 3820 | Eye of the Storm | 0.65 | Sheltered / still water | global | player defaults |
| 568 | Zul'Aman | 3805 | Zul'Aman | 0.75 | Calm inland water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 568 | Zul'Aman | 4805 | Tor'Watha | 0.75 | Calm inland water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 571 | Northrend | 65 | Dragonblight | 1.00 | Neutral baseline | 0x688094/0x0A2033/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 571 | Northrend | 66 | Zul'Drak | 1.00 | Neutral baseline | 0x66705E/0x0C1810/0xA87CC0 | 0.5/2/0.3/0.4/0.55 |
| 571 | Northrend | 67 | The Storm Peaks | 1.35 | Exposed coast / island | 0x8FB0CC/0x0D2A47/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 571 | Northrend | 210 | Icecrown | 1.10 | Moderate movement | 0x5E7488/0x061626/0xDC8660 | 1.45/0.4/0.9/1.1/1 |
| 571 | Northrend | 394 | Grizzly Hills | 1.00 | Neutral baseline | 0x74928A/0x0C2A22/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 571 | Northrend | 495 | Howling Fjord | 1.30 | Exposed coast / island | 0x6E8CA4/0x0A2438/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 571 | Northrend | 2817 | Crystalsong Forest | 0.85 | Calm inland water | 0x8A7CA8/0x160E30/0x9CD8B0 | 1.35/0.5/1.1/1.1/1.25 |
| 571 | Northrend | 3477 | Azjol-Nerub | 0.25 | Very calm / enclosed water | global | player defaults |
| 571 | Northrend | 3537 | Borean Tundra | 1.25 | Exposed coast / island | 0x7492AC/0x0B283E/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 571 | Northrend | 3711 | Sholazar Basin | 1.10 | Moderate movement | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 571 | Northrend | 3979 | The Frozen Sea | 1.40 | Open sea / strongest movement | 0x66849C/0x081E33/0xDC8660 | 1.45/0.4/0.9/1.1/1 |
| 571 | Northrend | 4197 | Wintergrasp | 1.20 | Exposed coast / island | 0x8FB4CC/0x0D2F4A/0xE88F66 | 1.65/0.3/1.15/1.2/1.15 |
| 571 | Northrend | 4201 | Vordrassil's Tears | 0.85 | Calm inland water | 0x88ACBE/0x0F3348/0xE28A64 | 1.45/0.45/1.15/1.1/1.1 |
| 571 | Northrend | 4258 | The North Sea | 1.45 | Open sea / strongest movement | 0x66849C/0x081E33/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 571 | Northrend | 4395 | Dalaran | 0.80 | Calm inland water | 0x84A0C0/0x0E2C48/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 571 | Northrend | 4602 | Force Interior | 0.25 | Very calm / enclosed water | global | player defaults |
| 571 | Northrend | 4630 | The North Sea | 1.45 | Open sea / strongest movement | 0x6E8CA4/0x0A2438/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 571 | Northrend | 4742 | Hrothgar's Landing | 1.20 | Exposed coast / island | 0x6E8CA4/0x0A2438/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 571 | Northrend | 13979 | The Frozen Sea | 1.40 | Open sea / strongest movement | 0x66849C/0x081E33/0xDC8660 | 1.45/0.4/0.9/1.1/1 |
| 572 | Ruins of Lordaeron | 3968 | Ruins of Lordaeron | 0.25 | Very calm / enclosed water | global | player defaults |
| 574 | Utgarde Keep | 206 | Utgarde Keep | 0.30 | Very calm / enclosed water | global | player defaults |
| 575 | Utgarde Pinnacle | 1196 | Utgarde Pinnacle | 0.30 | Very calm / enclosed water | global | player defaults |
| 576 | The Nexus | 4265 | The Nexus | 0.30 | Very calm / enclosed water | global | player defaults |
| 578 | The Oculus | 4228 | The Oculus | 0.30 | Very calm / enclosed water | global | player defaults |
| 580 | The Sunwell | 4075 | Sunwell Plateau | 0.30 | Very calm / enclosed water | global | player defaults |
| 585 | Magister's Terrace | 4131 | Magisters' Terrace | 0.30 | Very calm / enclosed water | global | player defaults |
| 595 | The Culling of Stratholme | 4100 | The Culling of Stratholme | 0.30 | Very calm / enclosed water | global | player defaults |
| 598 | Sunwell Fix (Unused) | 4076 | Reuse Me 7 | 1.00 | Neutral baseline | global | player defaults |
| 599 | Halls of Stone | 4264 | Halls of Stone | 0.30 | Very calm / enclosed water | global | player defaults |
| 600 | Drak'Tharon Keep | 4196 | Drak'Tharon Keep | 0.30 | Very calm / enclosed water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 601 | Azjol-Nerub | 4277 | Azjol-Nerub | 0.30 | Very calm / enclosed water | 0x5E7A72/0x071812/0xC8788C | 0.75/1.2/0.08/0.1/0.25 |
| 602 | Halls of Lightning | 4272 | Halls of Lightning | 0.30 | Very calm / enclosed water | global | player defaults |
| 603 | Ulduar | 4273 | Ulduar | 0.30 | Very calm / enclosed water | global | player defaults |
| 604 | Gundrak | 4416 | Gundrak | 0.60 | Sheltered / still water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 607 | Strand of the Ancients | 4384 | Strand of the Ancients | 1.30 | Exposed coast / island | 0x6E8CA4/0x0A2438/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 608 | Violet Hold | 4415 | The Violet Hold | 0.30 | Very calm / enclosed water | global | player defaults |
| 609 | Ebon Hold | 4298 | Plaguelands: The Scarlet Enclave | 0.30 | Very calm / enclosed water | 0x66705E/0x0C1810/0xA87CC0 | 0.5/2/0.3/0.4/0.55 |
| 615 | The Obsidian Sanctum | 4493 | The Obsidian Sanctum | 0.25 | Very calm / enclosed water | global | player defaults |
| 616 | The Eye of Eternity | 4500 | The Eye of Eternity | 0.30 | Very calm / enclosed water | global | player defaults |
| 617 | Dalaran Sewers | 4378 | Dalaran Arena | 0.35 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 618 | The Ring of Valor | 4406 | The Ring of Valor | 0.30 | Very calm / enclosed water | global | player defaults |
| 619 | Ahn'kahet: The Old Kingdom | 4494 | Ahn'kahet: The Old Kingdom | 0.30 | Very calm / enclosed water | 0x5E7A72/0x071812/0xC8788C | 0.75/1.2/0.08/0.1/0.25 |
| 624 | Vault of Archavon | 4603 | Vault of Archavon | 0.25 | Very calm / enclosed water | global | player defaults |
| 628 | Isle of Conquest | 4710 | Isle of Conquest | 1.25 | Exposed coast / island | 0x6E8CA4/0x0A2438/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 631 | Icecrown Citadel | 4812 | Icecrown Citadel | 0.25 | Very calm / enclosed water | global | player defaults |
| 631 | Icecrown Citadel | 4893 | The Frost Queen's Lair | 0.25 | Very calm / enclosed water | global | player defaults |
| 631 | Icecrown Citadel | 4894 | Putricide's Laboratory of Alchemical Horrors and Fun | 0.25 | Very calm / enclosed water | global | player defaults |
| 631 | Icecrown Citadel | 4895 | The Crimson Hall | 0.25 | Very calm / enclosed water | global | player defaults |
| 631 | Icecrown Citadel | 4896 | The Frozen Throne | 0.25 | Very calm / enclosed water | global | player defaults |
| 631 | Icecrown Citadel | 4897 | The Sanctum of Blood | 0.25 | Very calm / enclosed water | global | player defaults |
| 632 | The Forge of Souls | 4809 | The Forge of Souls | 0.25 | Very calm / enclosed water | global | player defaults |
| 641 | Transport: Alliance Airship BG | 4763 | Transport: Alliance Gunship | 0.25 | Very calm / enclosed water | global | player defaults |
| 642 | Transport: HordeAirshipBG | 4764 | Transport: Horde Gunship | 0.25 | Very calm / enclosed water | global | player defaults |
| 645 | Blackrock Caverns | 4926 | Blackrock Caverns | 0.30 | Very calm / enclosed water | global | player defaults |
| 649 | Trial of the Crusader | 4722 | Trial of the Crusader | 0.30 | Very calm / enclosed water | global | player defaults |
| 650 | Trial of the Champion | 4723 | Trial of the Champion | 0.30 | Very calm / enclosed water | global | player defaults |
| 658 | Pit of Saron | 4813 | Pit of Saron | 0.30 | Very calm / enclosed water | global | player defaults |
| 668 | Halls of Reflection | 4820 | Halls of Reflection | 0.25 | Very calm / enclosed water | global | player defaults |
| 672 | Transport: The Skybreaker (Icecrown Citadel Raid) | 4832 | Transport: Alliance Gunship (IGB) | 0.30 | Very calm / enclosed water | global | player defaults |
| 673 | Transport: Orgrim's Hammer (Icecrown Citadel Raid) | 4833 | Transport: Horde Gunship (IGB) | 0.30 | Very calm / enclosed water | global | player defaults |
| 724 | The Ruby Sanctum | 4987 | The Ruby Sanctum | 0.30 | Very calm / enclosed water | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 726 | Twin Peaks | 5031 | Twin Peaks | 0.85 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 800 | Battle Royale | 5000 | Battle Royal | 1.00 | Neutral baseline | global | player defaults |
| 800 | Battle Royale | 10100 | Test FFa | 1.00 | Neutral baseline | global | player defaults |
| 806 | Twisting Nether | 4999 | Twisting Nether | 0.35 | Very calm / enclosed water | global | player defaults |
| 808 | Twisting Nether | 5004 | Twisting Nether | 0.35 | Very calm / enclosed water | global | player defaults |
| 820 | Frozen Tears Sanctum | 10104 | Frozen Tears Sanctum | 0.30 | Very calm / enclosed water | global | player defaults |
| 821 | Tinkertech Showdown | 10108 | Tinkertech Showdown | 0.30 | Very calm / enclosed water | global | player defaults |
| 850 | Housing Human | 10017 | Housing Human | 0.30 | Very calm / enclosed water | global | player defaults |
| 880 | Setis (PvE) | 10235 | The Scarab Wall | 0.40 | Sheltered / still water | global | player defaults |
| 881 | Azuregos (PvE) | 10236 | Azuregos (PvE) | 0.85 | Calm inland water | global | player defaults |
| 882 | Lord Kazzak (PvE) | 10237 | Throne of the Doom Lord | 0.35 | Very calm / enclosed water | global | player defaults |
| 883 | Soggoth (PvE) | 10238 | Master’s Gastric Pit | 0.30 | Very calm / enclosed water | global | player defaults |
| 884 | Depthbreaker's Hollow | 10240 | Depthbreaker's Hollow | 0.80 | Calm inland water | 0x627488/0x081A2A/0xDC8660 | 0.8/1.2/0.55/0.7/0.75 |
| 885 | Emeriss (PvE) | 10271 | Bough Shadow | 0.75 | Calm inland water | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 886 | Taerar (PvE) | 10272 | Dream Bough | 0.75 | Calm inland water | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 887 | Ysondre (PvE) | 10273 | Seradane | 0.75 | Calm inland water | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 888 | Lethon (PvE) | 10274 | Twilight Grove | 0.75 | Calm inland water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 889 | Snowgrave (PvE) | 10280 | Snowgrave's Cavern | 0.25 | Very calm / enclosed water | global | player defaults |
| 890 | Atal'zull (PvE) | 10281 | Zul'Mashar | 0.65 | Sheltered / still water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 900 | Future Northrend | 10330 | Future Northrend | 0.85 | Calm inland water | global | player defaults |
| 901 | Newnerub | 10018 | Newnerub | 0.30 | Very calm / enclosed water | global | player defaults |
| 902 | Blacklineage | 10010 | Blacklineage | 0.30 | Very calm / enclosed water | global | player defaults |
| 903 | Stormwind Sewer | 10011 | Stormwind Sewer | 0.30 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 903 | Stormwind Sewer | 10065 | Stormwind Sewers | 0.30 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 904 | Orgrimmar Depths | 10012 | Orgrimmar Depths | 0.30 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 905 | Mish Faramos | 10013 | Mish Faramos | 0.75 | Calm inland water | global | player defaults |
| 906 | Harvest Island | 10009 | Harvest Island | 1.15 | Moderate movement | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 907 | Fading Island | 10019 | Sea of Azzar | 1.45 | Open sea / strongest movement | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 907 | Fading Island | 10111 | Helyas revenge (general area) | 0.85 | Calm inland water | 0x627488/0x081A2A/0xDC8660 | 0.8/1.2/0.55/0.7/0.75 |
| 907 | Fading Island | 10163 | Fading Island | 1.15 | Moderate movement | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 908 | Frozen Reach | 10023 | Frozen Reach | 1.25 | Exposed coast / island | 0x66849C/0x081E33/0xDC8660 | 1.45/0.4/0.9/1.1/1 |
| 909 | Azzar Faire | 10078 | Dawnrise Island | 1.15 | Moderate movement | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 909 | Azzar Faire | 10081 | Glorious Azzar Faire | 1.00 | Neutral baseline | global | player defaults |
| 920 | SpellDevMap | 10007 | Spell Dev Map | 1.00 | Neutral baseline | global | player defaults |
| 921 | BlackDevMap | 10006 | Black Dev Map | 1.00 | Neutral baseline | global | player defaults |
| 922 | DungeonDevMap | 10014 | Dungeon Dev Map | 1.00 | Neutral baseline | global | player defaults |
| 923 | RaidDevMap | 10015 | Raid Dev Map | 1.00 | Neutral baseline | global | player defaults |
| 924 | OutdoorDevMap | 10066 | OutdoorDevMap | 1.00 | Neutral baseline | global | player defaults |
| 925 | MinigamesDevMap | 10067 | MinigamesDevMap | 1.00 | Neutral baseline | global | player defaults |
| 926 | RobinDevMap | 10097 | RobinDevMap | 1.00 | Neutral baseline | global | player defaults |
| 927 | Azerothdev | 10161 | Horde Infiltration Camp | 1.00 | Neutral baseline | global | player defaults |
| 929 | AdtDevMap | 10156 | ADT Dev Map | 1.00 | Neutral baseline | global | player defaults |
| 930 | Underworld | 10000 | Underworld | 0.30 | Very calm / enclosed water | global | player defaults |
| 930 | Underworld | 10001 | Underworld | 0.30 | Very calm / enclosed water | global | player defaults |
| 931 | Astaroth | 10002 | Astaroth | 0.30 | Very calm / enclosed water | global | player defaults |
| 932 | A Wrinkle in Time | 10008 | A Wrinkle in Time | 0.75 | Calm inland water | global | player defaults |
| 933 | Regret's Chasm | 10060 | Pools of the Eidola | 0.80 | Calm inland water | 0x8A7CA8/0x160E30/0x9CD8B0 | 1.35/0.5/1.1/1.1/1.25 |
| 936 | Vaults of the Inquisition | 10218 | Vaults of the Inquisition | 0.30 | Very calm / enclosed water | global | player defaults |
| 937 | Road to De Other Side | 10219 | Road to De Other Side | 0.60 | Sheltered / still water | global | player defaults |
| 938 | Azga Necromancer Dungeon | 10233 | Azga Necromancer Dungeon | 0.30 | Very calm / enclosed water | global | player defaults |
| 939 | Vulture Cult Sanctuary Dungeon | 10234 | Vulture Cult Sanctuary Dungeon | 0.30 | Very calm / enclosed water | global | player defaults |
| 940 | Shadow Bone Depths | 10300 | The Deadwood | 0.75 | Calm inland water | 0x74984E/0x0A2408/0xB86ED8 | 0.5/1.9/0.35/0.5/0.8 |
| 940 | Shadow Bone Depths | 10301 | A Dark Place | 0.30 | Very calm / enclosed water | global | player defaults |
| 940 | Shadow Bone Depths | 10328 | Shadowbone Depths | 0.30 | Very calm / enclosed water | global | player defaults |
| 940 | Shadow Bone Depths | 10329 | A Dark Place | 0.30 | Very calm / enclosed water | global | player defaults |
| 941 | Temple Of Embers | 10331 | Temple of Embers | 0.30 | Very calm / enclosed water | global | player defaults |
| 950 | Aspects Trials | 10003 | Aspects Trials | 0.30 | Very calm / enclosed water | global | player defaults |
| 950 | Aspects Trials | 10004 | Throne of Norgannon | 0.30 | Very calm / enclosed water | global | player defaults |
| 950 | Aspects Trials | 10005 | Halls of Valor | 0.30 | Very calm / enclosed water | global | player defaults |
| 960 | Mini Games | 10016 | Azzar Faire MiniGames | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10069 | Bullet Romper | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10070 | Shadow Eye | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10071 | Duck Hunt | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10072 | Rainbow Race | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10073 | Vertical Ascend | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10074 | Kul Tiran Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10075 | Mechagnomic Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10076 | Titanic Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10077 | Volcanic Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10098 | Emerald Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10099 | Naga Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10101 | Dark Iron Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10102 | Zandalari Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10103 | Legion Ascent | 1.00 | Neutral baseline | global | player defaults |
| 960 | Mini Games | 10112 | Wonka Wonka! | 1.00 | Neutral baseline | global | player defaults |
| 961 | Ocean Battle | 10020 | Ocean Battle | 1.30 | Exposed coast / island | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 961 | Ocean Battle | 10030 | Ocean's Sword | 1.25 | Exposed coast / island | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 961 | Ocean Battle | 10031 | Gronn's Spine | 0.75 | Calm inland water | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 965 | Ocean Battle 2 | 10021 | To Northrend | 1.25 | Exposed coast / island | 0x7AA2C0/0x082C46/0xEC9066 | 1.2/0.7/0.95/1.2/1.1 |
| 967 | Inevitable Future | 10022 | Inevitable Future | 0.75 | Calm inland water | global | player defaults |
| 968 | The Abbys | 10068 | AzzarEvent | 0.85 | Calm inland water | global | player defaults |
| 971 | Stormwind Sewer Room | 10155 | [PH] Stormwind Sewer Room | 0.30 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 972 | Orgrimmar Depths | 10162 | Orgrimmar Depths | 0.30 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 980 | Tol'viron Arena | 8457 | Tol'Viron Arena | 0.65 | Sheltered / still water | global | player defaults |
| 980 | Tol'viron Arena | 10055 | Uldum | 0.65 | Sheltered / still water | 0x8FC4C0/0x0D4A52/0xE8946A | 1.55/0.4/1.2/1.15/1.1 |
| 981 | The Robodrome | 10059 | The Robodrome | 0.30 | Very calm / enclosed water | global | player defaults |
| 982 | Coliseum of Past Echoes | 10026 | Plains of Echoes | 0.55 | Sheltered / still water | global | player defaults |
| 983 | Imperial Arena of Thakraj | 10028 | Ahn'kahet: The Old Kingdom | 0.30 | Very calm / enclosed water | global | player defaults |
| 984 | Maldraxxus Coliseum | 10032 | Maldraxxus | 0.55 | Sheltered / still water | global | player defaults |
| 985 | Nagrand Arena | 10038 | Blade's Edge Mountains | 0.65 | Sheltered / still water | global | player defaults |
| 986 | Blade's Edge Arena | 10040 | Nagrand | 0.65 | Sheltered / still water | global | player defaults |
| 987 | Warsong Gulch | 10034 | Warsong Gulch | 0.85 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 988 | Arathi Basin | 10043 | Arathi Basin | 0.95 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 998 | Temple of Kotmogu | 6051 | The Temple of Kotmogu | 0.30 | Very calm / enclosed water | global | player defaults |
| 1007 | Guardian's Hall | 6137 | Guardian's Hall | 0.30 | Very calm / enclosed water | global | player defaults |
| 1008 | Spark of Creator | 6138 | Spark of Creator | 0.30 | Very calm / enclosed water | global | player defaults |
| 1010 | CoaSpellVisuals | 15010 | Spell Dev Map | 1.00 | Neutral baseline | global | player defaults |
| 1134 | The Tiger's Peak | 6732 | The Tiger's Peak | 0.35 | Very calm / enclosed water | global | player defaults |
| 1199 | Instwood | 7006 | Darrowmere Forest | 0.80 | Calm inland water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 1200 | Lordaeron | 7000 | Lordaeron | 0.85 | Calm inland water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 1201 | Haunted Valley | 7012 | Haunted Valley | 0.75 | Calm inland water | 0x5E7268/0x061A16/0xC8788C | 0.65/1.6/0.4/0.5/0.65 |
| 1280 | Southshore vs. Tarren Mill | 7107 | Hillsbrad Foothills | 0.90 | Calm inland water | 0x7FA6BC/0x0B3045/0xE08A64 | 1.25/0.65/1.05/1.05/1.05 |
| 1401 | Baradin Hold Arena | 8462 | Baradin Hold Arena | 0.65 | Sheltered / still water | global | player defaults |
| 1402 | Obelisk of the Stars | 8463 | Obelisk of the Stars | 0.35 | Very calm / enclosed water | global | player defaults |
| 1403 | The Twisting Nether | 4990 | The Twisting Nether | 0.30 | Very calm / enclosed water | global | player defaults |
| 1403 | The Twisting Nether | 10051 | The Twisting Nether | 0.30 | Very calm / enclosed water | global | player defaults |
| 1504 | Black Rook Hold Arena | 8456 | Black Rook Hold Arena | 0.40 | Sheltered / still water | global | player defaults |
| 1504 | Black Rook Hold Arena | 10057 | Black Rook Hold | 0.40 | Sheltered / still water | global | player defaults |
| 1552 | Ashamane's Fall | 8458 | Ashamane's Fall | 0.75 | Calm inland water | global | player defaults |
| 1552 | Ashamane's Fall | 10053 | Val'sharah | 0.75 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 1683 | The Inventor's Library | 8443 | The Inventor's Library | 0.30 | Very calm / enclosed water | global | player defaults |
| 1684 | Amphitheater of Anguish | 8442 | Amphitheater of Anguish | 0.40 | Sheltered / still water | global | player defaults |
| 1731 | Grizlo's Brawlers Guild | 12108 | Grizlo's Brawlers Guild | 0.30 | Very calm / enclosed water | global | player defaults |
| 1758 | The Crucible (Staging) | 6001 | Icecrown | 0.85 | Calm inland water | 0x66849C/0x081E33/0xDC8660 | 1.45/0.4/0.9/1.1/1 |
| 1758 | The Crucible (Staging) | 6002 | The North Sea | 1.45 | Open sea / strongest movement | 0x6E8CA4/0x0A2438/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 1758 | The Crucible (Staging) | 6028 | Hrothgar's Landing | 1.25 | Exposed coast / island | 0x6E8CA4/0x0A2438/0xDC8660 | 1.3/0.55/1/1.15/1.05 |
| 1771 | The Grim Marsh | 11429 | The Grim Marsh | 0.85 | Calm inland water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 1774 | Eonar's Cradle | 11439 | Eonar's Cradle | 0.75 | Calm inland water | 0x74A88C/0x0B2E20/0xC47896 | 1.35/0.55/1.2/1.1/1.2 |
| 1775 | Iskarr Village | 11435 | Iskirr Village | 0.75 | Calm inland water | global | player defaults |
| 1776 | Vardrheim | 11427 | Temple of Eternity | 0.30 | Very calm / enclosed water | global | player defaults |
| 1777 | custom_vrykul2 | 11437 | The Forgotten Isle | 1.15 | Moderate movement | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 1781 | Forgotten Mine | 11430 | Forgotten Mine | 0.30 | Very calm / enclosed water | global | player defaults |
| 1787 | Baradin Hold | 10109 | Baradin Hold | 0.40 | Sheltered / still water | global | player defaults |
| 1807 | Karazhan Crypts | 11440 | The Karazhan Crypts | 0.30 | Very calm / enclosed water | 0x667C46/0x0A1806/0x9C7CC8 | 0.45/2.4/0.2/0.3/0.5 |
| 1808 | Pilgrim's Isle | 3001 | Pilgrim's Isle | 1.15 | Moderate movement | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 1908 | Zanza's Isle | 15011 | Zanza's Isle | 1.15 | Moderate movement | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 1908 | Zanza's Isle | 15012 | Ruins of Atali | 0.65 | Sheltered / still water | 0x6E7A52/0x101C0E/0x9C7CD8 | 0.38/2.5/0.22/0.35/0.55 |
| 1908 | Zanza's Isle | 15013 | Invader's Bay | 1.30 | Exposed coast / island | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 1908 | Zanza's Isle | 15014 | Jin'Vile Overlook | 0.85 | Calm inland water | 0x6E8A76/0x0C2218/0xD07896 | 1/0.9/0.95/0.9/0.95 |
| 1908 | Zanza's Isle | 15016 | Ritual Grounds | 0.35 | Very calm / enclosed water | 0x86B4BC/0x0A3B4D/0xF09268 | 1.8/0.3/1.3/1.25/1.2 |
| 1911 | Akhen City Ruins | 15015 | Akhen City Ruins | 0.45 | Sheltered / still water | global | player defaults |
