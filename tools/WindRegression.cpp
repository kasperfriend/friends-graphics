// Run from repository root. No SDK needed on Linux; Windows additionally
// exercises the real location-scoped INI read/save/reload path.
#include "../src/Environment/WindTuning.h"
#include "../src/Environment/WindDebug.h"
#include "../src/Environment/OverlayLayout.h"
#include "../src/Effects/CloudDynamics.h"
#include "../src/Effects/PrecipitationMotion.h"
#include "../src/Water/WaveMotion.h"
#include "../src/Water/WaterNoise.h"
#include "../src/Water/WaterSeaState.h"
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#include "../src/Environment/LocationTuning.h"
#endif

using namespace renderer;
using namespace renderer::water;
// A hand edit (the profile API called directly, as an editor or a hand edit
// would) is detected by the cached read's file-identity check: write time plus
// size. In a live session such an edit settles long before the next stat; a
// value of the same length written microseconds before a stat is the one
// combination the identity check cannot see, so a hand edit in this test lets
// the file settle before the renderer looks at it again - the timing a real
// player's edit has, not a same-length rewrite inside one instruction.
#if defined(_WIN32)
// The two halves of the identity the cached read compares.
struct HandEditStamp { ULONGLONG written = 0; DWORD bytes = 0; };
static HandEditStamp StampOf(const std::wstring& path) {
    HandEditStamp stamp{};
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        stamp.written = (static_cast<ULONGLONG>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                        info.ftLastWriteTime.dwLowDateTime;
        stamp.bytes = info.nFileSizeLow;
    }
    return stamp;
}
// Two of the edits below change a value without changing its length - "2" to "7"
// is one byte either way - so the size half of the identity cannot reveal them
// and the timestamp is the only signal left. That signal is weaker than it looks:
// the stamp a write gets comes from the system's cached idea of the current time,
// which moves in ticks of roughly 15 ms, so a write made microseconds after an
// earlier one to the same file can be stamped identically to it. The sequence
// below is exactly that - a save, some reads, then a hand edit, all inside one
// tick - and the result is a file whose identity has not moved at all, which the
// cache answers from what it already holds. That is the one combination the
// identity check cannot see, and it is why these checks flaked rather than failed.
//
// So age the file out of the tick it is currently in before editing it. Two ticks
// is enough to be sure the edit's own stamp differs from the one already recorded,
// whatever the platform's granularity turns out to be.
static void SettleBeforeHandEdit() { Sleep(40); }

