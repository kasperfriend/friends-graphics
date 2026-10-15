#pragma once
#include "GrassShaderWind.h"
#include "WindSystem.h"
#include "VegetationWind.h"
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace renderer
{
// Which build is drawing this HUD, as the loaded d3d9.dll's own file time
// ("2026-10-15 18:32"), published once by the renderer at startup and shown on
// the vegetation rows. Empty when nothing published one - the portable suite
// draws these rows without a DLL behind them, and a row that is not there is
// better than a row that lies.
inline const char* buildStamp = nullptr;

// What fits the HUD's own box: 600 logical pixels at a 16 pixel font, drawn 10
// pixels in from the left and clipped 18 short of the right. A row longer than
// this is not a longer row, it is a row whose end nobody can read - so the gate
// census continues on the next line instead. The regression suite pins both the
// budget and the continuation.
inline constexpr size_t kVegetationHudRowBudget = 72;

// The prefix of a continued gate row. Only ever appears when a frame refuses at
// enough gates that one row cannot hold them all.
inline constexpr const char* kMoreGatesPrefix = "Plants gate (more): ";

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
    if (!settings.swayModels)
    {
        // The whole model path is off, not broken: the grass the client draws as
        // cards is the blade path and reports on its own row.
        char off[96]{};
        std::snprintf(off, sizeof(off), "Trees & bushes: off (F7) | %u model draw(s) left alone",
            status.modelsOff);
        lines.emplace_back(off);
        return lines;
    }
    // "waiting" is the one number that is not a refusal and is not the effect
    // working either: a model that has been measured, is plant-shaped and is
    // standing still, but has not repeated itself in a second frame yet. It is
    // printed only when there is one, and it is what tells "being measured"
    // apart from "nothing happened" in a row that would otherwise read
    // "0 leaning of 40 drew, 0 solid".
    char waiting[24]{};
    if (status.pending) std::snprintf(waiting, sizeof(waiting), ", %u waiting", status.pending);
    std::snprintf(text, sizeof(text), "Plants: %u leaning of %u drew%s, %u solid | max %.1f deg",
        status.swaying, status.candidates, waiting, status.skippedMaterial, double(status.maxAngle));
    lines.emplace_back(text);
    // The size the gate last looked at, then the gates that actually refused
    // something this frame - and only those: a list of fourteen zeroes is a line
    // nobody reads, while "pal 21" on its own says the palette bound is the
    // problem in six characters. The order is the order the gates run in, so the
    // first name on the row is the first thing to look at.
    //
    //   flat  fixed-function T&L: no vertex shader at all, never the model path
    //   decl  the vertex declaration cannot be read (no usable position / bone
    //         indices: not the client's skinned model path)
    //   depth no colour writes: the client's shadow map or another depth pass
    //   never a pass this project identified as never a model (UI, rain, water)
    //   vs    a model draw this build could not prove (expected to stay zero)
    //   cam   the camera pose is missing, or the draw belongs to another view
    //   world world-shaped constants AND no bone block at c31: the world's own
    //         projection, refused once the palette failed to prove otherwise
    //   pal   the palette does not look like object -> view matrices / is short
    //   geo   the draw's own vertices could not be read (the palette bound)
    //   shape a body's worth of bones, or longer than it is tall
    //   size  outside the supported height range, measured against `h`
    //   ground the model hangs from its placement point instead of standing on
    //         it: a cape, a shoulder pad, a sword, a saddle
    //   move  the client is animating it
    //   far   beyond DistanceYards
    //   calm  inside the range, but the wind leaves a zero angle at that spot
    //   write the leaned registers could not be written back
    //   restore the client's own matrices did not come back - this renderer's
    //         write is what deformed a model, whatever the model was
    //   new   model vertex shaders this build had not captured (the hashes the
    //         renderer log just named: add the foliage ones to TorchSignatures.h)
    //
    // "solid" on the row above is deliberately not a gate: a frame with
    // something leaning counts one solid draw per trunk it carries.
    //
    // The labels are printed in the order the gates run, so the first name on
    // the row is the first thing to look at, and a gate that refused nothing is
    // not printed at all. A frame where everything refuses at once does not fit
    // the box, and a clipped row is exactly as useful as the "0" the player
    // reported - so the list continues on the next row, prefixed, rather than
    // being cut off. Both rows together are the whole census, in gate order.
    std::vector<std::pair<const char*, unsigned>> refused;
    const auto report = [&refused](const char* label, unsigned value) {
        if (value) refused.emplace_back(label, value);
    };
    report("flat", status.skippedFixedFunction);
    report("decl", status.skippedLayout);
    report("depth", status.skippedDepthOnly);
    report("never", status.skippedNeverModel);
    report("vs", status.skippedShader);
    report("cam", status.skippedCamera + status.skippedViewport);
    report("world", status.skippedWorld);
    report("pal", status.skippedPalette + status.unbounded);
    report("geo", status.skippedGeometry);
    report("shape", status.skippedShape);
    report("size", status.skippedSize);
    report("tall", status.skippedTall);
    report("ground", status.skippedGround);
    report("move", status.skippedAnimated);
    report("far", status.skippedRange);
    report("calm", status.skippedCalm);
    report("write", status.skippedWrite);
    report("restore", status.restoreMismatch);
    report("new", status.learnedShaders);
    // The head carries the size the gate last measured and the number of draws
    // that arrived as the client's skinned model path at all: "no gate refused
    // anything" and "this client does not draw this field as models" look
    // identical on a row of refusals, and only this number tells them apart.
    std::snprintf(text, sizeof(text), "Plants gate: h %.1f yd, %u models | ", double(status.lastMeasured),
        status.modelDraws);
    if (refused.empty())
        lines.emplace_back(std::string(text) + "none refused");
    else
    {
        std::string row = text;
        bool startsRow = true; // the row already carries the head, so no separator
        for (const auto& [label, value] : refused)
        {
            const std::string entry = std::string(label) + ' ' + std::to_string(value);
            if (!startsRow && row.size() + entry.size() + 1 > kVegetationHudRowBudget)
            {
                lines.emplace_back(row);
                row = kMoreGatesPrefix;
                startsRow = true;
            }
            if (!startsRow) row += ' ';
            row += entry;
            startsRow = false;
        }
        lines.emplace_back(row);
    }
    // And which build drew the row. A report about this feature is a screenshot
    // of these lines, and a screenshot could not say which d3d9.dll drew them -
    // one report arrived with counters that read exactly like the build before
    // it, from a client that had not restarted. The stamp is the loaded DLL's
    // own file time, so it needs no version to keep in step with the source, and
    // a screenshot now names the build it came from.
    if (buildStamp && *buildStamp)
    {
        std::snprintf(text, sizeof(text), "ModernWoWRenderer %s", buildStamp);
        lines.emplace_back(text);
    }
    return lines;
}

