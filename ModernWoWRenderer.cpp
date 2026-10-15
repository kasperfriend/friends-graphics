#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <mutex>
#include "WaterDiagnostics.h"
#include "src/Diagnostics/CelestialDrawDiagnostics.h"
#include "WaterHighlight.h"
#include "CelestialHighlight.h"
#include "src/Effects/VegetationSway.h"
#include "WaterEffect.h"
#include "DistanceFog.h"
#include "src/Effects/EnvironmentFogCapture.h"
#include "VolumeIntegration.h"
#include "WaterReflection.h"
#include "WeatherVisuals.h"
#include "TuningOverlay.h"
#include "src/Core/ShaderCache.h"
#include "src/Core/FrameContext.h"
#include "src/Core/DeviceHealth.h"
#include "src/Diagnostics/RendererDiagnostics.h"
#include "src/D3D9/DepthCapture.h"
#include "src/D3D9/CameraCapture.h"
#include "src/D3D9/TrackedRenderState.h"
#include "src/D3D9/CelestialTracker.h"
#include "src/Scene/DrawCallClassifier.h"
#include "NativeShadowDiagnostics.h"
#include "src/Materials/MaterialCacheManager.h"
#include "src/Lighting/LocalLightManager.h"
#include "src/Effects/LocalLightingRenderer.h"
#include "src/Effects/GroundSurfaceCapture.h"
#include "src/Environment/WindTuning.h"

static_assert(renderer::kD3DErrDeviceLost == D3DERR_DEVICELOST, "src/Core/DeviceHealth.h must match d3d9.h");
static_assert(renderer::kD3DErrDeviceNotReset == D3DERR_DEVICENOTRESET, "src/Core/DeviceHealth.h must match d3d9.h");