// Then wait until the edit is actually visible in the file's identity, rather than
// for a fixed interval that assumes it will be by then.
//
// The 25 ms floor stays. The cache re-measures a file at most every 10 ms, so an
// edit landing inside that window is not looked for yet whichever way the
// timestamp behaves, and the checks below are about the read answering the edit
// rather than about the stat race.
//
// The ceiling is what turns an edit that never becomes visible into a failure of
// the check that depends on it, after long enough that it cannot be a busy
// machine - and the failure then says something true about the identity check
// rather than about the runner.
static void SettleHandEdit(const std::wstring& path, const HandEditStamp& before) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const HandEditStamp now = StampOf(path);
        const bool moved = now.written != before.written || now.bytes != before.bytes;
        const ULONGLONG elapsed = GetTickCount64() - start;
        if ((moved && elapsed >= 25) || elapsed >= 5000) return;
        Sleep(5);
    }
}
#endif
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void Pass(const char* message) { std::printf("PASS %s\n", message); }
static bool Near(double a, double b, double e = 1e-5) { return std::abs(a - b) <= e; }
static double PeriodDistance(double a, double b, double period) { return std::abs(std::remainder(a - b, period)); }
static std::string Read(const char* path)
{
    std::ifstream file(path);
    Check(bool(file), path);
    return {std::istreambuf_iterator<char>(file), {}};
}
static WindSettings Steady(WindDirection direction = WindDirection::NorthEast, float strength = 1.f)
{
    WindSettings s; s.dynamic = false; s.direction = direction; s.strength = strength;
    return s;
}
static WindState Air(WindDirection direction, float strength = 1.f)
{
    WindSystem system; system.Configure(Steady(direction, strength));
    return system.Update({}, 0);
}
static void FiniteState(const WindState& state)
{
    Check(std::isfinite(state.strength) && state.strength >= 0 && state.strength <= 3, "wind strength range");
    Check(std::isfinite(state.targetStrength) && state.targetStrength >= 0 && state.targetStrength <= 3, "wind target range");
    Check(std::isfinite(state.headingDegrees) && state.headingDegrees >= 0 && state.headingDegrees < 360, "wind heading range");
    Check(Near(std::hypot(state.direction.x, state.direction.y), 1, 2e-7), "wind direction stays normalized");
    Check(Near(std::hypot(state.velocity.x, state.velocity.y), state.strength * kWindReferenceSpeed, 1e-5), "wind velocity magnitude");
    Check(std::isfinite(state.deltaSeconds) && state.deltaSeconds >= 0 && state.deltaSeconds <= .1f, "bounded wind delta");
}
static void SamePhases(const WaveField& a, const WaveField& b, double epsilon)
{
    for (int i = 0; i < kTrainCount; ++i)
        Check(PeriodDistance(a.swellPhase[i], b.swellPhase[i], kTrainCells[i]) < epsilon, "swell phase continuity");
    for (int i = 0; i < 2; ++i)
    {
        Check(PeriodDistance(a.ripplePhase[i], b.ripplePhase[i], kLatticeWrap) < epsilon, "ripple phase continuity");
        Check(PeriodDistance(a.warpPhase[i], b.warpPhase[i], kLatticeWrap) < epsilon, "warp phase continuity");
        Check(PeriodDistance(a.groupPhase[i], b.groupPhase[i], kLatticeWrap) < epsilon, "group phase continuity");
        Check(PeriodDistance(a.uvDrift[i], b.uvDrift[i], 1) < epsilon, "native detail phase continuity");
    }
}
static void TestDirectionsAndTime()
{
    const WindVector expected[] = {{-1,0},{-.70710678f,.70710678f},{0,1},{.70710678f,.70710678f},
                                  {1,0},{.70710678f,-.70710678f},{0,-1},{-.70710678f,-.70710678f}};
    for (unsigned i = 0; i < 8; ++i)
    {
        const auto state = Air(static_cast<WindDirection>(i), 1.25f);
        FiniteState(state);
        Check(state.targetDirection == static_cast<WindDirection>(i), "all eight targets selectable");
        Check(Near(state.direction.x, expected[i].x) && Near(state.direction.y, expected[i].y), "downwind travel is opposite the selected source bearing");
        Check(Near(-state.direction.x, kWindFromDirections[i].x) && Near(-state.direction.y, kWindFromDirections[i].y), "source bearings use WoW map axes (+X north, +Y west)");
        Check(Near(state.headingDegrees, i * 45.f), "source bearing degrees clockwise from map north");
        Check(NearestWindDirection(state.headingDegrees) == state.targetDirection, "debug compass matches state");
        Check(Near(state.strength, 1.25), "authored strength");
    }
    Check(NearestWindDirection(359.9f) == WindDirection::North && NearestWindDirection(22.4f) == WindDirection::North &&
          NearestWindDirection(22.5f) == WindDirection::NorthEast, "nearest-sector seam");
    WindSystem turns; turns.Configure(Steady(WindDirection::NorthWest)); turns.Update({}, 0);
    turns.Configure(Steady(WindDirection::North));
    Check(turns.Update({}, .1f).headingDegrees > 315.f, "NW to N turns across 360 seam, not the long way");
    WindSystem reverse; reverse.Configure(Steady(WindDirection::North)); reverse.Update({}, 0);
    reverse.Configure(Steady(WindDirection::NorthWest));
    Check(reverse.Update({}, .1f).headingDegrees > 359.f, "N to NW turns backwards across seam");
    Pass("eight map-referenced wind-from bearings, downwind vectors, normalized travel and shortest-arc turns");

    WindSystem system;
    unsigned visited = 0;
    float low = 10, high = 0;
    auto previous = system.Update({}, 0);
    for (int i = 0; i < 36000; ++i)
    {
        const auto current = system.Update({}, .1f);
        FiniteState(current);
        Check(PeriodDistance(current.headingDegrees, previous.headingDegrees, 360) <= .1501, "no step at direction changes");
        Check(std::abs(current.strength - previous.strength) < .01f, "continuous gust strength");
        visited |= 1u << unsigned(current.targetDirection);
        low = std::min(low, current.strength); high = std::max(high, current.strength);
        previous = current;
    }
    Check(visited == 255, "dynamic wind eventually visits all eight sectors");
    Check(high - low > .15f && low < 1.f && high > 1.f, "strength both rises and falls with time");
    auto reload = system;
    reload.Configure(reload.Settings());
    const auto a = system.Update({}, .016f), b = reload.Update({}, .016f);
    Check(a.headingDegrees == b.headingDegrees && a.strength == b.strength && a.secondsToChange == b.secondsToChange,
          "identical F7/F12 reload preserves direction, gust and schedule");
    auto shortPause = system, longPause = system;
    const auto c = shortPause.Update({}, .1f), d = longPause.Update({}, 1000000.f);
    Check(c.headingDegrees == d.headingDegrees && c.strength == d.strength, "long stalls take only one bounded step");
    const auto before = system.State();
    for (float dt : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        const auto after = system.Update({}, dt);
        Check(before.headingDegrees == after.headingDegrees && before.strength == after.strength &&
              before.secondsToChange == after.secondsToChange, "invalid/stalled clock cannot move wind");
    }
    auto settings = system.Settings(); settings.enabled = false; system.Configure(settings);
    const auto disabled = system.Update({3, 1}, .1f);
    Check(disabled.strength == 0 && disabled.velocity.x == 0 && disabled.velocity.y == 0 &&
          !disabled.DrivesClouds() && !disabled.DrivesWaves(), "disabled wind publishes no influence");
    for (int i = 0; i < 600; ++i) system.Update({}, .1f);
    settings.enabled = true; system.Configure(settings);
    const auto resumed = system.Update({}, 0);
    Check(resumed.headingDegrees == before.headingDegrees && resumed.strength == before.strength &&
          resumed.secondsToChange == before.secondsToChange, "re-enable preserves simulation, not a new random wind");
    settings.affectWaves = false; settings.affectClouds = false; settings.debugView = true;
    system.Configure(settings);
    const auto unlinked = system.Update({}, 0);
    Check(unlinked.strength == resumed.strength && unlinked.headingDegrees == resumed.headingDegrees &&
          !unlinked.DrivesWaves() && !unlinked.DrivesClouds() && unlinked.debugView,
          "link/debug edits do not reset or perturb wind");
    Pass("dynamic rise/fall, all-sector scheduling, reload/disable continuity and stall/NaN safety");

    auto simulated = [](int fps) {
        WindSystem wind; wind.Update({}, 0);
        for (int i = 0; i < fps * 365; ++i)
            wind.Update(i >= fps * 60 && i < fps * 240 ? WindWeather{1,1} : WindWeather{}, 1.f / fps);
        return wind.State();
    };
    const auto reference = simulated(60);
    for (int fps : {30, 144, 240})
    {
        const auto compared = simulated(fps);
        Check(PeriodDistance(reference.headingDegrees, compared.headingDegrees, 360) < .07, "frame-rate independent turning");
        Check(Near(reference.strength, compared.strength, .001), "frame-rate independent strength");
        Check(reference.targetDirection == compared.targetDirection, "frame-rate independent target schedule");
    }
    Pass("30/60/144/240 Hz produce the same wind evolution within integration tolerance");
}
static void TestWeather()
{
    Check(ResolveWindWeather(0, 2, 4).strength == 0 && ResolveWindWeather(9, 1, 1).kind == 0, "unknown weather is clear");
    Check(Near(ResolveWindWeather(1, 1.5f, .5f).strength, .75) && ResolveWindWeather(1, 2, 4).strength == 1,
          "zone intensity is applied before one common clamp");
    Check(ResolveWindWeather(1, NAN, 1).strength == 0 && ResolveWindWeather(1, -3, 1).strength == 0, "invalid intensity is calm");
    for (const auto& [kind, target] : {std::pair{1,1.8f}, {2,1.4f}, {3,2.1f}})
    {
        WindSystem wind; wind.Configure(Steady()); wind.Update({}, 0);
        const float first = wind.Update({kind, 1}, .1f).strength;
        Check(first > 1 && first < 1.02f, "weather builds, never steps");
        for (int i = 0; i < 2400; ++i) wind.Update({kind, 1}, .1f);
        Check(Near(wind.State().strength, target, 1e-5), "weather type response");
        const float clearing = wind.Update({}, .1f).strength;
        Check(clearing < target && clearing > target - .02f, "weather eases down on clearing");
        for (int i = 0; i < 3000; ++i) wind.Update({}, .1f);
        Check(Near(wind.State().strength, 1, 1e-5), "clearing returns to baseline");
    }
    WindSystem still; still.Configure(Steady(WindDirection::East, 0));
    Check(still.Update({3, 1}, .1f).strength == 0, "zero base is calm even in sandstorm");
    WindSettings noWeather = Steady(); noWeather.weatherResponse = 0;
    still.Configure(noWeather);
    for (int i = 0; i < 3000; ++i) still.Update({3, 1}, .1f);
    Check(Near(still.State().strength, 1, 1e-5), "weather-response opt-out");
    WindSettings corrupt; corrupt.strength = INFINITY; corrupt.gustAmount = NAN; corrupt.gustSeconds = 0;
    corrupt.directionSeconds = -50; corrupt.transitionSeconds = NAN; corrupt.weatherResponse = 9999;
    still.Configure(corrupt);
    for (int i = 0; i < 1000; ++i) FiniteState(still.Update({3, INFINITY}, .1f));
    auto strong = Steady(WindDirection::West, 2); strong.weatherResponse = 2;
    WindSystem cap; cap.Configure(strong);
    Check(cap.Update({3,1}, 0).strength == 3, "maximum storm strength is bounded");
    Pass("native rain/snow/sand, clearing, zero strength, weather opt-out and corrupt-input limits");
}
static void TestCloudsAndSea()
{
    for (unsigned i = 0; i < 8; ++i)
    {
        const auto wind = Air(static_cast<WindDirection>(i), 1.5f);
        CloudShape base; base.evolutionSpeed = 0;
        const auto driven = ApplyCloudWind(base, wind);
        Check(Near(driven.windSpeed, 39) && Near(driven.windShear, base.windShear * 1.5f), "cloud strength coupling");
        CloudMotion motion; constexpr float dt = .025f; motion.Advance(driven, dt);
        const float direction[] = {wind.direction.x, wind.direction.y};
        for (int axis = 0; axis < 2; ++axis)
        {
            Check(Near(std::remainder(motion.shape[axis], 1.0) * driven.featureSize / dt,
                       -direction[axis] * driven.windSpeed, 1e-4), "cloud body travels downwind, not sample-offset direction");
            Check(Near(std::remainder(motion.detail[axis], 1.0) * driven.detailSize / dt,
                       -direction[axis] * driven.windSpeed * 1.7, 1e-4), "cloud detail follows the same wind");
            Check(Near(std::remainder(motion.weather[axis], 1.0) * driven.weatherSize / dt,
                       -direction[axis] * driven.windSpeed * .35, 1e-4), "cloud weather map follows the same wind");
        }
        CloudDynamics live;
        const auto resolved = live.Update(base, {}, false, CloudWeather::Clear, 0, 12, dt, wind);
        Check(Near(resolved.windAngle, driven.windAngle) && Near(resolved.windSpeed, driven.windSpeed), "cloud dynamics uses shared snapshot");
        Check(live.Motion().shape == motion.shape, "cloud density/shadow field shares one integrated offset");
        auto unlinked = wind; unlinked.affectClouds = false;
        const auto fallback = ApplyCloudWind(base, unlinked);
        Check(fallback.windSpeed == base.windSpeed && fallback.windAngle == base.windAngle, "cloud opt-out restores legacy authoring");
    }
    const auto calm = ApplyCloudWind(CloudShape{}, Air(WindDirection::North, 0));
    CloudMotion evolving; evolving.Advance(calm, 1.f);
    Check(calm.windSpeed == 0 && calm.windShear == 0 && evolving.shape[0] == 0 && evolving.shapeZ > 0,
          "no-wind cloud drift stops while independent evolution remains");
    CloudShape frozen; frozen.evolutionSpeed = 0;
    auto motionShape = ApplyCloudWind(frozen, Air(WindDirection::North));
    CloudMotion motion; motion.Advance(motionShape, 1e6f);
    const auto before = motion;
    motionShape = ApplyCloudWind(frozen, Air(WindDirection::South, 2));
    motion.Advance(motionShape, 0);
    Check(before.shape == motion.shape && before.weather == motion.weather, "wind edits never rephase clouds from session age");
    Pass("all eight cloud travel signs, strength/shear, shared density offsets and independent evolution");

    SeaTuning tuning; tuning.sharedWind = true;
    const auto normal = TargetSeaState(tuning, {});
    Check(normal.agitation == 1 && normal.motion == 1 && normal.wind == 1, "reference wind is neutral sea state");
    tuning.windStrength = 0; const auto flat = TargetSeaState(tuning, {3,1});
    tuning.windStrength = 2; const auto storm = TargetSeaState(tuning, {1,1});
    const auto sameAir = TargetSeaState(tuning, {});
    Check(storm.agitation == sameAir.agitation && storm.motion == sameAir.motion && storm.wind == sameAir.wind,
          "water never applies weather twice to shared wind");
    Check(flat.agitation < normal.agitation && flat.motion < normal.motion && flat.wind == 0 && flat.agitation > 0,
          "calm air leaves residual swell, not wind foam");
    Check(storm.agitation > normal.agitation && storm.motion > normal.motion && storm.shear > normal.shear,
          "stronger air roughens and quickens the sea");
    tuning.zoneAgitation = .45f; const auto sheltered = TargetSeaState(tuning, {});
    tuning.zoneAgitation = 1.45f; const auto exposed = TargetSeaState(tuning, {});
    Check(sheltered.agitation < exposed.agitation, "shared wind preserves zone exposure");
    tuning.response = 0; const auto noResponse = TargetSeaState(tuning, {1,1});
    Check(noResponse.agitation == 1.45f, "per-water response zero preserves authored energy");
    tuning.windStrength = 0; const auto stillAir = TargetSeaState(tuning, {1,1});
    Check(stillAir.agitation == noResponse.agitation && stillAir.wind == 0,
          "sea energy response off must not invent wind-driven foam in calm air");
    tuning.windStrength = 2; const auto blownAir = TargetSeaState(tuning, {1,1});
    Check(blownAir.agitation == stillAir.agitation && blownAir.wind > stillAir.wind,
          "foam still follows the actual wind when sea energy response is off");
    tuning.authoredWind = 0; Check(TargetSeaState(tuning, {}).wind == 0, "water's foam bias zero still wins");
    tuning.windStrength = NAN; tuning.response = INFINITY; tuning.zoneAgitation = NAN;
    const auto safe = TargetSeaState(tuning, {1,1});
    Check(std::isfinite(safe.agitation) && std::isfinite(safe.motion), "corrupt wind cannot poison sea constants");
    SeaStateFilter filter; filter.Observe(SeaTuning{}, {}, 0, 14);
    Check(std::isfinite(filter.Observe(tuning, {1,1}, .1f, NAN).agitation), "invalid sea response time cannot poison wind coupling");
    Pass("wave energy/motion/foam coupling, local exposure, zero controls and no doubled storm response");
}
static void TestPrecipitationMotion()
{
    for (unsigned direction = 0; direction < 8; ++direction)
    {
        const auto wind = Air(static_cast<WindDirection>(direction));
        PrecipitationMotion motion;
        motion.Advance(.1f, 1.f, wind, {1.f, 0.f}, {0.f, 1.f}, true);
        const auto phase = motion.Phase();
        Check(Near(phase.fall, .1f) && Near(phase.windX, wind.direction.x * .035f) &&
              Near(phase.windY, -wind.direction.y * .007f),
              "all compass headings project downwind onto camera-right and screen-down");
        PrecipitationMotion turnedCamera;
        turnedCamera.Advance(.1f, 1.f, wind, {0.f, -1.f}, {1.f, 0.f}, true);
        Check(Near(turnedCamera.Phase().windX, -wind.direction.y * .035f) &&
              Near(turnedCamera.Phase().windY, -wind.direction.x * .007f),
              "camera turns rotate precipitation drift, not world wind or cloud/wave headings");
    }
    PrecipitationMotion motion;
    const auto east = Air(WindDirection::East);
    for (int i = 0; i < 2000; ++i)
        motion.Advance(.1f, 1.f, east, {1.f, 0.f}, {0.f, 1.f}, true);
    const auto before = motion.Phase();
    motion.Advance(0, 4.f, Air(WindDirection::West, 3.f), {0.f, -1.f}, {1.f, 0.f}, true);
    Check(motion.Phase().fall == before.fall && motion.Phase().windX == before.windX &&
          motion.Phase().windY == before.windY, "speed, heading and camera edits do not multiply session age");
    motion.Advance(.1f, 4.f, Air(WindDirection::West, 3.f), {0.f, -1.f}, {1.f, 0.f}, true);
    Check(Near(motion.Phase().fall - before.fall, .4f, 2e-5) &&
          std::abs(motion.Phase().windX - before.windX) <= .421f,
          "new speed and direction only change future precipitation motion");
    PrecipitationMotion noView, calm;
    noView.Advance(.1f, 1.f, east, {1.f, 0.f}, {0.f, 1.f}, false);
    calm.Advance(.1f, 1.f, Air(WindDirection::East, 0), {1.f, 0.f}, {0.f, 1.f}, true);
    Check(noView.Phase().windX == 0 && calm.Phase().windX == 0 &&
          noView.Phase().fall == calm.Phase().fall && calm.Phase().fall > 0,
          "missing view or calm wind keeps gravity, without an invented screen-space heading");
    const auto previous = motion.Phase();
    motion.Advance(INFINITY, NAN, east, {NAN, 0.f}, {0.f, 1.f}, true);
    Check(motion.Phase().fall == previous.fall && motion.Phase().windX == previous.windX,
          "invalid delta, speed or camera cannot corrupt continuity phases");
    auto disabled = east; disabled.enabled = false;
    motion.Advance(.1f, 1.f, disabled, {1.f, 0.f}, {0.f, 1.f}, true);
    Check(motion.Phase().windX == previous.windX &&
          Near(motion.Phase().fall - previous.fall, .1f, 2e-5),
          "turning shared wind off removes added drift without freezing rain");
    // The phase split (the long-session banding fix): after a full six-hour
    // integration, what the pixel stage receives must still be small - the
    // fraction in [0,1), the wrapped integer in [0,period) - and it must
    // reconstruct the true phase. Either range broken is the float32
    // cancellation the stage exists to avoid.
    {
        PrecipitationMotion session;
        const auto east = Air(WindDirection::East);
        for (int i = 0; i < 6*3600*60; ++i)
            session.Advance(1.f/60.f, 1.2f, east, {1.f, 0.f}, {0.f, 1.f}, true);
        const double fall = session.Fall(), wx = session.WindX(), wy = session.WindY();
        Check(fall > 1000.0, "the six-hour session actually accumulated phase");
        const auto split = ComputeWrappedPhases(fall, wx, wy, 0);
        const double speed[3] = { 1.25, 1.0, 0.75 };
        const double periodX[3] = { 32768.0, 32768.0, 32768.0 };
        const double periodY[3] = { 32768.0, 16384.0, 8192.0 };
        for (int i = 0; i < 3; ++i) {
            const double px = 64.0*speed[i]*wx;
            const double py = 16.0*speed[i]*(1.2*fall + wy);
            const float* layer = i == 0 ? split.c6 : i == 1 ? split.c7 : split.c8;
            for (int a = 0; a < 2; ++a) {
                const float fv = layer[a*2], nv = layer[a*2+1];
                const double phase = a ? py : px;
                const double period = a ? periodY[i] : periodX[i];
                Check(fv >= 0.f && fv < 1.f,
                      "the split's fraction stays in [0,1) after six hours");
                Check(nv >= 0.f && nv < (float)period,
                      "the split's integer part stays wrapped after six hours");
                const double k = std::round((phase - (double(nv) + double(fv))) / period);
                Check(std::fabs(phase - (double(nv) + double(fv) + k*period)) < 1e-6,
                      "the wrapped phase reconstructs the true phase");
            }
        }
        // The period tables must stay in lockstep with the constants the
        // stage spells into its hash.
        Check(precipitationlayers::rainPeriodX[0] == 32768.f &&
              precipitationlayers::rainPeriodY[1] == 16384.f &&
              precipitationlayers::rainPeriodY[2] == 8192.f,
              "the rain periods match the stage's hard-coded hash periods");
        // The wobble and mist lanes stay bounded in every mode.
        for (int mode = 0; mode <= 3; ++mode) {
            const auto m = ComputeWrappedPhases(fall, wx, wy, mode);
            Check(m.c9[0] >= 0.f && m.c9[0] < 6.29f && m.c9[3] >= 0.f && m.c9[3] < 6.29f,
                  "the wobble arguments stay 2pi-wrapped");
            Check(m.c10[0] >= 0.f && m.c10[0] < 1.f && m.c10[1] >= 0.f && m.c10[1] < 1.f,
                  "the mist drift keeps its decimal part only");
        }
    }
    Pass("precipitation follows the shared wind in camera space without duplicate angle/speed clocks");
}
static void TestWaveMotion()
{
    constexpr float dt = 1.f / 60;
    WaveMotion startup;
    startup.Advance(0, 0, .32f, 25, Air(WindDirection::West), 0, 0, false, true);
    Check(startup.ScalePercent() == 25 && Near(startup.Build(0, 0, false, true).lattice[3], 0),
          "saved size/direction is primed before the first draw, without a camera");
    const auto initial = startup.Build(0, 0, true, true);
    startup.Advance(.1f, 1, .32f, 25, Air(WindDirection::East, 0), 40000, -40000, true, true);
    Check(startup.Build(0, 0, true, true).lattice[1] == initial.lattice[1], "zero-strength wind cannot steer residual swell");
    for (unsigned direction = 0; direction < 8; ++direction)
    {
        const auto wind = Air(static_cast<WindDirection>(direction));
        WaveMotion motion; motion.Advance(0, 1, 0, 100, wind, 42000, -38000, true, true);
        const auto before = motion.Build(42000, -38000, true, true);
        motion.Advance(dt, 1, 0, 100, wind, 42000, -38000, true, true);
        const auto after = motion.Build(42000, -38000, true, true);
        Check(Near(-before.lattice[0] * kRippleCellYards, wind.direction.x) &&
              Near(-before.lattice[1] * kRippleCellYards, wind.direction.y), "dominant wave travels downwind, away from selected source bearing");
        const double yards = PhaseSpeedYards(kTrainCells[0] * kRippleCellYards) * double(dt);
        const double moved = (wind.direction.x * before.lattice[0] + wind.direction.y * before.lattice[1]) * yards;
        Check(PeriodDistance(before.swellPhase[0], after.swellPhase[0] + moved, kTrainCells[0]) < 2e-5,
              "moving downwind at dispersion speed follows the same crest");
    }
    for (float scale : {25.f, 100.f, 400.f})
    {
        WaveMotion motion;
        for (int i = 0; i < 2000; ++i) motion.Advance(.05f, .7f, .43f, scale, {}, 12000, -9000, true, true);
        const auto integrated = motion.Build(12000, -9000, true, true);
        const auto legacy = BuildWaveField(double(.05f) * 2000, .7f, .43f, scale, 12000, -9000, true, true);
        SamePhases(integrated, legacy, .003);
    }
    Pass("waves travel downwind from all eight source bearings; constant unlinked motion matches legacy field");

    const auto startWind = Air(WindDirection::NorthEast), turnedWind = Air(WindDirection::SouthWest);
    for (float pivot : {-50000.f, 0.f, 51000.f})
    {
        WaveMotion motion; motion.Advance(.1f, 1, .32f, 100, startWind, pivot, -pivot, true, true);
        for (int i = 0; i < 1000; ++i) motion.Advance(.1f, 1, .32f, 100, startWind, pivot, -pivot, true, true);
        auto control = motion;
        control.Advance(dt, 1, .32f, 100, startWind, pivot, -pivot, true, true);
        motion.Advance(dt, 1, .32f, 100, turnedWind, pivot, -pivot, true, true);
        const auto expected = control.Build(pivot, -pivot, true, true);
        const auto turned = motion.Build(pivot, -pivot, true, true);
        SamePhases(expected, turned, 2e-5);
        Check(turned.lattice[1] != expected.lattice[1], "wind really changes wave orientation");
        const auto beforeEdit = motion.Build(pivot, -pivot, true, true);
        motion.Advance(0, 9, 1, 100, turnedWind, pivot, -pivot, true, true);
        SamePhases(beforeEdit, motion.Build(pivot, -pivot, true, true), 1e-6);
        // Scale changes retain the local phase, including the low-frequency
        // warp and the grouping envelope, instead of jumping from elapsed time.
        motion.Advance(0, 9, 1, 25, turnedWind, pivot, -pivot, true, true);
        SamePhases(beforeEdit, motion.Build(pivot, -pivot, true, true), 2e-5);
    }
    Pass("direction/size changes re-anchor locally even at continent coordinates; no world-origin sweep");

    WaveMotion motion;
    motion.Advance(.1f, 1, .32f, 100, startWind, 4213, -7781, true, true);
    for (int i = 0; i < 1000; ++i) motion.Advance(.1f, 1, .32f, 100, turnedWind, 4213, -7781, true, true);
    const auto world = motion.Build(0, 0, true, true);
    for (float camera : {-46000.f, 0.f, 530.f, 49000.f})
    {
        const auto field = motion.Build(camera, camera * .5f, true, true);
        const float x = (4213 - camera) * field.lattice[0] + (-7781 - camera * .5f) * field.lattice[1];
        const float y = (4213 - camera) * field.lattice[2] + (-7781 - camera * .5f) * field.lattice[3];
        const float wx = 4213 * world.lattice[0] - 7781 * world.lattice[1];
        const float wy = 4213 * world.lattice[2] - 7781 * world.lattice[3];
        const auto warped = WarpWavePlane(field, x, y), reference = WarpWavePlane(world, wx, wy);
        for (int i = 0; i < kTrainCount; ++i)
            Check(PeriodDistance(SwellTheta(i, warped.plane.x, warped.plane.y, field.swellPhase),
                SwellTheta(i, reference.plane.x, reference.plane.y, world.swellPhase), kTwoPi) < .035,
                "all draws/reflection cameras see the same turned, warped wave");
    }
    auto held = motion, pause = motion;
    motion.Advance(.1f, 1, .32f, 100, turnedWind, 4213, -7781, true, true);
    pause.Advance(1e9f, 1, .32f, 100, turnedWind, 4213, -7781, true, true);
    SamePhases(motion.Build(0, 0, true, true), pause.Build(0, 0, true, true), 1e-6);
    const auto before = held.Build(0, 0, true, true);
    held.Advance(.1f, 0, .32f, 100, startWind, -15000, 20000, true, true);
    SamePhases(before, held.Build(0, 0, true, true), 1e-6);
    Check(before.lattice[0] == held.Build(0, 0, true, true).lattice[0], "zero wave speed also freezes turns");
    held.Advance(.1f, 9, 1, 100, startWind, 4213, -7781, true, false);
    SamePhases(before, held.Build(0, 0, true, true), 1e-6);
    Check(held.Build(0, 0, true, false).uvDrift[0] == 0, "waves off stops native UV drift");
    for (int i = 0; i < 100000; ++i)
    {
        const auto wind = Air(static_cast<WindDirection>((i / 1200) % 8));
        held.Advance(.1f, 9, 1, 25, wind, 51000, -51000, true, true);
    }
    held.Advance(NAN, INFINITY, NAN, INFINITY, startWind, NAN, INFINITY, true, true);
    const auto safe = held.Build(NAN, INFINITY, true, true);
    for (float phase : safe.swellPhase) Check(std::isfinite(phase) && phase >= 0 && phase <= 16, "bounded long-session phases");
    for (float value : safe.lattice) Check(std::isfinite(value), "finite shader basis after invalid inputs");
    Pass("camera/reflection coherence, speed/off freeze, long sessions, device gaps and invalid input safety");
}
static std::map<std::wstring, int> SettingValues(const WindSettings& s)
{
    return {{L"Enabled",s.enabled},{L"Dynamic",s.dynamic},{L"Direction",int(s.direction)},
        {L"StrengthPercent",int(std::lround(s.strength * 100))},{L"GustPercent",int(std::lround(s.gustAmount * 100))},
        {L"GustSeconds",int(s.gustSeconds)},{L"DirectionChangeSeconds",int(s.directionSeconds)},
        {L"TransitionSeconds",int(s.transitionSeconds)},{L"WeatherResponsePercent",int(std::lround(s.weatherResponse * 100))},
        // One switch per consumer, each one restoring that consumer's legacy
        // behaviour on its own. There is no vegetation consumer any more: the
        // wind reaches the waves, the clouds and the precipitation drift, and
        // nothing that is drawn.
        {L"AffectWaves",s.affectWaves},{L"AffectClouds",s.affectClouds},{L"DebugView",s.debugView}};
}
static void TestConfigurationAndWiring()
{
    const auto ini = Read("GraphicsEffects.ini");
    const auto section = ini.substr(ini.find("[Wind]"), ini.find("[VolumetricClouds]") - ini.find("[Wind]"));
    // Win32 INI readers do not give useful diagnostics for repeated keys;
    // shipped duplicate cloud resolution/mist values once made edits depend
    // on whichever occurrence the API happened to pick.
    std::set<std::string> iniKeys;
    std::map<std::pair<std::string,std::string>,int> iniDefaults;
    std::istringstream iniLines(ini);
    std::string line, group;
    while (std::getline(iniLines, line))
    {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == line.npos || line[start] == ';' || line[start] == '#') continue;
        if (line[start] == '[') { group = line.substr(start, line.find(']', start) - start + 1); continue; }
        const auto equals = line.find('=', start);
        if (equals == line.npos) continue;
        const auto end = line.find_last_not_of(" \t", equals - 1);
        const auto key = line.substr(start, end - start + 1);
        auto normalized = group + "/" + key;
        for (char& ch : normalized) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        Check(iniKeys.emplace(normalized).second,
              "shipped INI must not define a setting twice in one section (case-insensitive)");
        try { iniDefaults.emplace(std::pair{group, key}, std::stoi(line.substr(equals + 1))); }
        catch (const std::invalid_argument&) {} // e.g. a non-integer authored value
        catch (const std::out_of_range&) {}
    }
    for (const auto& item : tuningoverlay::items)
    {
        if (item.kind == tuningoverlay::KIND_HEADER) continue;
        const std::string key(item.key, item.key + std::wcslen(item.key));
        const std::string groupName(item.section, item.section + std::wcslen(item.section));
        const auto found = iniDefaults.find({"[" + groupName + "]", key});
        if (found != iniDefaults.end()) Check(found->second == item.value,
            "F7 Reset Defaults must restore the shipped INI value, not a contradictory preset");
    }
    const auto clouds = ini.substr(ini.find("[VolumetricClouds]"), ini.find("[LocalFog]") - ini.find("[VolumetricClouds]"));
    Check(clouds.find("Quality=2\n") != clouds.npos && clouds.find("ResolutionPercent=25\n") != clouds.npos &&
          clouds.find("\nSampleCount=") == clouds.npos && clouds.find("\nLightSampleCount=") == clouds.npos &&
          clouds.find("\nBaseStep=") == clouds.npos && clouds.find("\nWindAngle=") == clouds.npos,
          "cloud quality and resolution have one owner; legacy wind angle is not shipped while linked");
    const auto weatherIni = ini.substr(ini.find("[WeatherVisuals]"), ini.find("[DynamicLighting]") - ini.find("[WeatherVisuals]"));
    Check(weatherIni.find("\nWindStrengthPercent=") == weatherIni.npos &&
          weatherIni.find("\nWindAngle=") == weatherIni.npos &&
          weatherIni.find("\nMode=") == weatherIni.npos,
          "weather continuity layer no longer ships duplicate wind or type overrides");
    for (const auto& item : tuningoverlay::items)
        Check(!item.section || std::wstring(item.section) != L"WeatherVisuals" ||
              std::wstring(item.key) != L"Mode", "F7 cannot force rain over detected snow or vice versa");
    const auto cloudSource = Read("src/Effects/VolumetricClouds.cpp");
    Check(cloudSource.find("read(L\"SampleCount\"") == cloudSource.npos &&
          cloudSource.find("read(L\"LightSampleCount\"") == cloudSource.npos &&
          cloudSource.find("read(L\"BaseStep\"") == cloudSource.npos,
          "old explicit march settings cannot silently override F7 quality on existing INIs");
    const auto weatherSource = Read("WeatherVisuals.h");
    Check(weatherSource.find("precipitationMotion.Advance(dt, speed, frame.wind") != weatherSource.npos &&
          weatherSource.find("ReadTuningInt(L\"WindAngle\"") == weatherSource.npos &&
          weatherSource.find("ReadTuningInt(L\"WindStrengthPercent\"") == weatherSource.npos &&
          // The long-session banding fix: the shader no longer carries an
          // unbounded phase of its own. The integrated fall/wind arrive as
          // the CPU's split decimal parts (c6/c7/c8), and the integer parts
          // move only the wrapped cell index.
          weatherSource.find("float phaseX[3] = { layerPhase0.x, layerPhase1.x, layerPhase2.x };") != weatherSource.npos &&
          weatherSource.find("float2 cell = floor(gridDec) - float2(phaseXInt[i], phaseYInt[i]);") != weatherSource.npos &&
          weatherSource.find("weatherParams.x * weatherParams.w") == weatherSource.npos &&
          weatherSource.find("const int renderMode = nativeWeatherMode;") != weatherSource.npos &&
          weatherSource.find("ReadTuningInt(L\"Mode\"") == weatherSource.npos,
          "production weather uses native type and integrated fall/wind, not separate clocks or overrides");
    const auto weatherResetStart = weatherSource.find("inline void Reset(IDirect3DDevice9* device)");
    Check(weatherResetStart != weatherSource.npos &&
          weatherSource.substr(weatherResetStart, weatherSource.find("inline bool IsTerrainShader", weatherResetStart) - weatherResetStart)
              .find("precipitationMotion") == std::string::npos,
          "device recovery retains precipitation phases while dropping GPU resources");
    std::map<std::wstring, int> readDefaults;
    const auto defaults = SettingValues(ReadWindSettings([&](const wchar_t* key, int value) { readDefaults[key] = value; return value; }));
    Check(defaults == SettingValues(WindSettings{}) && defaults == readDefaults, "configuration defaults match simulation defaults");
    unsigned count = 0;
    for (const auto& control : tuningoverlay::items)
    {
        if (!control.section || std::wstring(control.section) != L"Wind") continue;
        ++count;
        Check(defaults.at(control.key) == control.value, "F7 and runtime have identical defaults");
        const std::wstring key(control.key);
        const std::string entry = std::string(key.begin(), key.end()) + "=" + std::to_string(control.value) + "\n";
        Check(section.find(entry) != std::string::npos, "shipped INI and F7 default match");
        if (control.kind == tuningoverlay::KIND_SLIDER)
        {
            for (const auto& [asked, expected] : {std::pair{control.lo - 1000, control.lo}, {control.hi + 1000, control.hi}})
            {
                const auto values = SettingValues(ReadWindSettings([&](const wchar_t* k, int fallback) { return key == k ? asked : fallback; }));
                Check(values.at(key) == expected, "INI and F7 range endpoints match");
            }
        }
        if (key == L"Direction")
            for (int i = 0; i < 8; ++i)
            {
                auto item = control; item.value = i;
                Check(tuningoverlay::ValueLabel(item) == WindDirectionName(static_cast<WindDirection>(i)), "F7 shows compass names, not unexplained numbers");
            }
    }
    Check(count == defaults.size(), "every supported wind option has an F7 control");
    const auto main = Read("ModernWoWRenderer.cpp");

    // The vegetation sway is GONE, and it is gone in every place a feature can be
    // switched on from. Each of these is a way for it to come back without
    // anybody noticing: a file restored by a merge, a section still read from the
    // INI, a toggle still drawn in F7, a HUD row still printed, a library still
    // compiled. What it must never be is an off-by-default switch - a player
    // would still be paying for it, and a later edit would turn it back on.
    {
        for (const char* path : {"src/Effects/VegetationSway.h", "src/Effects/GrassBladeSway.h",
                "src/Environment/GrassShaderWind.h", "src/Environment/VegetationWind.h",
                "src/Core/M2DrawGeometry.h", "src/Core/M2ShaderProbe.h", "src/Core/TextureCutout.h",
                "docs/vegetation-wind.md", "tools/VegetationWindRegression.cpp",
                "tools/Test-VegetationWind.cmd", "tools/VegetationPreview.cpp"})
            Check(!std::filesystem::exists(path), "the sway's own file is deleted, not disabled");
        Check(ini.find("[Vegetation]") == ini.npos && ini.find("AffectVegetation") == ini.npos &&
              ini.find("GrassBlades") == ini.npos && ini.find("SwayModels") == ini.npos,
              "no sway section, no plant link and no sway key is shipped");
        for (const auto& item : tuningoverlay::items)
        {
            const std::wstring section(item.section ? item.section : L"");
            const std::wstring key(item.key ? item.key : L"");
            Check(section != L"Vegetation" && key != L"AffectVegetation" && key != L"SwayModels" &&
                  key != L"GrassBlades" && key != L"SwayAnimatedModels" && key != L"TreePercent" &&
                  key != L"GrassLimitYards",
                  "F7 has no sway control left, not even a hidden one");
        }
        const auto windSystem = Read("src/Environment/WindSystem.h");
        const auto windTuning = Read("src/Environment/WindTuning.h");
        const auto windDebug = Read("src/Environment/WindDebug.h");
        Check(windSystem.find("affectVegetation") == windSystem.npos &&
              windSystem.find("DrivesVegetation") == windSystem.npos &&
              windTuning.find("ReadVegetationSettings") == windTuning.npos,
              "the shared wind has no vegetation consumer to drive");
        Check(windDebug.find("Plants") == windDebug.npos && windDebug.find("Blades") == windDebug.npos &&
              windDebug.find("Waves: ") != windDebug.npos && windDebug.find("Rain drift: ") != windDebug.npos,
              "the wind HUD keeps its own rows and none of the sway rows");
        Check(main.find("vegetation::") == main.npos && main.find("grassblade::") == main.npos &&
              main.find("ReloadVegetationTuning") == main.npos &&
              main.find("SetVertexShaderConstantF") == main.npos,
              "the renderer has no sway path, no sway reload and no blade-constant hook left");
        const auto project = Read("ModernWoWRenderer.vcxproj");
        const auto workflow = Read(".github/workflows/build.yml");
        Check(project.find("Vegetation") == project.npos && project.find("GrassBlade") == project.npos &&
              project.find("TextureCutout") == project.npos,
              "the project file does not compile the sway");
        Check(workflow.find("VegetationWindRegression") == workflow.npos &&
              workflow.find("Test-VegetationWind") == workflow.npos,
              "no CI job builds the sway suite");
    }
    Check(Read("ModernWoWRenderer.ini").find("\n[Atmosphere]\n") == std::string::npos &&
          main.find("DrawAtmosphere") == main.npos && main.find("ReloadSettings()") == main.npos,
          "dead flat Atmosphere controls no longer compete with the live volume settings");
    const auto nativeFog = Read("DistanceFog.h");
    const auto volumeFog = Read("src/Effects/DirectionalVolumetricLighting.cpp");
    const auto startup = Read("ModernWoWRenderer.ini");
    const auto nativeSection = startup.substr(startup.find("[DistanceFog]"), startup.find("[Volume]") - startup.find("[DistanceFog]"));
    Check(nativeSection.find("\nEnabled=") == nativeSection.npos &&
          nativeFog.find("L\"Enabled\",0,mainIni") == nativeFog.npos &&
          nativeFog.find("key,fallback,tuningIni.c_str()") != nativeFog.npos,
          "one F7 edge-fog switch owns both volume and native fallback, not a second startup gate");
    Check(nativeFog.find("ReadTuning(L\"DistancePercent\",100)") != nativeFog.npos &&
          nativeFog.find("ReadTuning(L\"PowerPercent\",100)") != nativeFog.npos &&
          volumeFog.find("ReadInt(L\"DistanceFog\",L\"DistancePercent\",100") != volumeFog.npos &&
          volumeFog.find("ReadInt(L\"DistanceFog\",L\"PowerPercent\",100") != volumeFog.npos &&
          Read("src/Materials/MaterialCacheManager.cpp").find("ReadInt(L\"AIMaterials\", L\"Enabled\", 0") != std::string::npos,
          "fallbacks match F7/INI neutral fog and keep unfinished materials off");
    const auto present = main.find("HRESULT WINAPI HookedPresent");
    const auto fallback = main.find("    volume::Present(d);", present);
    const auto weather = main.find("    weathervisuals::Present(d,tuningoverlay::visible);", present);
    const auto snapshot = main.find("nextFrame.wind=g_windSystem.Update", present);
    const auto sea = main.find("    watereffect::Present();", present);
    Check(fallback < weather && weather < snapshot && snapshot < sea, "one next-frame snapshot after all composites and weather detection, before sea advance");
    // The chain is one reload per subsystem, each one timed by the line a
    // player's log carries, and it runs on an F7 edit or a settled place - never
    // on a place whose tuning is already in force, which is the crossing this
    // client's `Area=0`/`Area=223` flap produces on every other sample. F12
    // reads the per-frame focus sample (one GetForegroundWindow per frame,
    // shared by every hotkey - FrameContext::wowFocused), not a fresh sample.
    Check(main.find("if(f7Edited||(locationTuningChanged&&placeTuningStale)){") != main.npos &&
          main.find("ReloadWindTuning();stage(tWind);") != main.npos &&
          main.find("watereffect::ReloadTuning();stage(tWater);") != main.npos &&
          main.find("volume::ReloadTuning();stage(tVolume);") != main.npos &&
          main.find("weathervisuals::ReloadTuning();stage(tWeather);") != main.npos &&
          main.find("AppliedTuningStillInForce(") != main.npos &&
          main.find("the place that settled is the one already in force") != main.npos &&
          main.find("if(down&&!tuningKeyDown&&renderer::FrameContext::Current().wowFocused){renderer::locationtuning::InvalidateScopedKeys();ReloadWindTuning();") != main.npos,
          "F7/location/F12 all reload wind, F12 drops the scoped read cache so a hand-edited file is picked up, and a "
          "crossing that settles on the place already in force reloads nothing");
    // A crossing must not stall the frame. This client's place flaps across a
    // boundary while the player walks it, and a change used to re-read every
    // scoped key of GraphicsEffects.ini (per-key probes) AND every row of the
    // F7 menu on the spot. All three pieces are pinned here: the change is
    // applied once the place has settled, the scoped key sets are read once per
    // scope, and the chain itself is untouched.
    const auto tuning = Read("src/Environment/LocationTuning.h");
    // Caching the VALUES was only half of it: an entry still had to be FILLED, and
    // filling one asked the profile API for a single section - which opens and
    // parses the whole file to find it. A crossing touches ~66 sections, so its
    // cost was the size of the settings file times 66: measured at 1.4 ms for a
    // fresh install, 66 ms for an 84 KB LocationGraphics.ini, 691 ms for 587 KB
    // and 2653 ms for 2.1 MB, and every F7 save invalidates the cache so the next
    // crossing pays the table again. The file is now read and indexed ONCE per
    // identity and a section is parsed out of that text, which is what makes the
    // cost of a crossing independent of how much the player has authored.
    Check(tuning.find("inline void EnsureProfileLoaded(ProfileValues& file,const std::wstring& path)") != tuning.npos &&
          tuning.find("IndexProfileText(file);") != tuning.npos &&
          tuning.find("ParseSectionSpan(file.text,file.spans[at],values);") != tuning.npos &&
          // No per-section profile-API call is left to re-read the whole file.
          tuning.find("GetPrivateProfileSectionW(") == tuning.npos &&
          tuning.find("GetPrivateProfileSectionNamesW(") == tuning.npos &&
          tuning.find("std::vector<wchar_t> buffer(32768)") == tuning.npos &&
          tuning.find("const std::wstring wanted=LowerKeyName(key);") != tuning.npos &&
          tuning.find("if(values->find(wanted)==values->end())continue;") != tuning.npos &&
          tuning.find("GetPrivateProfileStringW(scope.c_str(),key,L\"\",value,128,localFile.c_str())") == tuning.npos &&
          tuning.find("inline void InvalidateScopedKeys() { InvalidateProfileValues(); }") != tuning.npos &&
          tuning.find("if(nextLocal!=localFile||!ScopedFileUnchanged())InvalidateScopedKeys();") != tuning.npos &&
          tuning.find("if(written)InvalidateScopedKeys();") != tuning.npos &&
          // There is no second, key-only memo left to fall out of step with the
          // value cache or to overflow in the middle of an apply.
          tuning.find("RememberScopedFile") == tuning.npos && tuning.find("memoWritten") == tuning.npos,
          "a scoped read answers from one whole-file load per identity, so filling a section costs that section and "
          "not another parse of the whole file");
    Check(tuning.find("inline DWORD ReadProfileString(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path)") != tuning.npos &&
          tuning.find("inline bool ProfileIdentity(ProfileValues& file,const std::wstring& path)") != tuning.npos &&
          tuning.find("file.sections.clear();") != tuning.npos &&
          tuning.find("GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)") != tuning.npos &&
          tuning.find("ProfileValues& file=ProfileCache()[std::wstring(path)];") != tuning.npos,
          "and the values behind those reads come from a section read once per file identity: a crossing's hundred "
          "reads are map lookups and a stat each, not a hundred parses of the whole file (measured at 125-200 ms per "
          "crossing before this)");
    // And nothing behind those reads may be stale: a write of this module's own
    // drops the values, because a file's own last-write time is not a signal right
    // after a write (NTFS updates it lazily, and a value of the same length does
    // not change the size either). A Save followed by the read-back that reports
    // the new state went stale exactly that way, which the water device
    // regression caught; every write path in Save now goes through WriteProfile.
    Check(main.find("renderer::locationtuning::InvalidateProfileValues();") == main.npos &&
          main.find("renderer::locationtuning::InvalidateScopedKeys();ReloadWindTuning();") != main.npos &&
          tuning.find("inline DWORD ReadProfileString(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path)") != tuning.npos &&
          tuning.find("inline bool WriteProfile(LPCWSTR path,LPCWSTR section,LPCWSTR key,int value)") != tuning.npos &&
          tuning.find("if(written)InvalidateProfileValues();") != tuning.npos &&
          tuning.find("inline void InvalidateScopedKeys() { InvalidateProfileValues(); }") != tuning.npos &&
          tuning.find("if(IsGlobal(section))return WriteProfile(baseFile.c_str(),section,key,value);") != tuning.npos &&
          tuning.find("if(!editable)return WriteProfile(baseFile.c_str(),section,key,value);") != tuning.npos &&
          tuning.find("const bool written=WriteProfile(localFile.c_str(),scope.c_str(),key,value);") != tuning.npos,
          "and every write it makes drops the cached values, so a saved toggle is what the next read answers");
    // ONE cache, and that is a correctness fact rather than a tidying: until this
    // sweep the key sets of a scope lived in a second cache with a ceiling of 96
    // scopes, and a real apply - nine tuning reloads plus the F7 menu's hundred-odd
    // rows, each read at every level of its own section - asks about more scopes
    // than that. The ceiling was reached in the middle of the apply, the whole key
    // cache was dropped, and every read after it paid a GetPrivateProfileSectionW:
    // a whole-file parse per scope per level, dozens of them in a session's log,
    // while the place kept flapping. A crossing's cost must not depend on how many
    // scopes the apply happens to walk.
    Check(tuning.find("inline const std::unordered_map<std::wstring,std::wstring>* ScopedValuesFor(") != tuning.npos &&
          tuning.find("inline bool ScopedValuePresent(") != tuning.npos &&
          tuning.find("return values&&values->find(LowerKeyName(key))!=values->end();") != tuning.npos &&
          tuning.find("ScopedKeysFor") == tuning.npos &&
          tuning.find("RememberScopedFile") == tuning.npos &&
          tuning.find(">=4096") != tuning.npos,
          "one identity-checked cache answers both presence and value for a scope, so the read path, HasScopedValue "
          "and the water colours see one file view - and no second key-only memo is left to overflow in the middle "
          "of an apply");
    Check(tuning.find("if(nextLocal!=localFile||!ScopedFileUnchanged())InvalidateScopedKeys();") != tuning.npos &&
          tuning.find("if(written)InvalidateScopedKeys();") != tuning.npos &&
          tuning.find("inline bool ScopedFileUnchanged() {") != tuning.npos &&
          tuning.find("return ProfileIdentity(file,localFile);") != tuning.npos &&
          tuning.find("++profileReads;") != tuning.npos &&
          tuning.find("++profileParses;") != tuning.npos,
          "a different base directory, a file changed underneath the memo and an editor save all drop what was read "
          "out of the scoped file - and a PLACE change does not, which is what a crossing used to spend its second "
          "re-reading section by section");
    // And a crossing must not WRITE for the sake of writing. Three stages appended a
    // line about their own tuning on every settled place, and in the game folder a
    // line is an open, a write and a close - milliseconds each on a machine whose
    // antivirus watches that folder, on exactly the path a player walks a border on.
    // Each of them now writes only when its numbers change, which is the only time
    // the line says anything new.
    Check(Read("WaterEffect.h").find("if(lastRow!=row.str()){lastRow=row.str();") != std::string::npos &&
          Read("VolumeIntegration.h").find(
              "if(!logPath.empty()&&lastPostProcessLine!=message){lastPostProcessLine=message;Log(message);}") !=
              std::string::npos &&
          Read("src/Materials/MaterialCacheManager.cpp").find("if (lastTuningLine != line)") != std::string::npos,
        "the per-crossing log lines are written when their numbers change, not once per settled place");
    // And the stall itself, measured rather than described. "Still very hard lag
    // on area change" has two possible owners - the client loading the place, or
    // this project's own work on the frame that noticed - and this module cannot
    // see inside the client's loader. It CAN see the gap between presents, on the
    // same performance counter the reload line uses, next to what its own work
    // cost: a 2 s gap around a reload of sixty microseconds is the client's loader,
    // and the line says so in the report instead of leaving it to an argument.
    Check(main.find("frame stall: %.0f ms between presents") != main.npos &&
          main.find("the frame that took this gap drew %llu draw(s) against %llu frame(s) for the session") !=
              main.npos &&
          main.find("const uint64_t frameDraws = sessionDraws - g_drawsAtLastPresent;") != main.npos &&
          main.find("the last tuning reload cost %.2f ms, and this project's ") != main.npos &&
          main.find("the place changed \" + std::to_string(") != main.npos &&
          main.find("g_lastReloadMicros=micros(renderer::PerformanceCounter()-reloadBegan);") != main.npos &&
          main.find("const bool stall = renderer::FrameGapIsAStall(gapMs, g_presentGapMs);") != main.npos &&
          Read("src/Diagnostics/RendererDiagnostics.h").find("uint64_t DrawCalls() const noexcept") !=
              std::string::npos &&
          Read("src/Diagnostics/RendererDiagnostics.cpp").find(
              "m_drawCalls.fetch_add(1, std::memory_order_relaxed);") != std::string::npos,
          "a frame that stalls is written to the log with its own gap, the place it happened in, how long ago the "
          "place changed, what this project's own reload cost AND the draw load of the frame itself - the one number "
          "that says whether the frame that took the gap was drawing the place again, so the next report says who the "
          "stall belongs to");
    Check(main.find("const bool settled=g_placePending&&now-g_placeChangedAt>=kLocationSettleMs&&") != main.npos &&
          main.find("const bool locationTuningChanged=settled||(placeChanged&&!g_locationConfigured);") != main.npos &&
          main.find("if(placeChanged){g_placeChangedAt=now;g_placePending=true;}") != main.npos &&
          main.find("if(locationTuningChanged){\n        g_placePending=false;g_locationConfigured=true;") != main.npos &&
          main.find("if(placeTuningStale){g_locationAppliedAt=now;tuningoverlay::Configure(g_basePath);}") != main.npos &&
          main.find("if(now-g_staleLoggedAt>=1000){") != main.npos &&
          main.find("++g_placeSkipped;") != main.npos &&
          main.find("answered %llu read(s) from memory with %llu whole-file parse(s)") != main.npos,
          "a location change is applied once the place has held still, one reload per gap, and the first one at once so "
          "startup configures itself - while a place whose tuning is already in force is skipped without spending the "
          "gap, and a walk along a border does not write a log line per sample about it: the line says how many reads "
          "the apply answered from memory and how many whole-file parses it paid");
    // And the skip has to survive a heavy settings file. The listing it is decided
    // on came from GetPrivateProfileSectionNamesW into a fixed 32768-character
    // buffer, which about a thousand authored scopes overflow: the overflow made
    // the fingerprint unprovable, unprovable turned the skip OFF, and an
    // unprovable listing is not cached either - so the whole file was re-listed on
    // every ask, twice per crossing, forever. Measured at 84 KB: 1.4 ms per
    // listing and no skip at all; at 2.1 MB, 46 ms. The listing now comes off the
    // index the load builds, so it has no ceiling, and both the suffix walk and
    // the fingerprint are kept against the identities they were derived from.
    Check(tuning.find("inline const std::vector<std::wstring>* SectionNamesOf(const std::wstring& path)") != tuning.npos &&
          tuning.find("for(const auto& entry:file.firstSpanOf)file.listing.push_back(entry.first);") != tuning.npos &&
          tuning.find("GetPrivateProfileSectionNamesW(") == tuning.npos &&
          tuning.find("if(length>=static_cast<DWORD>(buffer.size())-2)return nullptr;") == tuning.npos &&
          tuning.find("ScopedSuffixList(localWritten,localBytes,baseWritten,baseBytes)") != tuning.npos &&
          tuning.find("std::wstring suffix=SectionSuffixOf(name);") != tuning.npos &&
          // Deepest level wins, by a stable sort of pointers rather than a
          // std::map and a std::set of copied strings per suffix.
          tuning.find("std::stable_sort(resolved.begin(),resolved.end(),") != tuning.npos &&
          tuning.find("FingerprintMix(hash,L\"scoped\");") != tuning.npos &&
          tuning.find("FingerprintMix(hash,suffix);") != tuning.npos &&
          tuning.find("return current!=0&&current==appliedFingerprint;") != tuning.npos &&
          tuning.find("if(!provable) {active=previous;return 0;}") != tuning.npos &&
          // The answers are remembered, and a write drops them with the values.
          tuning.find("for(const FingerprintMemo& memo:FingerprintMemos())") != tuning.npos &&
          tuning.find("ForgetScopedAnswers();") != tuning.npos,
          "the skip is decided on what the FILE holds, never on what a previous read cached: the section list and "
          "the per-suffix answers are read from the profile file itself, so a scope written by hand - or by the "
          "editor - in a section no read has touched still refuses the skip instead of quietly never applying, and a "
          "listing the API truncated refuses to skip at all rather than assume the file holds no more sections");    const auto cloud = Read("src/Effects/VolumetricClouds.cpp");
    Check(cloud.find("dt,f.wind)") != cloud.npos && cloud.find("f.wind.sequence!=m_windSequence") != cloud.npos, "clouds consume snapshot once per frame");
    const auto water = Read("WaterEffect.h");
    Check(water.find("frame.wind.DrivesWaves(),frame.wind.strength") != water.npos &&
          water.find("waveScalePercent,frame.wind,pivotX,pivotY") != water.npos, "waves use shared direction AND strength");
    const auto resetBegin = water.find("inline void Reset(IDirect3DDevice9* d)");
    Check(resetBegin != water.npos, "production water reset is present");
    const auto reset = water.substr(resetBegin, water.find("void DrawStatus", resetBegin) - resetBegin);
    Check(reset.find("ResetObservationCache(d)") != reset.npos &&
          reset.find("ResetMrtCapabilities(d)") != reset.npos &&
          reset.find("waterTargetDescValid = false") != reset.npos,
          "device reset drops GPU observation/MRT/target caches alongside persistent wind");
    Check(reset.find("lastWaveAnchor.valid=false") != reset.npos &&
          water.find("seaFilter.Forget();") == water.npos,
          "device reset invalidates only the camera anchor, not CPU wind/sea phase history");
    auto debug = Air(WindDirection::East, 1.25f);
    const auto lines = WindDebugLines(debug);
    Check(lines[1].find("From E 90.0") != std::string::npos && lines[2].find("125.0%") != std::string::npos &&
          lines[4].find("Rain drift: wind") != std::string::npos,
          "debug reports actual heading, strength and rain's link to shared wind");
    debug.enabled=false;
    Check(WindDebugLines(debug)[4].find("Rain drift: off") != std::string::npos,
          "debug reports no added rain drift when shared wind is off");
    Pass("INI uniqueness, cloud quality, rain link, wind/F7 defaults and frame ordering");

