#pragma once
#include "WindSystem.h"
#include "VegetationWind.h"
#include <array>
#include <cstdio>
#include <string>
#include <vector>

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
    std::snprintf(text, sizeof(text), "Waves: %s | Clouds: %s | Plants: %s | Rain drift: %s",
        wind.DrivesWaves() ? "linked (sea inertia)" : "legacy",
        wind.DrivesClouds() ? "linked" : "legacy",
        wind.DrivesVegetation() ? "linked" : "legacy", wind.enabled ? "wind" : "off");
    lines[4] = text;
    return lines;
}

// Two extra lines for the vegetation consumer: what is leaning, and which gate
// refused the rest.
//
// The feature's first in-client report was "the HUD says 0 swaying and nothing
// moves", and the reason it said zero was that the gates that rejected every
// draw - the material, the shader, the declaration, the camera, the palette
// bound - were the ones that counted nothing. Both rows are therefore part of
// the feature, not of a debugging session: the second row is the one that says
// WHICH gate is refusing, and a player can read it out loud without a debugger.
//
// Kept separate from the wind lines above so a vegetation-off session reports
// exactly what it did before the feature existed.
inline std::vector<std::string> VegetationDebugLines(const VegetationStatus& status,
    const VegetationSettings& settings, const WindState& wind)
{
    // One short row, never more: the HUD's box is 600 logical pixels wide at a
    // 16 pixel font, and a line that does not fit is a line nobody can read.
    // The regression suite pins the budget.
    char text[224]{};
    std::vector<std::string> lines;
    if (!settings.enabled)
    {
        lines.emplace_back("Plants: off (F7)");
        return lines;
    }
    if (!wind.DrivesVegetation())
    {
        lines.emplace_back("Plants: legacy (wind off or unlinked)");
        return lines;
    }
    std::snprintf(text, sizeof(text), "Plants: %u leaning of %u drew, %u solid | max %.1f deg",
        status.swaying, status.candidates, status.skippedMaterial, double(status.maxAngle));
    lines.emplace_back(text);
    // The size the gate last looked at, then the gates that actually refused
    // something this frame - and only those: a list of twelve zeroes is a line
    // nobody reads, while "pal 21" on its own says the palette bound is the
    // problem in six characters. The order is the order the gates run in, so the
    // first name on the row is the first thing to look at.
    //
    //   decl  the vertex declaration cannot be read (no usable position)
    //   world the constants bind the WMO/terrain projection: scenery, not plants
    //   vs    no bone indices, no captured variant, no M2 projection: unproven
    //   geo   the draw's own vertices could not be read (the palette bound)
    //   depth no colour writes: the client's shadow map or another depth pass
    //   cam   the camera pose is missing, or the draw belongs to another view
    //   pal   the palette does not look like object -> view matrices / is short
    //   shape too many bones, or longer than it is tall
    //   size  outside the supported height range, measured against `h`
    //   move  the client is animating it
    //   far   beyond DistanceYards
    //   new   model vertex shaders this build had not captured (the hashes the
    //         renderer log just named: add the foliage ones to TorchSignatures.h)
    //
    // "solid" on the row above is deliberately not a gate: a frame with
    // something leaning counts one solid draw per trunk it carries.
    //
    // The labels are printed in the order the gates run, so the first name on
    // the row is the first thing to look at, and a gate that refused nothing is
    // not printed at all.
    std::string gates;
    const auto report = [&gates](const char* label, unsigned value) {
        if (!value) return;
        if (!gates.empty()) gates += ' ';
        gates += label;
        gates += ' ';
        gates += std::to_string(value);
    };
    report("decl", status.skippedLayout);
    report("world", status.skippedWorld);
    report("vs", status.skippedShader);
    report("geo", status.skippedGeometry);
    report("depth", status.skippedDepthOnly);
    report("cam", status.skippedCamera + status.skippedViewport);
    report("pal", status.skippedPalette + status.unbounded);
    report("shape", status.skippedShape);
    report("size", status.skippedSize);
    report("move", status.skippedAnimated);
    report("far", status.skippedRange);
    report("new", status.learnedShaders);
    std::snprintf(text, sizeof(text), "Plants gate: h %.1f yd | ", double(status.lastMeasured));
    lines.emplace_back(std::string(text) + (gates.empty() ? "none refused" : gates));
    return lines;
}
}
