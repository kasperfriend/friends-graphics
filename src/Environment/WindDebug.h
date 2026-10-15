#pragma once
#include "WindSystem.h"
#include <cstdio>
#include <string>

namespace renderer
{
// Diagnostic text only; the wind simulation itself creates no geometry,
// particles, noise textures or render targets.
inline std::array<std::string, 5> WindDebugLines(const WindState& wind)
{
    std::array<std::string, 5> lines;
    char text[192]{};
    std::snprintf(text, sizeof(text), "WIND %s | %s | compass bearing is FROM (N=+X, E=-Y)",
        wind.enabled ? "ON" : "OFF / legacy motion", wind.dynamic ? "dynamic" : "fixed direction");
    lines[0] = text;
    std::snprintf(text, sizeof(text), "From %s %.1f deg | target from %s | next turn %.1f s",
        WindDirectionName(NearestWindDirection(wind.headingDegrees)), double(wind.headingDegrees),
        WindDirectionName(wind.targetDirection), double(wind.secondsToChange));
    lines[1] = text;
    std::snprintf(text, sizeof(text), "Strength %.1f%% | target %.1f%% | speed %.2f yd/s",
        double(wind.strength * 100.f), double(wind.targetStrength * 100.f), double(wind.strength * kWindReferenceSpeed));
    lines[2] = text;
    std::snprintf(text, sizeof(text), "World velocity XY: %.2f, %.2f | weather %s %.0f%%",
        double(wind.velocity.x), double(wind.velocity.y),
        wind.weather.kind == 1 ? "rain" : wind.weather.kind == 2 ? "snow" : wind.weather.kind == 3 ? "sand" : "clear",
        double(wind.weather.strength * 100.f));
    lines[3] = text;
    std::snprintf(text, sizeof(text), "Waves: %s | Clouds: %s | Rain drift: %s",
        wind.DrivesWaves() ? "linked (sea inertia)" : "legacy",
        wind.DrivesClouds() ? "linked" : "legacy", wind.enabled ? "wind" : "off");
    lines[4] = text;
    return lines;
}
}