#if defined(_WIN32)
    const auto directory = std::filesystem::absolute("build/wind-tuning-regression");
    std::filesystem::create_directories(directory);
    const auto base = directory.wstring() + L"\\";
    const auto file = base + L"GraphicsEffects.ini";
    std::filesystem::remove(file); std::filesystem::remove(base + L"LocationGraphics.ini");
    locationtuning::Sync(base, {}, false);
    Check(locationtuning::Save(L"Wind", L"Direction", 2), "global direction save");
    Check(locationtuning::Save(L"Wind", L"DebugView", 1), "global debug save");
    const auto load = [&] { return ReadWindSettings([&](const wchar_t* key, int fallback) {
        return int(locationtuning::ReadInt(L"Wind", key, fallback, file.c_str())); }); };
    Check(load().direction == WindDirection::East && load().debugView, "global wind readback");
    locationtuning::Sync(base, {0,12,87,true}, true); locationtuning::editZone = true;
    Check(locationtuning::Save(L"Wind", L"Direction", 6), "zone wind save");
    Check(load().direction == WindDirection::West, "zone overrides global wind");
    locationtuning::editZone = false;
    Check(locationtuning::Save(L"Wind", L"Direction", 3), "subarea wind save");
    Check(locationtuning::Save(L"Wind", L"DebugView", 0), "subarea debug save");
    Check(load().direction == WindDirection::SouthEast && !load().debugView, "subarea overrides zone wind/debug");
    locationtuning::Sync(base, {0,12,86,true}, true);
    Check(load().direction == WindDirection::West && load().debugView, "leaving subarea restores zone/global wind");
    locationtuning::Sync(base, {}, false);
    Check(load().direction == WindDirection::East, "unverified client reads/writes global wind");
    SettleBeforeHandEdit();
    const auto beforeGlobalEdit = StampOf(file);
    Check(WritePrivateProfileStringW(L"Wind", L"Direction", L"7", file.c_str()) != 0, "manual INI edit");
    SettleHandEdit(file, beforeGlobalEdit);
    Check(load().direction == WindDirection::NorthWest, "reload reads edited file without restarting wind");
    // The crossing gate, against the real profile API: a settle of a place whose own
    // scoped settings already resolve to what the nine subsystems are running must be
    // skippable, and a place whose scope holds anything a read would pick up must
    // NEVER be skipped. The second half is what keeps this from trading a stall for a
    // setting that quietly stops working: a skip decided from what was cached instead
    // of from what the file holds leaves a scope written by the editor, or by hand,
    // unread - and its value dead - until something else happens to change.
    const auto saved = LocationContext{0,12,88,true};    // the area that gets a scope of its own
    const auto fresh = LocationContext{0,12,90,true};    // two areas that override nothing
    const auto neighbour = LocationContext{0,12,91,true};
    const auto scoped = base + L"LocationGraphics.ini";
    locationtuning::Sync(base, fresh, true);
    locationtuning::MarkTuningApplied(fresh, true);
    Check(locationtuning::AppliedTuningStillInForce(fresh, true), "the place that was just applied is still in force");
    locationtuning::Sync(base, neighbour, true);
    Check(locationtuning::AppliedTuningStillInForce(neighbour, true),
          "an area that overrides nothing answers every scoped read the way the neighbouring area does, so walking "
          "into it is skipped without re-reading nine subsystems");
    SettleBeforeHandEdit();
    const auto beforeScopeEdit = StampOf(scoped);
    Check(WritePrivateProfileStringW(L"Map.0.Zone.12.Area.88.Wind", L"Direction", L"4", scoped.c_str()) != 0,
          "a scope written for one area, as the editor or a hand edit would");
    SettleHandEdit(scoped, beforeScopeEdit);
    locationtuning::Sync(base, saved, true);
    Check(load().direction == WindDirection::South, "the area's own scope is what a read answers now");
    locationtuning::Sync(base, saved, true);
    locationtuning::MarkTuningApplied(saved, true);
    Check(locationtuning::AppliedTuningStillInForce(saved, true), "the area is in force once its own tuning was applied");
    Check(!locationtuning::AppliedTuningStillInForce(fresh, true),
          "walking back is not skipped once the two areas resolve to different values");
    Check(!locationtuning::HasScopedValue(L"Water", L"WaterColour"), "no scoped colour for this area yet");
    SettleBeforeHandEdit();
    const auto beforeSectionEdit = StampOf(scoped);
    Check(WritePrivateProfileStringW(L"Map.0.Zone.12.Area.88.Water", L"WaterColour", L"1", scoped.c_str()) != 0,
          "a scope written into a section no read in this session has touched");
    SettleHandEdit(scoped, beforeSectionEdit);
    locationtuning::Sync(base, saved, true);
    Check(load().direction == WindDirection::South, "the area's other scopes are untouched by the new section");
    locationtuning::Sync(base, saved, true);
    Check(locationtuning::HasScopedValue(L"Water", L"WaterColour"),
          "what the water colours ask - does this scope hold the key at all - is answered by the hand-written scope");
    Check(!locationtuning::AppliedTuningStillInForce(saved, true),
          "a scope no read in this session ever touched still changes the place's fingerprint, so a crossing is "
          "never skipped past the value it holds");
    locationtuning::MarkTuningApplied(saved, true);
    Check(locationtuning::AppliedTuningStillInForce(saved, true), "and once that was applied the place is skipped again");
    SettleBeforeHandEdit();
    const auto beforeValueEdit = StampOf(scoped);
    Check(WritePrivateProfileStringW(L"Map.0.Zone.12.Area.88.Wind", L"Direction", L"5", scoped.c_str()) != 0,
          "the area's wind is edited");
    SettleHandEdit(scoped, beforeValueEdit);
    locationtuning::Sync(base, saved, true);
    Check(load().direction == WindDirection::SouthWest, "the edit is what a read answers");
    locationtuning::Sync(base, saved, true);
    Check(!locationtuning::AppliedTuningStillInForce(saved, true),
          "an edited value refuses the skip with nothing else changed");
    // And what a crossing COSTS, counted rather than argued: a walk that walks more
    // scopes than the old key memo could hold - nine tuning reloads and the F7
    // menu's hundred-odd rows, every level of every section - must be answered out
    // of the sections already in memory, with no whole-file parse behind it however
    // many samples the place flaps across. This is the assertion that fails if a
    // crossing ever re-reads the file again: whether the memo comes back with a
    // ceiling, or the values are dropped because the PLACE changed rather than
    // because the FILE did.
    // One crossing, exactly as the chain reads one: the nine tuning reloads' own
    // keys, the water colours' "does this scope hold it" question, and a row of the
    // F7 menu - against a place that flaps between two areas.
    const auto crossing = [&](int sample) {
        const auto place = (sample % 2) ? saved : neighbour;   // the flap this client's Area=0/Area=223 produces
        locationtuning::Sync(base, place, true);
        (void)load();
        (void)locationtuning::HasScopedValue(L"Water", L"WaterColour");
        (void)locationtuning::ReadEditorInt(L"Wind", L"Direction", 0, file.c_str());
    };
    locationtuning::MarkTuningApplied(saved, true);
    // Warm-up first, and the counters are taken after it: the FIRST read of a
    // section is a whole-file parse by definition, and that is the one parse a
    // crossing is allowed to pay. What this asserts is that it pays no more.
    for (int sample = 0; sample < 16; ++sample) crossing(sample);
    const uint64_t parsesBefore = locationtuning::profileParses;
    const uint64_t readsBefore = locationtuning::profileReads;
    for (int sample = 0; sample < 400; ++sample) crossing(sample);
    Check(locationtuning::profileParses == parsesBefore &&
          locationtuning::profileReads > readsBefore,
          "400 crossings over a real INI - more scopes than the old key memo could hold - answer every settings read "
          "out of memory and pay zero whole-file parses: the cost of a crossing does not depend on how many scopes "
          "the apply walks, and does not come back when the place changes");
    Pass("real Windows INI: global fallback, zone/subarea precedence, debug persistence, manual reload and the "
         "crossing gate over real files");
#endif
}
int main() try
{
    TestDirectionsAndTime(); TestWeather(); TestCloudsAndSea(); TestPrecipitationMotion(); TestWaveMotion(); TestConfigurationAndWiring();
    std::puts("Wind regressions passed");
    return 0;
}
catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
