#!/usr/bin/env python3
"""Generate/check the water sea-state audit from the shipped database and INI."""
from __future__ import annotations

import argparse
import re
import shlex
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATABASE = ROOT / "data" / "areas.txt"
PROFILES = ROOT / "EnvironmentProfiles.ini"
OUTPUT = ROOT / "docs" / "water-zone-profile-audit.md"


def read_database() -> tuple[dict[int, str], list[dict[str, int | str]]]:
    maps: dict[int, str] = {}
    areas: list[dict[str, int | str]] = []
    for line_no, line in enumerate(DATABASE.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#") or line == "EnvironmentDatabase 1":
            continue
        row = shlex.split(line)
        if row[0] == "M" and len(row) == 3:
            maps[int(row[1])] = row[2]
        elif row[0] == "A" and len(row) == 6:
            areas.append({
                "id": int(row[1]), "map_id": int(row[2]), "parent_id": int(row[3]),
                "flags": int(row[4]), "name": row[5],
            })
        else:
            raise ValueError(f"{DATABASE}:{line_no}: malformed area database row")
    if not maps or not areas:
        raise ValueError("the shipped area database is empty")
    return maps, areas


def read_zone_profiles() -> dict[int, dict[str, str]]:
    profiles: dict[int, dict[str, str]] = {}
    section: str | None = None
    for line_no, raw in enumerate(PROFILES.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("["):
            if not line.endswith("]"):
                raise ValueError(f"{PROFILES}:{line_no}: malformed section")
            section = line[1:-1]
            if section.startswith("Zone."):
                zone_id = int(section[5:])
                if zone_id in profiles:
                    raise ValueError(f"duplicate {section}")
                profiles[zone_id] = {}
            continue
        if "=" not in line:
            raise ValueError(f"{PROFILES}:{line_no}: expected key=value")
        if section and section.startswith("Zone."):
            key, value = (part.strip() for part in line.split("=", 1))
            profiles[int(section[5:])][key] = value
    return profiles


def water_behavior(value: float) -> str:
    if value == 1.0:
        return "Neutral baseline"
    if value <= 0.35:
        return "Very calm / enclosed water"
    if value <= 0.65:
        return "Sheltered / still water"
    if value < 1.0:
        return "Calm inland water"
    if value <= 1.15:
        return "Moderate movement"
    if value <= 1.35:
        return "Exposed coast / island"
    return "Open sea / strongest movement"


def markdown_cell(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ")


def render() -> str:
    maps, areas = read_database()
    profiles = read_zone_profiles()
    roots = [area for area in areas if area["parent_id"] == 0]
    root_by_id = {int(area["id"]): area for area in roots}
    missing = sorted(set(root_by_id) - set(profiles))
    extra = sorted(set(profiles) - set(root_by_id))
    if missing or extra:
        raise ValueError(f"root/profile mismatch: missing={missing[:10]} extra={extra[:10]}")

    by_map: dict[int, list[dict[str, int | str]]] = defaultdict(list)
    for area in roots:
        zone_id = int(area["id"])
        values = profiles[zone_id]
        if "WaterAgitationMultiplier" not in values:
            raise ValueError(f"Zone.{zone_id} has no WaterAgitationMultiplier")
        multiplier = float(values["WaterAgitationMultiplier"])
        if not 0.0 <= multiplier <= 4.0:
            raise ValueError(f"Zone.{zone_id} has an out-of-range multiplier")
        by_map[int(area["map_id"])].append({
            **area, "display_name": values.get("Name", str(area["name"])), "multiplier": multiplier,
        })
    if sum(map(len, by_map.values())) != len(roots):
        raise ValueError("the generated listing omitted one or more root zones")

    lines = [
        "# Water zone profile audit",
        "",
        f"Generated from the authoritative `{DATABASE.relative_to(ROOT)}` and `{PROFILES.relative_to(ROOT)}` files.",
        f"The verified Ascension patch-M tables contain **{len(maps)} maps** and **{len(areas)} area records**, including **{len(roots)} map-root zones**. Every root has a direct `WaterAgitationMultiplier`; other areas retain the existing Map → Zone → Area inheritance order.",
        "",
        "**Scope:** this is Ascension's custom, WotLK 3.3.5-based client table, not the stock Wrath DBC catalog. It includes Ascension map IDs and some expansion-derived names. Their presence in the loaded table makes them addressable by this supported client; it does not prove that the zone is currently playable or has rendered water. A stock 3.3.5 client is outside this renderer's tested target and will not auto-enable the verified Ascension location provider.",
        "",
        "`1.00` is the neutral baseline. Lower values keep enclosed, sheltered or inland water calmer; higher values are reserved for exposed coasts and open seas. Neutral entries avoid inventing extra motion where a map has no known water or is a test/development space. Where a map has no rendered water, its multiplier is inert. These are authoring multipliers, not measurements of a live frame.",
        "",
        "Automatic runtime application requires the exact supported Ascension executable (or another client whose location RVAs and table are explicitly verified). This audit checks every root ID, map association and profile resolution source; it cannot observe a running game process or confirm all zones in-world.",
        "",
        "Regenerate or validate this list with `python3 tools/GenerateWaterZoneAudit.py` or `python3 tools/GenerateWaterZoneAudit.py --check`.",
        "",
        "| Map ID | Map | Zone ID | Root zone | Multiplier | Behavior |",
        "| ---: | --- | ---: | --- | ---: | --- |",
    ]
    for map_id in sorted(by_map):
        map_name = maps.get(map_id)
        if map_name is None:
            raise ValueError(f"map {map_id} is missing from the database")
        for area in sorted(by_map[map_id], key=lambda entry: int(entry["id"])):
            multiplier = float(area["multiplier"])
            lines.append(
                f"| {map_id} | {markdown_cell(map_name)} | {area['id']} | "
                f"{markdown_cell(str(area['display_name']))} | {multiplier:.2f} | {water_behavior(multiplier)} |"
            )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the committed audit is out of date")
    args = parser.parse_args()
    try:
        content = render()
    except (OSError, ValueError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != content:
            print(f"FAIL {OUTPUT.relative_to(ROOT)} is missing or stale; regenerate it with this tool", file=sys.stderr)
            return 1
        print(f"PASS {OUTPUT.relative_to(ROOT)} lists all root zones and matches the shipped profiles")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(content, encoding="utf-8")
    print(f"Wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
