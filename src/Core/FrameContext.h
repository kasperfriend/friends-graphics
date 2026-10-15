#pragma once
#include <d3d9.h>
#include <cstdint>
#include "MathTypes.h"
#include "../Environment/EnvironmentProfile.h"
#include "../Environment/WindSystem.h"

namespace renderer
{
    struct FrameContext
    {
        static FrameContext& Current();

        ResolvedEnvironmentState environment;
        // Published after ALL rendering in Present, then immutable throughout
        // the next frame, including the pre-UI and fallback cloud paths.
        // Kept across camera/depth resets; wind is not a GPU resource.
        WindState wind;
        uint64_t frameIndex = 0;
        IDirect3DDevice9* device = nullptr;

        uint32_t width = 0;
        uint32_t height = 0;
        D3DVIEWPORT9 viewport{};

        Matrix4 view;
        Matrix4 projection;
        Matrix4 viewProjection;
        Matrix4 inverseView;

        Vec3 cameraPosition;

        Vec3 sunDirectionView;
        Vec3 sunDirectionWorld;
        Vec3 directionalLightColor;

        float daylightFactor = 0.0f;
        float moonlightFactor = 0.0f;
        float shadowLightFactor = 0.0f;

        // Coordinates of the sun projected on screen [0..1]
        float sunScreenX = -1.0f;
        float sunScreenY = -1.0f;
        float sunStrength = 0.0f;
        // The real camera-relative moon billboard, captured only when its draw
        // is visible this frame. Clouds match its screen centre and view depth so
        // the sprite does not act like an opaque wall in the scene depth buffer.
        float moonDiscScreenX = -1.0f;
        float moonDiscScreenY = -1.0f;
        float moonDiscViewDepth = 0.0f;
        bool moonDiscValid = false;
        // Confirmed CelestialTracker source used by the atmosphere path.
        // celestialIsMoon is deliberately explicit: colour is never used to
        // guess which body is active.
        float celestialIntensity = 0.0f;
        bool celestialIsMoon = false;
        // Stable direct light for clouds/atmosphere, decoupled from the
        // disc-draw signal above: `celestialIntensity` means "the disc was
        // drawn on screen this frame" and remains the god-ray/glare origin
        // gate, while these fields mean "the dominant body is up" and stay
        // lit when the disc is off screen, edge-culled or behind the cloud
        // deck itself. Filled by CelestialLightStabilizer in the compositor
        // (see src/D3D9/CelestialStability.h); directLightValid is false on
        // frames built outside that path (tests, diagnostics), where the
        // legacy celestial fields remain the fallback.
        float directLightIntensity = 0.0f;
        bool directLightIsMoon = false;
        bool directLightValid = false;
        Vec3 directLightDirectionView;
        Vec3 directLightDirectionWorld;

        // Projection reconstruction parameters [P22, P32, P00, P11]
        float projUnpack[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float depthMaxZ = 1.0f;

        // Raw forward (world->view) transform, rows exactly as the game's
        // vertex shader constants store them (row 3 = translation; same
        // row-vector convention used throughout this codebase). CameraCapture
        // swaps viewRaw into previousViewRaw before overwriting it with each
        // new frame's value, so by the time this frame's rendering reads
        // previousViewRaw it holds LAST frame's transform - used for ray/fog
        // temporal reprojection: transforming this frame's reconstructed
        // world position by previousViewRaw (+ the projection, which is
        // assumed FOV-stable frame to frame) gives where that point
        // appeared on screen last frame, so history is sampled at the
        // correct motion-compensated UV instead of naively at the same UV.
        Matrix4 viewRaw;
        bool viewRawValid = false;
        Matrix4 previousViewRaw;
        bool previousViewValid = false;

        // Directional shadow map data
        Matrix4 shadowMatrix;
        Matrix4 viewToShadowMatrix;
        IDirect3DTexture9* shadowTexture = nullptr;
        bool shadowMapValid = false;
        uint32_t shadowMapSize = 2048;
        float shadowMapDistance = 180.0f;

        IDirect3DTexture9* depthTexture = nullptr;
        IDirect3DSurface9* depthSurface = nullptr;
        IDirect3DTexture9* sceneColor = nullptr;
        IDirect3DSurface9* sceneSurface = nullptr;
        IDirect3DTexture9* atmosphereNoise = nullptr;
        // Published only after a successful cloud pass in this frame.
        IDirect3DTexture9* cloudTexture = nullptr;
        IDirect3DTexture9* cloudShadowTexture = nullptr;
        Vec4 cloudShadowArea; // origin XY, reciprocal extent, elevation fade
        Vec4 cloudLightProjection; // light XY/Z, base altitude, thickness
        float cloudRayStrength = 0.f;
        // Per-pixel "how much of this pixel is water" (0..1) and the water
        // surface's own view-space depth, written by watereffect::Scope as
        // extra render targets during the actual water draws this frame
        // (see WaterEffect.h). The main depth buffer holds whatever is
        // BEHIND water (water doesn't write it), so the atmosphere pass
        // uses these to march air only down to the water SURFACE instead
        // of through the water to the seabed - see
        // DirectionalVolumetricLighting.cpp's boundary-depth pass.
        IDirect3DTexture9* waterMaskTexture = nullptr;
        IDirect3DTexture9* waterDepthTexture = nullptr;

        bool depthAvailable = false;
        bool cameraValid = false;
        bool composed = false;

        // Sampled once per frame at the top of Present: is WoW the
        // foreground window? Every hotkey (F7-F12) reads this instead of
        // calling GetForegroundWindow/GetWindowThreadProcessId itself -
        // ten separate samples per frame could disagree while a focus
        // change lands mid-frame, which would fire some toggles and
        // swallow others in the same frame. The window handle is kept
        // alongside the flag so the F7 overlay measures the SAME window
        // the focus decision was made from (the original code used one
        // GetForegroundWindow for both).
        bool wowFocused = false;
        HWND wowForeground = nullptr;

        void ResetForNewFrame(uint64_t newFrameIndex, IDirect3DDevice9* dev);
    };
}