namespace
{
using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
using EndSceneFn = HRESULT (WINAPI*)(IDirect3DDevice9*);
using PresentFn = HRESULT (WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using ResetFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using DrawFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
using DrawIndexedFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using DrawUPFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
using DrawIndexedUPFn = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
using SetLightFn = HRESULT (WINAPI*)(IDirect3DDevice9*,DWORD,const D3DLIGHT9*);
using LightEnableFn = HRESULT (WINAPI*)(IDirect3DDevice9*,DWORD,BOOL);
PresentFn originalPresent=nullptr;
ResetFn originalReset=nullptr;
DrawFn originalDraw=nullptr;
DrawIndexedFn originalDrawIndexed=nullptr;
DrawUPFn originalDrawUP=nullptr;
DrawIndexedUPFn originalDrawIndexedUP=nullptr;
SetLightFn originalSetLight=nullptr;
LightEnableFn originalLightEnable=nullptr;
using ClearFn=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
ClearFn originalClear=nullptr;
bool unified=false,graphicsActive=true,graphicsKeyDown=false,graphicsShowStatus=false,tuningKeyDown=false;
// Alt+Tab / device-loss state. Only touched on the render thread. A gap longer
// than kFrameGapMs between presents means the frame stream was interrupted.
constexpr uint64_t kFrameGapMs = 1000;
renderer::DeviceAvailability g_availability = renderer::DeviceAvailability::Operational;
renderer::FrameGapDetector g_frameGap;
renderer::WindSystem g_windSystem;
renderer::FrameClock g_windClock;

HMODULE g_systemD3D9 = nullptr;
Direct3DCreate9Fn g_realCreate9 = nullptr;
Direct3DCreate9ExFn g_realCreate9Ex = nullptr;
EndSceneFn g_originalEndScene = nullptr;
std::mutex g_hookMutex;
void** g_hookedTable = nullptr;
std::once_flag g_initialize;
thread_local bool g_drawingOverlay = false;
std::wstring g_basePath;

// The log every subsystem writes to. Each call used to open the file, write one
// line and close it again, and an area crossing writes a handful of lines - which
// a scanner watching a game folder turns into milliseconds each. That is what the
// "Location tuning reloaded in 47 ms" line in a player's log was made of, and it
// was still true after the settings themselves had been made to read from memory.
// Lines are buffered here and written once per frame (Present flushes; so does a
// device-loss transition), so a hundred lines cost what one did and a frame with
// nothing to say touches no file at all. The file is NOT held open between
// flushes: a player can still move or delete it while the game runs, which is how
// this log gets looked at in the first place.
std::string g_logPending;
void FlushLog()
{
    if(g_logPending.empty())return;
    std::ofstream out(g_basePath + L"ModernWoWRenderer.log", std::ios::app);
    out << g_logPending;
    out.flush();
    g_logPending.clear();
}
void Log(char const* text)
{
    if(!text)return;
    g_logPending += text;
    g_logPending += '\n';
    // A burst goes out on the spot past a few kilobytes, so a crash can never take
    // more than one frame's worth of lines with it.
    if(g_logPending.size() >= 8192)FlushLog();
}

void SetAvailability(renderer::DeviceAvailability next)
{
    if (next == g_availability) return;
    // Whatever is pending belongs to the state this transition is about to change.
    FlushLog();
    g_availability = next;
    // Presents keep arriving while the device is lost, so the frame-gap check
    // alone would not notice. History from either side of the transition is stale.
    volume::InvalidateTemporalHistory();
    Log(next == renderer::DeviceAvailability::Lost
        ? "Direct3D device lost (Alt+Tab, mode change or driver reset): effects paused until it is restored."
        : "Direct3D device restored: effects resumed.");
}

void ReloadVegetationTuning()
{
    const auto ini=g_basePath+L"GraphicsEffects.ini";
    // The sway reports a shader variant it had to prove itself to this log, by
    // hash, so the captured list can be extended from a player's own session
    // instead of from a guess. Nothing else it does writes to it.
    vegetation::logLine=Log;
    vegetation::Configure(renderer::ReadVegetationSettings([&](const wchar_t* key,int fallback) {
        return static_cast<int>(renderer::locationtuning::ReadInt(L"Vegetation",key,fallback,ini.c_str()));
    }));
    // The grass the client draws as blades is bent in the client's own vertex
    // shader, so it has its own switch ([Vegetation] GrassBlades) and the same
    // log: the two share the INI, the wind snapshot and the HUD, and nothing
    // else. Both are read here so a reload reaches them together.
    grassblade::logLine=Log;
    renderer::GrassBladeSettings blades;
    blades.enabled=vegetation::settings.grassBlades;
    grassblade::Configure(blades);
}

void ReloadWindTuning()
{
    // A crossing no longer drops the cached sections. It used to, on the argument
    // that an apply is the moment every edit has to be visible - and the price
    // was the crossing itself: with the sections gone, the nine reloads and the
    // F7 menu's rows behind this call re-read every section of both files, and
    // each of those is a whole-file parse. That is the number in this line: 62 to
    // 78 ms on every settled crossing, dozens of times in one session's log, and
    // what a player feels as the stall. What keeps the reads honest now is per
    // read rather than per apply, and costs microseconds:
    //   * every read stats the file first (ProfileIdentity), so an edit from
    //     outside - an editor, another tool, this project's own Save - is visible
    //     to the very next read;
    //   * every write this module makes drops the values outright
    //     (WriteProfile), because a file's last-write time is not a signal right
    //     after a write;
    //   * F12 still drops everything, which is the "read it all from disk again"
    //     the key the client prints has always meant.
    const auto ini=g_basePath+L"GraphicsEffects.ini";
    g_windSystem.Configure(renderer::ReadWindSettings([&](const wchar_t* key,int fallback) {
        return static_cast<int>(renderer::locationtuning::ReadInt(L"Wind",key,fallback,ini.c_str()));
    }));
    ReloadVegetationTuning();
}

void RestoreDeviceHooksAfterReset(IDirect3DDevice9* device);

HRESULT WINAPI HookedEndScene(IDirect3DDevice9* device)
{
    // Overlays cannot draw on a lost device; forward the call untouched.
    if (g_availability == renderer::DeviceAvailability::Lost) return g_originalEndScene(device);
    g_drawingOverlay=true;
    if(unified){if(graphicsShowStatus){watereffect::DrawStatus(device,graphicsActive?"FX ON":"FX OFF",0);
        if(graphicsActive&&volume::enabled)watereffect::DrawStatus(device,volume::composed?"AIR ON":"AIR WAIT",1);}
    }
    else {watereffect::DrawStatus(device);
    if(distancefog::showStatus){
        const auto& fog=renderer::DirectionalVolumetricLighting::Instance().Settings();
        const bool volumeEdge=volume::enabled&&volume::active&&volume::composed&&fog.enabled&&fog.edgeFogEnabled;
        watereffect::DrawStatus(device,!distancefog::active||!distancefog::effectEnabled?"HAZE OFF":
            volumeEdge||distancefog::matches?"HAZE ON":"HAZE WAIT",1);
    }}
    if(weathervisuals::enabled&&weathervisuals::showStatus)watereffect::DrawStatus(device,weathervisuals::GetStatusText(),3);
    nativeshadowdiag::DrawPreview(device);
    tuningoverlay::DrawWindDebug(device,renderer::FrameContext::Current().wind,vegetation::DebugLines());
    tuningoverlay::Draw(device);
    renderer::MaterialCacheManager::Instance().DrawDebugOverlay(device);
    g_drawingOverlay=false;
    return g_originalEndScene(device);
}

// When the place last changed, when its tuning was last applied, and whether the
// renderer has configured itself for a place at all yet. See HookedPresent.
uint64_t g_placeChangedAt = 0;
uint64_t g_locationAppliedAt = 0;
uint64_t g_staleLoggedAt = 0;
unsigned g_placeSkipped = 0;
bool g_placePending = false;
bool g_locationConfigured = false;
constexpr uint64_t kLocationSettleMs = 300;   // the environment manager's own area debounce
constexpr uint64_t kLocationReloadGapMs = 1000;

// And the stall itself, counted rather than described. A report of "still very
// hard lag on area change" has exactly two possible owners: the client loading
// the place it just walked into, or this project's own work on the frame that
// noticed. This module cannot see inside the client's loader, but it can see the
// FRAME GAP - the microseconds between presents, on the same performance counter
// the reload line uses - and it knows exactly what its own work cost and when the
// place last changed. So the next report does not have to argue about it: a stall
// is written to the log once, with the gap, the minute the gap ended, how long the
// place had been settled, and what the last tuning reload cost. If the gaps are
// the client's loader, the line says so by showing a 2 s gap around a reload that
// cost 63 us.
//
// The threshold is relative to the session's own average frame, so the same code
// reports a stall on a 30 fps machine and on a 240 fps one, and a frame that is
// merely slower is not a stall - only one an order of magnitude off, or a quarter
// of a second outright, is worth a line.
std::int64_t g_lastPresentTick = 0;
double g_presentGapMs = 0.0;          // a running average of the frame gap, in ms
double g_worstGapMs = 0.0;            // and the worst one this session
uint64_t g_worstGapAt = 0;            // when it happened, in GetTickCount64 ms
uint64_t g_stallLoggedAt = 0;
unsigned g_stallsLogged = 0;
unsigned g_stalls = 0;
std::int64_t g_lastReloadMicros = 0;  // what the last tuning reload cost, for the line
constexpr unsigned kStallLogCap = 60; // a session's worth; a log is not a stall itself

HRESULT WINAPI HookedPresent(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND w,const RGNDATA* r) {
    const uint64_t now = GetTickCount64();
    // Every Present is a frame boundary. A long pause means the frame stream
    // was interrupted (Alt+Tab, minimised window, loading screen), so temporal
    // history from before the pause must not be blended into the next frame.
    if (g_frameGap.Observe(now, kFrameGapMs)) volume::InvalidateTemporalHistory();

    // The frame gap, on the performance counter, measured BEFORE any of this
    // project's work runs for the frame: what the gap holds is the client's own
    // frame - its renderer, its loader, and this module's work on the previous
    // one. A stall is written to the log when it is an order of magnitude off the
    // session's own average frame, or a quarter of a second outright, at most
    // once a second and at most a session's worth of lines.
    {
        const std::int64_t tick = renderer::PerformanceCounter();
        const std::int64_t frequency = (std::max<std::int64_t>)(1, renderer::PerformanceFrequency());
        if (g_lastPresentTick)
        {
            const double gapMs = double(tick - g_lastPresentTick) * 1000.0 / double(frequency);
            // A running average that a stall cannot swamp: one 2 s frame in sixty
            // must not turn the average into a third of a second, or the second
            // stall would look normal.
            g_presentGapMs = g_presentGapMs ? (g_presentGapMs * .95 + gapMs * .05) : gapMs;
            if (gapMs > g_worstGapMs) { g_worstGapMs = gapMs; g_worstGapAt = now; }
            const bool stall = renderer::FrameGapIsAStall(gapMs, g_presentGapMs);
            if (stall)
            {
                ++g_stalls;
                if (g_stallsLogged < kStallLogCap && now - g_stallLoggedAt >= 1000)
                {
                    g_stallLoggedAt = now;
                    ++g_stallsLogged;
                    const auto location = renderer::EnvironmentProfileManager::Instance().Location();
                    std::string place = location.valid
                        ? ("place Map=" + std::to_string(location.mapId) + " Zone=" + std::to_string(location.zoneId) +
                            " Area=" + std::to_string(location.areaId))
                        : std::string("no place verified yet");
                    if (g_placeChangedAt)
                        place += ", the place changed " + std::to_string(
                            static_cast<unsigned long long>(now - g_placeChangedAt)) + " ms before this frame";
                    else
                        place += ", and no place change had been seen yet";
                    char line[420]{};
                    std::snprintf(line, sizeof(line),
                        "frame stall: %.0f ms between presents (session average %.1f ms, worst %.0f ms, %u so far) - %s; "
                        "the last tuning reload cost %.2f ms, and this project's own work for the stalled frame is "
                        "what the line above the gap was",
                        gapMs, g_presentGapMs, g_worstGapMs, g_stalls, place.c_str(),
                        double(g_lastReloadMicros) / 1000.0);
                    Log(line);
                }
            }
        }
        g_lastPresentTick = tick;
    }

    if (g_availability == renderer::DeviceAvailability::Lost) {
        // No effect pass can draw while the device is lost. Skip them instead
        // of letting each one fail (and log) every frame. Probe for recovery;
        // a device that needs a Reset is restored by HookedReset.
        if (SUCCEEDED(d->TestCooperativeLevel())) {
            SetAvailability(renderer::DeviceAvailability::Operational);
        } else {
            // Keep the per-frame state consistent and the weather detection
            // alive while the frame passes through untouched.
            waterreflection::Finish();
            volume::Finish(d);
            weathervisuals::Present(d, true);
            FlushLog();
            return originalPresent(d, a, b, w, r);
        }
    }

    // What the blade path did with the frame that just ended, every ten seconds
    // of play and only when it has something to say. This is the line a report can
    // be answered from without a screenshot - "the field the player stands in" is
    // `drew` draws that went out with a shader this build rewrote, and a report
    // that shows a green field with `drew 0` has narrowed the fault to the gate
    // (the counters next to it) rather than to the bend itself.
    {
        const uint64_t frameIndex = renderer::FrameContext::Current().frameIndex;
        if (frameIndex && frameIndex % 600 == 0) {
            const renderer::GrassBladeStatus& blades = grassblade::Status();
            if (blades.bladeDraws || blades.injected || blades.refused || blades.noTexcoord ||
                blades.heightDraws || blades.foliageDraws || blades.bigDraws) {
                char bladeLine[360]{};
                std::snprintf(bladeLine, sizeof(bladeLine),
                    "blade wind: %u draw(s) bent this frame, %u of them by height, %u block upload(s) | "
                    "%u shader(s) bent, %u looked at | "
                    "mat %u never %u decl %u big %u card %u read %u vs %u asm %u | %.2f yd at the tip",
                    blades.bladeDraws, blades.heightDraws, blades.uploads, blades.injected, blades.examined,
                    blades.materialRefused, blades.neverModel, blades.noTexcoord, blades.bigDraws,
                    blades.cardRefused, blades.cardUnreadable, blades.registerMismatch, blades.refused,
                    double(blades.amplitudeYards));
                Log(bladeLine);
            }
        }
    }

    // Scene depth is the strongest world-frame signal, but it is optional when
    // water is configured for waves without any scene-dependent features. In
    // that case a known terrain draw or accepted liquid-material draw is the
    // independent signal. A focused menu/loading frame with neither is NOT a
    // location sample: the provider would return an invalid tuple, whose debounce
    // eventually clears zone tuning.
    {
        const bool knownWorldDraw = weathervisuals::worldGeometrySeenThisFrame || watereffect::frameCandidates>0;
        const bool worldFrame = renderer::HasWorldContext(
            renderer::DepthCapture::Instance().HasDepth(), knownWorldDraw);
        auto& environment = renderer::EnvironmentProfileManager::Instance();
        environment.Update(now, worldFrame,
            renderer::ShouldHoldLocation(worldFrame, environment.Location().valid));
    }
    renderer::FrameContext::Current().environment=renderer::EnvironmentProfileManager::Instance().State();
    waterreflection::Present();
    volume::Present(d);
    renderer::MaterialCacheManager::Instance().Present(d);
    weathervisuals::Present(d,tuningoverlay::visible);
    const auto& environmentManager=renderer::EnvironmentProfileManager::Instance();
    const bool placeChanged=renderer::locationtuning::Sync(g_basePath,environmentManager.Location(),environmentManager.LocationTuningVerified());
    // A crossing is not a reason to re-read nine subsystems and every row of the
    // F7 menu per frame. This client's place flaps across a boundary while the
    // player walks it (`Area=0` and `Area=223` alternating in a real session,
    // each alternation a full reload of both) and that reading is what the
    // player sees as a stalled second, not the crossing itself. A change is
    // applied once the place has HELD STILL for kLocationSettleMs, and never
    // sooner than kLocationReloadGapMs after the last apply, so what is in force
    // is always the tuning of the place the player settled in - and walking back
    // and forth over a boundary reloads once, not once per sample. The very
    // first change is applied at once: that one is startup, and nothing should
    // wait three frames for the renderer to configure itself.
    if(placeChanged){g_placeChangedAt=now;g_placePending=true;}
    const bool settled=g_placePending&&now-g_placeChangedAt>=kLocationSettleMs&&
        (now-g_locationAppliedAt>=kLocationReloadGapMs||!g_locationConfigured);
    const bool locationTuningChanged=settled||(placeChanged&&!g_locationConfigured);
    // An F7 edit is always applied; a settled place is applied only when what is
    // in force is NOT already that place's tuning. The place that settled being
    // the one already applied - with neither file changed underneath it - means
    // every scoped read the nine reloads would make already returned this answer,
    // so there is nothing to re-read, nothing to reconfigure and nothing for the
    // player to feel. That is the case this client's `Area=0`/`Area=223` flap
    // produces on every other sample of a walk across a border.
    const bool f7Edited=tuningoverlay::Update();
    // An F7 edit always re-reads everything; a settled place is skipped when
    // nothing about it can differ from what is already in force.
    const bool placeTuningStale=!f7Edited&&locationTuningChanged&&
        !renderer::locationtuning::AppliedTuningStillInForce(
            environmentManager.Location(),environmentManager.LocationTuningVerified());
    if(locationTuningChanged){
        g_placePending=false;g_locationConfigured=true;
        if(placeTuningStale){g_locationAppliedAt=now;tuningoverlay::Configure(g_basePath);}
        else if(!f7Edited){
            // The place that settled adds nothing to the files, so the nine reloads
            // are let alone - and the line is written at most once a second. A walk
            // along a border hands a fresh place to this branch several times a
            // second, and a log line is a file open, a write and a close; the line
            // itself must not become the stall it is reporting on.
            ++g_placeSkipped;
            if(now-g_staleLoggedAt>=1000){
                g_staleLoggedAt=now;
                Log("Location tuning reloaded in 0 us - the place that settled is the one already in force: its own scoped "
                    "settings resolve to the values the nine subsystems are already running with, and neither file changed");
            }
        }
    }
    if(f7Edited||(locationTuningChanged&&placeTuningStale)){
        // How long a crossing's own work takes, in the line the next report will
        // carry: the numbers a player's log can be read for. A reload that reads
        // the tuning of a place that adds nothing to the file is already skipped
        // inside the scoped cache (see LocationTuning.h), so what is left here is
        // only ever a reload that had something to read.
        const std::int64_t reloadBegan=renderer::PerformanceCounter();
        const std::int64_t reloadFrequency=std::max<std::int64_t>(1,renderer::PerformanceFrequency());
        // Each stage is timed, and the line carries all nine: a crossing that
        // still costs something has to say WHICH reload cost it, in the log a
        // player sends, or the next round of this is guesswork again. The sum is
        // the number this line has always carried.
        // Microseconds, not the 15.6 ms ticks GetTickCount64 rounds to: a stage
        // that reads a few cached numbers costs single-digit microseconds, and a
        // timer that cannot see that is a timer that says 16 ms for every stage
        // and 47 ms for a chain that costs two.
        std::int64_t mark=reloadBegan,tWind=0,tWater=0,tFog=0,tVolume=0,tCelestial=0,tShadow=0,tMaterial=0,tLights=0,tWeather=0;
        const auto stage=[&mark,reloadFrequency](std::int64_t& out){const std::int64_t nowTick=renderer::PerformanceCounter();out=nowTick-mark;mark=nowTick;};
        // What the settings reads behind this apply actually did, so the next report
        // can answer the one question a stall asks: was the cost in the reads, and
        // were any of them a whole-file parse? (LocationTuning.h counts both.)
        const uint64_t readsBefore=renderer::locationtuning::profileReads;
        const uint64_t parsesBefore=renderer::locationtuning::profileParses;
        ReloadWindTuning();stage(tWind);
        watereffect::ReloadTuning();stage(tWater);
        distancefog::ReloadTuning();stage(tFog);
        volume::ReloadTuning();stage(tVolume);
        celestialhighlight::ReloadTuning(g_basePath);stage(tCelestial);
        nativeshadowdiag::ReloadEnhancement();stage(tShadow);
        renderer::MaterialCacheManager::Instance().ReloadTuning(g_basePath);stage(tMaterial);
        renderer::LocalLightManager::Instance().ReloadTuning();stage(tLights);
        weathervisuals::ReloadTuning();stage(tWeather);
        const auto micros=[reloadFrequency](std::int64_t ticks){return ticks*1000000/reloadFrequency;};
        char reloadLine[512]{};
        std::snprintf(reloadLine,sizeof(reloadLine),
            "Location tuning reloaded in %lld us (wind %lld water %lld fog %lld volume %lld celestial %lld shadow %lld "
            "material %lld lights %lld weather %lld) - the place was seen %lld ms before this line, and the settings "
            "behind it answered %llu read(s) from memory with %llu whole-file parse(s), after %u settled place(s) "
            "that needed nothing re-read",
            static_cast<long long>(micros(renderer::PerformanceCounter()-reloadBegan)),
            static_cast<long long>(micros(tWind)),static_cast<long long>(micros(tWater)),
            static_cast<long long>(micros(tFog)),static_cast<long long>(micros(tVolume)),
            static_cast<long long>(micros(tCelestial)),static_cast<long long>(micros(tShadow)),
            static_cast<long long>(micros(tMaterial)),static_cast<long long>(micros(tLights)),
            static_cast<long long>(micros(tWeather)),
            static_cast<long long>(now-g_placeChangedAt),
            static_cast<unsigned long long>(renderer::locationtuning::profileReads-readsBefore),
            static_cast<unsigned long long>(renderer::locationtuning::profileParses-parsesBefore),
            g_placeSkipped);
        Log(reloadLine);
        // What this reload cost, kept for the stall line: a 2 s frame gap reported
        // next to a reload that cost sixty microseconds is the client's own area
        // load, and the line says so instead of leaving it to an argument.
        g_lastReloadMicros=micros(renderer::PerformanceCounter()-reloadBegan);
        renderer::locationtuning::MarkTuningApplied(environmentManager.Location(),environmentManager.LocationTuningVerified());
    }
    if(unified){
        bool down=(GetAsyncKeyState(VK_F11)&0x8000)!=0;DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        if(down&&!graphicsKeyDown&&pid==GetCurrentProcessId()){
            graphicsActive=!graphicsActive;watereffect::active=distancefog::active=volume::active=weathervisuals::active=graphicsActive;vegetation::active=grassblade::active=graphicsActive;
            Log(graphicsActive?"F11 graphics ON":"F11 graphics OFF");
        }
        graphicsKeyDown=down;
    }
    {
        bool down=(GetAsyncKeyState(VK_F12)&0x8000)!=0;DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        if(down&&!tuningKeyDown&&pid==GetCurrentProcessId()){renderer::locationtuning::InvalidateScopedKeys();ReloadWindTuning();watereffect::ReloadTuning();distancefog::ReloadTuning();volume::ReloadTuning();celestialhighlight::ReloadTuning(g_basePath);nativeshadowdiag::ReloadEnhancement();renderer::MaterialCacheManager::Instance().ReloadTuning(g_basePath);renderer::LocalLightManager::Instance().ReloadTuning();weathervisuals::ReloadTuning();renderer::EnvironmentProfileManager::Instance().Reload();tuningoverlay::Configure(g_basePath);waterdiag::Configure(g_basePath);Log("F12 reloaded GraphicsEffects.ini, EnvironmentProfiles.ini and water diagnostic settings");}
        tuningKeyDown=down;
    }
    // All scene/cloud rendering has finished. Publish ONE wind snapshot for
    // the NEXT frame, then prepare its sea state. Advancing before volume's
    // fallback composite would give clouds a different wind from the water
    // already drawn in this frame. Detection is held independently of F10,
    // replacement textures and F7; do not infer severity from particle count.
    auto& nextFrame=renderer::FrameContext::Current();
    const auto weather=renderer::ResolveWindWeather(weathervisuals::nativeWeatherMode,
        weathervisuals::intensity,nextFrame.environment[renderer::Weather]);
    nextFrame.wind=g_windSystem.Update(weather,
        g_windClock.Observe(renderer::PerformanceCounter(),renderer::PerformanceFrequency()));
    // The sway is a consumer of that one snapshot, advanced by the snapshot's
    // own bounded delta: no second clock, no second wind. BeginFrame starts the
    // new frame's counters and forgets nothing else, so an F7 edit, a reload,
    // a device reset or an Alt+Tab cannot move a plant.
    vegetation::Update(nextFrame.wind);
    vegetation::BeginFrame();
    // The blade wind is the same snapshot and the same frame: its phases walk
    // forward by the snapshot's own delta and its block is packed for the frame,
    // so a blade and a bush on the same square yard of ground move together.
    grassblade::Update(nextFrame,vegetation::settings.sway);
    grassblade::BeginFrame();
    volume::cloudWeather=static_cast<renderer::CloudWeather>(weather.kind);
    volume::cloudWeatherStrength=weather.strength;
    watereffect::FeedWeather(weather.kind,weather.strength);
    waterhighlight::Present();
    watereffect::Present();
    distancefog::Present();
    waterdiag::Present(d);
    celestialdiag::Present(d);
    nativeshadowdiag::OnPresent(d);
    renderer::RendererDiagnostics::Instance().OnFrameEnd();
    const HRESULT presentResult = originalPresent(d,a,b,w,r);
    // One write per frame for every line the frame produced: see Log.
    FlushLog();
    SetAvailability(renderer::ClassifyDeviceResult(presentResult));
    return presentResult;
}
HRESULT WINAPI HookedReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* p) {
    tuningoverlay::font.Reset();
    tuningoverlay::windDebugFont.Reset();
    waterreflection::Reset(d);
    volume::Reset(d);
    watereffect::Reset(d);
    waterdiag::Reset(d);
    celestialdiag::Reset(d);
    nativeshadowdiag::Reset();
    vegetation::Reset(d);
    renderer::MaterialCacheManager::Instance().Reset(d);
    renderer::LocalLightingRenderer::Instance().Reset(d);
    renderer::LocalLightManager::Instance().Reset();
    weathervisuals::Reset(d);
    renderer::ShaderCache::Instance().Clear();
    renderer::DrawCallClassifier::Instance().ClearCache();
    renderer::DepthCapture::Instance().Reset(d);
    renderer::CameraCapture::Instance().Reset();
    HRESULT hr=originalReset(d,p);
    if(SUCCEEDED(hr)){
        renderer::g_trackedState={};
        RestoreDeviceHooksAfterReset(d);
        SetAvailability(renderer::DeviceAvailability::Operational);
    }
    else {
        // Stay paused; Present keeps probing and resumes once the device is usable.
        SetAvailability(renderer::DeviceAvailability::Lost);
    }
    return hr;
}
HRESULT WINAPI HookedClear(IDirect3DDevice9* d,DWORD n,const D3DRECT* rect,DWORD flags,D3DCOLOR color,float z,DWORD stencil){
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalClear(d,n,rect,flags,color,z,stencil);
    const bool waterNeedsDepth=watereffect::NeedsScene();
    if(volume::internal||g_drawingOverlay||(unified&&!graphicsActive)||(!volume::HasActiveEffects()&&!waterNeedsDepth))
        return originalClear(d,n,rect,flags,color,z,stencil);
    volume::BeforeClear(d,n,rect,flags,z);return originalClear(d,n,rect,flags,color,z,stencil);
}
using SetRenderStateFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SetRenderTargetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetVertexDeclarationFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using SetFVFFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD);
using SetVertexShaderFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVertexShaderConstantFFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetPixelShaderFn = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);