// And the grass the client draws as blades, when this build has bent a shader for
// it. Two rows at most, in the same shape as the model rows above:
//
//   blades  draws that went out carrying the wind - the field the player is
//           standing in, counted per draw because that is what is being bent
//   by height  of the draws that went out with the wind, the ones bent by the
//           vertex's own height because the card carries no texture coordinate
//   big   a batch far too large to be a blade card: the world drawn through a
//         shader this build bent for the grass, left exactly as the client drew it
//   shaders how many of the client's shaders this build has rewritten, how many
//           it looked at and left alone, and how far a blade's tip moves
//
// The refusal row only appears when something WAS refused, and it names the
// reason in the same six-character style as the gate row above, because "the
// grass still does not move" has exactly three answers on this path: the client
// does not draw it as blades at all (nothing on the row), the material was not
// foliage (`mat`), or the shader could not be rewritten (`decl` for a card whose
// declaration carries neither a texture coordinate nor a position, so there is no
// height to read a bend weight from, `vs` for one whose coordinate is not where
// the shader reads it, `asm` for a shader that could not be disassembled,
// reassembled or created, `read` for a card whose own vertices could not be read
// at all, `card` for one that was read and lies flat).
//
// One more row, and it is not a refusal: `world` counts shaders that were bent
// and bind the world's own projection. It is the shape the client draws an
// unskinned card with, so it is recorded rather than turned away - a row that
// said `world 376` among the refusals is what a still field looked like.
inline std::vector<std::string> GrassBladeDebugLines(const GrassBladeStatus& status,
    const GrassBladeSettings& settings)
{
    std::vector<std::string> lines;
    if (!settings.enabled) return lines;
    char text[256]{};
    // `by height` is the uv-less family: the cards the client draws with no
    // texture coordinate at all, bent by their own vertex height. It is the field
    // a player's grass usually IS, so it belongs on the row that says how much of
    // the frame is moving rather than among the refusals.
    std::snprintf(text, sizeof(text), "Blades: %u swaying, %u blocks/s, %u by height | %.2f yd at the tip",
        status.bladeDraws, status.uploads, status.heightDraws, double(status.amplitudeYards));
    lines.emplace_back(text);
    std::vector<std::pair<const char*, unsigned>> refused;
    const auto report = [&refused](const char* label, unsigned value) {
        if (value) refused.emplace_back(label, value);
    };
    report("mat", status.materialRefused);
    report("never", status.neverModel);
    report("decl", status.noTexcoord);
    report("big", status.bigDraws);
    report("card", status.cardRefused);
    report("read", status.cardUnreadable);
    report("vs", status.registerMismatch);
    report("asm", status.refused);
    std::snprintf(text, sizeof(text), "Blades gate: %u looked at, %u bent | ", status.examined,
        status.injected);
    if (refused.empty())
        lines.emplace_back(std::string(text) + "none refused");
    else
    {
        std::string row = text;
        bool startsRow = true;
        for (const auto& [label, value] : refused)
        {
            const std::string entry = std::string(label) + ' ' + std::to_string(value);
            if (!startsRow && row.size() + entry.size() + 1 > kVegetationHudRowBudget)
            {
                lines.emplace_back(row);
                row = kMoreGatesPrefix;
                startsRow = true;
            }
            if (!startsRow) row += ' ';
            row += entry;
            startsRow = false;
        }
        lines.emplace_back(row);
    }
    if (status.worldShaped)
    {
        std::snprintf(text, sizeof(text), "Blades world-path: %u bent (unskinned cards are drawn that way)",
            status.worldShaped);
        lines.emplace_back(text);
    }
    return lines;
}
}
