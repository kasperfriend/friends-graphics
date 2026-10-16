#pragma once
#include "WindSystem.h"
#include <cstdio>
#include <string>
#include <vector>

namespace renderer
{
// Which build is drawing this HUD, as the loaded d3d9.dll's own file time
// ("2026-10-15 18:32"), published once by the renderer at startup and stamped on
// the last row. Empty when nothing published one - the portable suite draws these
// rows without a DLL behind them, and a row that is not there is better than a row
// that lies. A report about this renderer is a screenshot of these rows, and
// without the stamp a screenshot could not say which build drew it.
inline const char* buildStamp = nullptr;

// The wind's own diagnostic text: five rows of the simulated state, plus the build
// stamp when one was published. Diagnostic text only; the wind simulation itself
// creates no geometry, particles, noise textures or render targets, and no row of
// this HUD changes anything it reports on.
inline std::vector<std::string> WindDebugLines(const WindState& wind)
{
    std::vector<std::string> lines;
    lines.reserve(6);
    char text[192]{};
    std::snprintf(text, sizeof(text), "WIND %s | %s | compass bearing is FROM (N=+X, E=-Y)",
        wind.enabled ? "ON" : "OFF / legacy motion", wind.dynamic ? "dynamic" : "fixed direction");
    lines.emplace_back(text);
    std::snprintf(text, sizeof(text), "From %s %.1f deg | target from %s | next turn %.1f s",
        WindDirectionName(NearestWindDirection(wind.headingDegrees)), double(wind.headingDegrees),
        WindDirectionName(wind.targetDirection), double(wind.secondsToChange));
    lines.emplace_back(text);
    std::snprintf(text, sizeof(text), "Strength %.1f%% | target %.1f%% | speed %.2f yd/s",
        double(wind.strength * 100.f), double(wind.targetStrength * 100.f), double(wind.strength * kWindReferenceSpeed));
    lines.emplace_back(text);
    std::snprintf(text, sizeof(text), "World velocity XY: %.2f, %.2f | weather %s %.0f%%",
        double(wind.velocity.x), double(wind.velocity.y),
        wind.weather.kind == 1 ? "rain" : wind.weather.kind == 2 ? "snow" : wind.weather.kind == 3 ? "sand" : "clear",
        double(wind.weather.strength * 100.f));
    lines.emplace_back(text);
    std::snprintf(text, sizeof(text), "Waves: %s | Clouds: %s | Rain drift: %s",
        wind.DrivesWaves() ? "linked (sea inertia)" : "legacy",
        wind.DrivesClouds() ? "linked" : "legacy", wind.enabled ? "wind" : "off");
    lines.emplace_back(text);
    if (buildStamp && *buildStamp)
    {
        std::snprintf(text, sizeof(text), "ModernWoWRenderer %s", buildStamp);
        lines.emplace_back(text);
    }
    return lines;
}
}