SetRenderStateFn originalSetRenderState = nullptr;
SetRenderTargetFn originalSetRenderTarget = nullptr;
SetTextureFn originalSetTexture = nullptr;
SetVertexDeclarationFn originalSetVertexDeclaration = nullptr;
SetFVFFn originalSetFVF = nullptr;
SetVertexShaderFn originalSetVertexShader = nullptr;
SetVertexShaderConstantFFn originalSetVertexShaderConstantF = nullptr;
SetPixelShaderFn originalSetPixelShader = nullptr;

HRESULT WINAPI HookedSetLight(IDirect3DDevice9* d, DWORD index, const D3DLIGHT9* light)
{
    const HRESULT hr = originalSetLight(d, index, light);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
        renderer::LocalLightManager::Instance().OnSetLight(index, light);
    return hr;
}

HRESULT WINAPI HookedLightEnable(IDirect3DDevice9* d, DWORD index, BOOL enabled)
{
    const HRESULT hr = originalLightEnable(d, index, enabled);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
        renderer::LocalLightManager::Instance().OnLightEnable(index, enabled);
    return hr;
}

HRESULT WINAPI HookedSetRenderTarget(IDirect3DDevice9* d, DWORD index, IDirect3DSurface9* surface)
{
    const HRESULT hr = originalSetRenderTarget(d, index, surface);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
        nativeshadowdiag::OnSetRenderTarget(index, surface);
    return hr;
}

HRESULT WINAPI HookedSetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* texture)
{
    const HRESULT hr = originalSetTexture(d, stage, texture);
    if (SUCCEEDED(hr) && !volume::internal && !weathervisuals::internal && !g_drawingOverlay &&
        !renderer::MaterialCacheManager::Instance().IsInternalPass())
    {
        nativeshadowdiag::OnSetTexture(stage, texture);
        renderer::MaterialCacheManager::Instance().OnSetTexture(stage, texture);
    }
    return hr;
}

HRESULT WINAPI HookedSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* vs)
{
    // A shader this project has bent for the wind is swapped in HERE, before a
    // single byte of it is tracked: what the device holds and what this
    // renderer's state mirror believes have to be the same object, or every
    // gate downstream of this line is reading a shader that is not being drawn.
    grassblade::Substitute(&vs);
    if (renderer::g_trackedState.currentVS != vs || (vs && renderer::g_trackedState.vsHash == 0))
    {
        renderer::g_trackedState.currentVS = vs;
        renderer::g_trackedState.vsHash = (vs && (!unified || graphicsActive)) ? renderer::ShaderCache::Instance().GetShaderHash(vs) : 0;
    }
    return originalSetVertexShader(d, vs);
}

HRESULT WINAPI HookedSetVertexShaderConstantF(IDirect3DDevice9* d, UINT start, const float* values, UINT count)
{
    // The blade wind writes its constants into c14..c19 and re-uploads them when
    // they are gone. Nothing else in this project touches that block, and the
    // engine's own per-chunk upload fills c0..c13 - so a write that reaches into
    // the block means somebody else has just overwritten the wind, and the next
    // grass draw has to put it back.
    grassblade::NoteConstantWrite(start, count);
    return originalSetVertexShaderConstantF(d, start, values, count);
}

HRESULT WINAPI HookedSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* ps)
{
    if (!weathervisuals::internal &&
        (renderer::g_trackedState.currentPS != ps || (ps && renderer::g_trackedState.psHash == 0)))
    {
        renderer::g_trackedState.currentPS = ps;
        renderer::g_trackedState.psHash = (ps && (!unified || graphicsActive)) ? renderer::ShaderCache::Instance().GetShaderHash(ps) : 0;
    }
    return originalSetPixelShader(d, ps);
}

HRESULT WINAPI HookedSetVertexDeclaration(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl)
{
    renderer::g_trackedState.currentVDecl = decl;
    return originalSetVertexDeclaration(d, decl);
}

HRESULT WINAPI HookedSetFVF(IDirect3DDevice9* d, DWORD fvf)
{
    renderer::g_trackedState.currentFVF = fvf;
    return originalSetFVF(d, fvf);
}

HRESULT WINAPI HookedSetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE state, DWORD val)
{
    switch (state)
    {
    case D3DRS_ALPHABLENDENABLE: renderer::g_trackedState.alphaBlend = (val != 0); break;
    case D3DRS_ALPHATESTENABLE:  renderer::g_trackedState.alphaTest = (val != 0); break;
    case D3DRS_ZWRITEENABLE:     renderer::g_trackedState.zWrite = (val != 0); break;
    case D3DRS_ZENABLE:          renderer::g_trackedState.zEnable = (val != 0); break;
    default: break;
    }
    return originalSetRenderState(d, state, val);
}

renderer::DrawClassification ClassifyCurrentDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n,
    const renderer::water::Observation* waterObservation=nullptr)
{
    renderer::DrawCallContext ctx;
    ctx.primitiveType = t;
    ctx.primitiveCount = n;
    ctx.vertexShader = renderer::g_trackedState.currentVS;
    ctx.pixelShader = renderer::g_trackedState.currentPS;
    ctx.vertexDecl = renderer::g_trackedState.currentVDecl;
    ctx.vsHash = renderer::g_trackedState.vsHash;
    ctx.psHash = renderer::g_trackedState.psHash;
    ctx.alphaBlend = renderer::g_trackedState.alphaBlend;
    ctx.alphaTest = renderer::g_trackedState.alphaTest;
    ctx.zWrite = renderer::g_trackedState.zWrite;
    ctx.zEnable = renderer::g_trackedState.zEnable;

    auto dc = renderer::DrawCallClassifier::Instance().Classify(
        d, ctx, volume::capturedViewTranslation, volume::capturedViewValid,
        volume::cameraCaptureShaderHash, waterObservation);
    renderer::RendererDiagnostics::Instance().RecordDrawCall(dc);
    return dc;
}

struct WaterDrawObservation {
    renderer::water::Observation material{};
    bool available=false;
};
WaterDrawObservation ObserveWaterForDraw(IDirect3DDevice9* d,bool requested) {
    const bool effectNeedsMaterial=watereffect::enabled&&watereffect::active&&watereffect::effectEnabled;
    if(g_drawingOverlay||(!requested&&!effectNeedsMaterial))return {};
    return {renderer::water::Observe(d),true};
}

HRESULT WINAPI HookedDraw(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT start,UINT n) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDraw(d,t,start,n);
    if(volume::internal||weathervisuals::internal)return originalDraw(d,t,start,n);
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawPrimitive",t,n);
    if(unified&&!graphicsActive)return originalDraw(d,t,start,n);
    if(!g_drawingOverlay){weathervisuals::ObserveWorldDraw();nativeshadowdiag::OnDraw(d,"DrawPrimitive",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);}
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    const bool updateShadows=!g_drawingOverlay&&volume::ShouldUpdateShadows();
    const bool captureGround=n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed;
    const bool needsWaterMaterial=updateShadows||captureGround||
        renderer::MaterialCacheManager::Instance().IsActive();
    const auto waterDraw=ObserveWaterForDraw(d,needsWaterMaterial);
    if(updateShadows){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        volume::ShadowDraw(d,dc,[&]{return originalDraw(d,t,start,n);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d,waterDraw.available?&waterDraw.material:nullptr);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawPrimitive",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,start,n,false);
    if(captureGround){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{return originalDraw(d,t,start,n);},waterDraw.available?&waterDraw.material:nullptr);volume::internal=false;}
    vegetation::Scope vegetationSway(d,g_drawingOverlay,vegetation::PlainDraw(t,start,n));
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed,
        waterDraw.available?&waterDraw.material:nullptr);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDraw(d,t,start,n);
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
}

HRESULT WINAPI HookedDrawIndexed(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,INT base,UINT min,UINT vertices,UINT start,UINT n) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    if(volume::internal||weathervisuals::internal)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawIndexedPrimitive",t,n);
    if(unified&&!graphicsActive)return originalDrawIndexed(d,t,base,min,vertices,start,n);
    if(!g_drawingOverlay){weathervisuals::ObserveWorldDraw();nativeshadowdiag::OnDraw(d,"DrawIndexedPrimitive",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);}
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    const bool updateShadows=!g_drawingOverlay&&volume::ShouldUpdateShadows();
    const bool captureGround=n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed;
    const bool needsWaterMaterial=updateShadows||captureGround||
        renderer::MaterialCacheManager::Instance().IsActive();
    const auto waterDraw=ObserveWaterForDraw(d,needsWaterMaterial);
    if(updateShadows){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        volume::ShadowDraw(d,dc,[&]{return originalDrawIndexed(d,t,base,min,vertices,start,n);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d,waterDraw.available?&waterDraw.material:nullptr);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawIndexedPrimitive",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,base,start,n);
    if(captureGround){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{return originalDrawIndexed(d,t,base,min,vertices,start,n);},waterDraw.available?&waterDraw.material:nullptr);volume::internal=false;}
    vegetation::Scope vegetationSway(d,g_drawingOverlay,
        vegetation::IndexedDraw(t,base,min,vertices,start,n));
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed,
        waterDraw.available?&waterDraw.material:nullptr);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawIndexed(d,t,base,min,vertices,start,n);
    if(SUCCEEDED(hr)&&!g_drawingOverlay&&renderer::MaterialCacheManager::Instance().IsActive()){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        if(dc.material==renderer::MaterialType::Terrain){
            renderer::MaterialCacheManager::Instance().ApplyNormalModulation(d,t,base,min,vertices,start,n,[&]{return originalDrawIndexed(d,t,base,min,vertices,start,n);});
        }
        else if(dc.material==renderer::MaterialType::WMO||dc.material==renderer::MaterialType::M2){
            renderer::MaterialCacheManager::Instance().ApplyObjectMaterial(d,n,[&]{return originalDrawIndexed(d,t,base,min,vertices,start,n);});
        }
    }
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
}

HRESULT WINAPI HookedDrawUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT n,const void* v,UINT stride) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawUP(d,t,n,v,stride);
    if(volume::internal||weathervisuals::internal)return originalDrawUP(d,t,n,v,stride);
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawPrimitiveUP",t,n);
    if(unified&&!graphicsActive)return originalDrawUP(d,t,n,v,stride);
    if(!g_drawingOverlay)nativeshadowdiag::OnDraw(d,"DrawPrimitiveUP",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    const bool updateShadows=!g_drawingOverlay&&volume::ShouldUpdateShadows();
    const bool captureGround=n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed;
    const bool needsWaterMaterial=updateShadows||captureGround||
        renderer::MaterialCacheManager::Instance().IsActive();
    const auto waterDraw=ObserveWaterForDraw(d,needsWaterMaterial);
    if(updateShadows){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        volume::ShadowDraw(d,dc,[&]{return originalDrawUP(d,t,n,v,stride);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d,waterDraw.available?&waterDraw.material:nullptr);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawPrimitiveUP",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,0,n,false,v,stride);
    if(captureGround){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{return originalDrawUP(d,t,n,v,stride);},waterDraw.available?&waterDraw.material:nullptr);volume::internal=false;}
    vegetation::Scope vegetationSway(d,g_drawingOverlay,vegetation::PlainUpDraw(t,n,v,stride));
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed,
        waterDraw.available?&waterDraw.material:nullptr);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::ObserveWorldDraw();
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawUP(d,t,n,v,stride);
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
}

HRESULT WINAPI HookedDrawIndexedUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT min,UINT vertices,UINT n,const void* indices,D3DFORMAT f,const void* v,UINT stride) {
    // While the device is lost nothing can draw; pass through without any effect work.
    if(g_availability==renderer::DeviceAvailability::Lost)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    if(volume::internal||weathervisuals::internal)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    // Observe native state before effect setup, including with F11 OFF.
    if(!g_drawingOverlay)waterdiag::Draw(d,"DrawIndexedPrimitiveUP",t,n);
    if(unified&&!graphicsActive)return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    if(!g_drawingOverlay)nativeshadowdiag::OnDraw(d,"DrawIndexedPrimitiveUP",t,n,renderer::g_trackedState.vsHash,renderer::g_trackedState.psHash,renderer::g_trackedState.currentFVF,renderer::g_trackedState.currentVDecl,renderer::g_trackedState.alphaBlend,renderer::g_trackedState.alphaTest,renderer::g_trackedState.zEnable,renderer::g_trackedState.zWrite);
    if(!g_drawingOverlay)volume::BeforeDraw(d);
    if(!g_drawingOverlay)weathervisuals::BeforeDraw(d);
    const bool updateShadows=!g_drawingOverlay&&volume::ShouldUpdateShadows();
    const bool captureGround=n>0&&!g_drawingOverlay&&volume::ready&&!volume::composed;
    const bool needsWaterMaterial=updateShadows||captureGround||
        renderer::MaterialCacheManager::Instance().IsActive();
    const auto waterDraw=ObserveWaterForDraw(d,needsWaterMaterial);
    if(updateShadows){
        auto dc=ClassifyCurrentDraw(d,t,n,waterDraw.available?&waterDraw.material:nullptr);
        volume::ShadowDraw(d,dc,[&]{return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);});
    }
    if(!g_drawingOverlay)waterreflection::Prepare(d,waterDraw.available?&waterDraw.material:nullptr);
    if(!g_drawingOverlay)celestialdiag::Draw(d,"DrawIndexedPrimitiveUP",t,n);
    if(!g_drawingOverlay)renderer::CelestialTracker::Instance().Observe(d,n);
    if(!g_drawingOverlay)renderer::LocalLightManager::Instance().ObserveTorchDraw(d,t,0,0,n,true,v,stride,indices,f,min+vertices);
    if(captureGround){volume::internal=true;renderer::GroundSurfaceCapture::Instance().Observe(d,renderer::FrameContext::Current(),[&]{return originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);},waterDraw.available?&waterDraw.material:nullptr);volume::internal=false;}
    vegetation::Scope vegetationSway(d,g_drawingOverlay,
        vegetation::IndexedUpDraw(t,min,vertices,n,indices,f,v,stride));
    waterhighlight::Scope tint(d,g_drawingOverlay);
    celestialhighlight::Scope chl(d,n,g_drawingOverlay);
    if(!g_drawingOverlay)renderer::EnvironmentFogCapture::Instance().Observe(d);
    distancefog::Scope fog(d,g_drawingOverlay||tint.changed);
    watereffect::Scope water(d,g_drawingOverlay||tint.changed,
        waterDraw.available?&waterDraw.material:nullptr);
    nativeshadowdiag::SoftnessScope nativeShadow(d,renderer::g_trackedState.psHash,renderer::g_trackedState.currentPS);
    weathervisuals::ObserveWorldDraw();
    weathervisuals::NativeParticleScope weatherParticle(g_drawingOverlay?nullptr:d,n);
    HRESULT hr=originalDrawIndexedUP(d,t,min,vertices,n,indices,f,v,stride);
    if(!g_drawingOverlay)celestialdiag::AfterDraw(d);
    return hr;
}

void RestoreDeviceHooksAfterReset(IDirect3DDevice9* device)
{
    if(!device)return;
    void** table=*reinterpret_cast<void***>(device);
    DWORD oldProtect=0;
    if(!VirtualProtect(&table[16],95*sizeof(void*),PAGE_READWRITE,&oldProtect))return;

    InterlockedExchangePointer(&table[42],reinterpret_cast<void*>(HookedEndScene));
    InterlockedExchangePointer(&table[51],reinterpret_cast<void*>(HookedSetLight));
    InterlockedExchangePointer(&table[53],reinterpret_cast<void*>(HookedLightEnable));
    InterlockedExchangePointer(&table[37],reinterpret_cast<void*>(HookedSetRenderTarget));
    InterlockedExchangePointer(&table[57],reinterpret_cast<void*>(HookedSetRenderState));
    InterlockedExchangePointer(&table[65],reinterpret_cast<void*>(HookedSetTexture));
    InterlockedExchangePointer(&table[87],reinterpret_cast<void*>(HookedSetVertexDeclaration));
    InterlockedExchangePointer(&table[89],reinterpret_cast<void*>(HookedSetFVF));
    InterlockedExchangePointer(&table[92],reinterpret_cast<void*>(HookedSetVertexShader));
    InterlockedExchangePointer(&table[94],reinterpret_cast<void*>(HookedSetVertexShaderConstantF));
    InterlockedExchangePointer(&table[107],reinterpret_cast<void*>(HookedSetPixelShader));
    // Wind/weather observation and F7 need frame/draw hooks even when all
    // visual backends are disabled or a shader failed. Re-enabling is live.
    {
        InterlockedExchangePointer(&table[16],reinterpret_cast<void*>(HookedReset));
        InterlockedExchangePointer(&table[17],reinterpret_cast<void*>(HookedPresent));
        InterlockedExchangePointer(&table[81],reinterpret_cast<void*>(HookedDraw));
        InterlockedExchangePointer(&table[82],reinterpret_cast<void*>(HookedDrawIndexed));
        InterlockedExchangePointer(&table[83],reinterpret_cast<void*>(HookedDrawUP));
        InterlockedExchangePointer(&table[84],reinterpret_cast<void*>(HookedDrawIndexedUP));
    }
    if(volume::enabled||watereffect::enabled){
        InterlockedExchangePointer(&table[39],reinterpret_cast<void*>(volume::SetDepth));
        InterlockedExchangePointer(&table[40],reinterpret_cast<void*>(volume::GetDepth));
        InterlockedExchangePointer(&table[43],reinterpret_cast<void*>(HookedClear));
    }
    DWORD ignored=0;
    VirtualProtect(&table[16],95*sizeof(void*),oldProtect,&ignored);
    g_hookedTable=table;
}

void HookDevice(IDirect3DDevice9* device)
{
    if (!device)
        return;
    std::lock_guard lock(g_hookMutex);
    void** table = *reinterpret_cast<void***>(device);
    if (g_hookedTable)
    {
        if (g_hookedTable != table)
            Log("Different device implementation: leaving this device untouched.");
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(&table[16], 95*sizeof(void*), PAGE_READWRITE, &oldProtect))
        return;
    g_originalEndScene = reinterpret_cast<EndSceneFn>(table[42]);
    originalReset=reinterpret_cast<ResetFn>(table[16]);
    originalPresent=reinterpret_cast<PresentFn>(table[17]);
    originalSetRenderTarget=reinterpret_cast<SetRenderTargetFn>(table[37]);
    originalSetRenderState=reinterpret_cast<SetRenderStateFn>(table[57]);
    originalSetTexture=reinterpret_cast<SetTextureFn>(table[65]);
    originalDraw=reinterpret_cast<DrawFn>(table[81]);
    originalDrawIndexed=reinterpret_cast<DrawIndexedFn>(table[82]);
    originalDrawUP=reinterpret_cast<DrawUPFn>(table[83]);
    originalDrawIndexedUP=reinterpret_cast<DrawIndexedUPFn>(table[84]);
    originalSetVertexDeclaration=reinterpret_cast<SetVertexDeclarationFn>(table[87]);
    originalSetFVF=reinterpret_cast<SetFVFFn>(table[89]);
    originalSetVertexShader=reinterpret_cast<SetVertexShaderFn>(table[92]);
    originalSetVertexShaderConstantF=reinterpret_cast<SetVertexShaderConstantFFn>(table[94]);
    originalSetPixelShader=reinterpret_cast<SetPixelShaderFn>(table[107]);
    originalSetLight=reinterpret_cast<SetLightFn>(table[51]);
    originalLightEnable=reinterpret_cast<LightEnableFn>(table[53]);

    InterlockedExchangePointer(&table[42], reinterpret_cast<void*>(HookedEndScene));
    InterlockedExchangePointer(&table[51], reinterpret_cast<void*>(HookedSetLight));
    InterlockedExchangePointer(&table[53], reinterpret_cast<void*>(HookedLightEnable));
    InterlockedExchangePointer(&table[37], reinterpret_cast<void*>(HookedSetRenderTarget));
    InterlockedExchangePointer(&table[57], reinterpret_cast<void*>(HookedSetRenderState));
    InterlockedExchangePointer(&table[65], reinterpret_cast<void*>(HookedSetTexture));
    InterlockedExchangePointer(&table[87], reinterpret_cast<void*>(HookedSetVertexDeclaration));
    InterlockedExchangePointer(&table[89], reinterpret_cast<void*>(HookedSetFVF));
    InterlockedExchangePointer(&table[92], reinterpret_cast<void*>(HookedSetVertexShader));
    InterlockedExchangePointer(&table[94], reinterpret_cast<void*>(HookedSetVertexShaderConstantF));
    InterlockedExchangePointer(&table[107], reinterpret_cast<void*>(HookedSetPixelShader));

    // CPU wind and its F7 controls must not depend on a working visual
    // backend. Draw hooks retain native weather observation while effects
    // are off; each optional effect still checks its own rendering gates.
    {
        InterlockedExchangePointer(&table[16],reinterpret_cast<void*>(HookedReset));
        InterlockedExchangePointer(&table[17],reinterpret_cast<void*>(HookedPresent));
        InterlockedExchangePointer(&table[81],reinterpret_cast<void*>(HookedDraw));
        InterlockedExchangePointer(&table[82],reinterpret_cast<void*>(HookedDrawIndexed));
        InterlockedExchangePointer(&table[83],reinterpret_cast<void*>(HookedDrawUP));
        InterlockedExchangePointer(&table[84],reinterpret_cast<void*>(HookedDrawIndexedUP));
    }
    if(volume::enabled||watereffect::enabled){
        volume::setDepth=reinterpret_cast<volume::SetDepthFn>(table[39]);
        volume::getDepth=reinterpret_cast<volume::GetDepthFn>(table[40]);
        renderer::DepthCapture::Instance().SetFunctions(volume::setDepth, volume::getDepth);
        originalClear=reinterpret_cast<ClearFn>(table[43]);
        InterlockedExchangePointer(&table[39],reinterpret_cast<void*>(volume::SetDepth));
        InterlockedExchangePointer(&table[40],reinterpret_cast<void*>(volume::GetDepth));
        InterlockedExchangePointer(&table[43],reinterpret_cast<void*>(HookedClear));
    }
    DWORD ignored = 0;
    VirtualProtect(&table[16], 95*sizeof(void*), oldProtect, &ignored);
    g_hookedTable = table;
    Log("D3D9 device hooked with fast state tracking; see INI for atmosphere/diagnostics switches.");
}

class Direct3D9Proxy final : public IDirect3D9
{
public:
    explicit Direct3D9Proxy(IDirect3D9* real) : _real(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9))
        {
            *out = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG refs = --_refs; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override { return _real->RegisterSoftwareDevice(init); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return _real->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* id) override { return _real->GetAdapterIdentifier(a, f, id); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override { return _real->GetAdapterModeCount(a, f); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* d) override { return _real->EnumAdapterModes(a, f, m, d); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* d) override { return _real->GetAdapterDisplayMode(a, d); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT ad, D3DFORMAT bf, BOOL w) override { return _real->CheckDeviceType(a, t, ad, bf, w); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT c) override { return _real->CheckDeviceFormat(a, t, af, u, r, c); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD* q) override { return _real->CheckDeviceMultiSampleType(a, t, f, w, m, q); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT ds) override { return _real->CheckDepthStencilMatch(a, t, af, rf, ds); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) override { return _real->CheckDeviceFormatConversion(a, t, s, d); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* c) override { return _real->GetDeviceCaps(a, t, c); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override { return _real->GetAdapterMonitor(a); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE t, HWND w, DWORD flags, D3DPRESENT_PARAMETERS* p, IDirect3DDevice9** device) override
    {
        HRESULT result = _real->CreateDevice(a, t, w, flags, p, device);
        if (SUCCEEDED(result) && device && *device)
            HookDevice(*device);
        return result;
    }
private:
    ~Direct3D9Proxy() { _real->Release(); }
    IDirect3D9* _real;
    std::atomic<ULONG> _refs{1};
};
// Which build is running, as the loaded proxy DLL's own file time. It is what
// the debug overlay stamps its rows with: a report about an effect is a
// screenshot of those rows, and a screenshot could not previously say which
// d3d9.dll drew it - one in-client report arrived with counters that read
// exactly like the build before it, and nothing in the picture could tell the
// two apart. The file time also needs no version to keep in step with the
// source, and it is the one identity a player can check in Explorer.
std::string g_buildStamp;
void Initialize();
void PublishBuildStamp()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Initialize), &self))
        return;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, std::size(path)))
        return;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
        return;
    FILETIME local{};
    SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &time))
        return;
    char text[64]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
    g_buildStamp = text;
    renderer::buildStamp = g_buildStamp.c_str();
}

void Initialize()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, std::size(path));
    std::wstring exe(path);
    g_basePath = exe.substr(0, exe.find_last_of(L"\\/") + 1);
    PublishBuildStamp();
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, std::size(systemDirectory)))
        return;
    g_systemD3D9 = LoadLibraryW((std::wstring(systemDirectory) + L"\\d3d9.dll").c_str());
    if (!g_systemD3D9)
        return;
    g_realCreate9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(g_systemD3D9, "Direct3DCreate9"));
    g_realCreate9Ex = reinterpret_cast<Direct3DCreate9ExFn>(GetProcAddress(g_systemD3D9, "Direct3DCreate9Ex"));
    waterdiag::Configure(g_basePath);
    celestialdiag::Configure(g_basePath);
    waterhighlight::Configure(g_basePath);
    celestialhighlight::Configure(g_basePath);
    celestialhighlight::ReloadTuning(g_basePath);
    ReloadWindTuning();
    renderer::FrameContext::Current().wind=g_windSystem.Update({},0.f);
    watereffect::Configure(g_basePath);
    distancefog::Configure(g_basePath);
    volume::Configure(g_basePath);
    weathervisuals::Configure(g_basePath);
    waterreflection::Configure(g_basePath);
    tuningoverlay::Configure(g_basePath);
    renderer::EnvironmentProfileManager::Instance().Configure(g_basePath);
    nativeshadowdiag::Configure(g_basePath);
    renderer::MaterialCacheManager::Instance().Configure(g_basePath);
    renderer::LocalLightManager::Instance().Configure(g_basePath);
    renderer::LocalLightingRenderer::Instance().Configure(g_basePath);
    renderer::RendererDiagnostics::Instance().SetLogPath(g_basePath + L"ModernWoWRenderer.log");
    unified=GetPrivateProfileIntW(L"Graphics",L"UnifiedToggle",0,(g_basePath+L"ModernWoWRenderer.ini").c_str())!=0;
    if(unified){watereffect::hotkey=false;distancefog::hotkey=false;
        graphicsShowStatus=GetPrivateProfileIntW(L"Graphics",L"ShowStatus",0,(g_basePath+L"ModernWoWRenderer.ini").c_str())!=0;
        graphicsActive=GetPrivateProfileIntW(L"Graphics",L"Enabled",1,(g_basePath+L"ModernWoWRenderer.ini").c_str())!=0;
        watereffect::active=distancefog::active=volume::active=weathervisuals::active=graphicsActive;
    }
    Log("Modern WoW Renderer proxy loaded.");
    if (!g_buildStamp.empty())
        Log(("Build " + g_buildStamp + ": the debug overlay stamps this on its rows, so a screenshot names the build "
            "it came from.").c_str());
    // Device vtable entries remain hooked for the process lifetime.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Initialize), &pinned);
}
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    std::call_once(g_initialize, Initialize);
    if (!g_realCreate9) return nullptr;
    IDirect3D9* real = g_realCreate9(sdkVersion);
    return real ? new Direct3D9Proxy(real) : nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    std::call_once(g_initialize, Initialize);
    Log("Direct3D9Ex requested: passthrough only, effects unavailable.");
    return g_realCreate9Ex ? g_realCreate9Ex(sdkVersion, out) : D3DERR_NOTAVAILABLE;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
