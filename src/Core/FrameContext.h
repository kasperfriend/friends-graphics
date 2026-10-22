#pragma once
#include <d3d9.h>
#include <cstdint>
#include "MathTypes.h"
#include "DeviceHealth.h"
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

        // Current-frame scene evidence is separate from camera validity: sky
        // fallbacks can publish a perfectly usable camera on a login/menu scene.
        // The direct terrain/liquid flags are filled by their draw observers;
        // the observation is resolved from those flags and verified client IDs.
        bool worldTerrainSeenThisFrame = false;
        bool worldWaterSeenThisFrame = false;
        bool worldSceneResolved = false;
        WorldSceneObservation worldScene;
        // True when native water (because replacement is OFF or not ready)
        // wrote this frame's coverage mask. Atmosphere must preserve those
        // pixels instead of fogging the original water out at range.
        bool nativeWaterMaskCaptured = false;

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
        // The vanilla sun is a far world-space billboard. Keep its own tracked
        // position separate from the selected glare source so a moon chosen at
        // night can never move the sun's protection mask. The cloud composite
        // preserves only original scene pixels at this confirmed disc.
        float sunDiscScreenX = -1.0f;
        float sunDiscScreenY = -1.0f;
        float sunDiscViewDepth = 0.0f;
        bool sunDiscValid = false;
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
        // How far the camera is under the liquid, 0 in air and 1 a yard and a half under it:
        // renderer::underwater::Tracker's ramp, settled once per frame from the liquid draws'
        // own matrices (see WaterEffect.h, controls[75], and src/Water/UnderwaterMedium.h) and
        // published by the compositor before any pass reads it. Every whole-frame term that
        // integrates AIR fades out on this - the atmosphere's haze, height fog, mist and halo,
        // the cloud deck landing on a surface seen from underneath - and the term that
        // integrates WATER fades in on it, so crossing the surface moves one medium into the
        // other instead of swapping them. Zero on every frame that drew no liquid, which makes
        // it the identity for a machine that never goes swimming.
        float underwaterRamp = 0.f;
        // The signed yards of water over the camera the ramp was built from, positive under
        // the surface. Diagnostics and logs read this; passes read the ramp.
        float underwaterYards = -1.f;
        // The SAME ramp, as settled by the last frame that composed. Precipitation composites
        // itself during the client's draw stream - before this frame's water draws report their
        // planes and before the composite settles the medium - so it cannot read `underwaterRamp`
        // (ResetForNewFrame clears that at the top of the frame) or a value that is only computed
        // at Present. VolumeIntegration publishes this copy in BeforeClear, one frame behind,
        // which the ramp's own third-of-a-second smoothing makes invisible; the precipitation
        // stage fades its whole field out on it so a diver does not have rain drawing over the
        // water. Zero on every frame that settles above the surface, and above the surface it is
        // the identity every consumer has always had.
        float compositedUnderwaterRamp = 0.f;

        // WHICH CAMERA THIS FRAME'S WORLD-ANCHORED PASSES MAY USE. `cameraValid`
        // only says a pose was published; it does not say the pose is this
        // frame's. In a place where the terrain capture never latches - a city
        // with everything overhead culled, which is most of Orgrimmar from
        // street level - the frame falls back to the sky transform, and when
        // even that cannot be corroborated the previous frame's pose is held.
        // Drawing the deck or a torch from that pose is what "the clouds and the
        // dynamic lights go with the camera" looks like: they are anchored to a
        // camera the player already left. A pass that places something in WORLD
        // space must check this, and skip instead of drawing it in the wrong
        // place; screen-space passes that merely want a direction do not.
        bool cameraPoseStale = false;
        // Milliseconds since a draw actually carried the camera's EYE (a real
        // capture, not a rotation-only sky transform). Anything placed within a
        // few yards of the camera - torch emitters above all - needs a fresh
        // eye, where far-away anchoring only needs a fresh orientation.
        // About sixty frames at 60 Hz. Long enough to survive the gaps in the
        // draw stream that cities and interiors produce (capture is not the
        // only way to get an eye, but it is the only way to REFRESH one), and
        // short enough that a lamp is never put where the player stood a
        // second ago: at running speed that is a dozen yards.
        static constexpr float kCameraEyeHoldMs = 1000.f;
        float cameraEyeAgeMs = 0.f;
        // How the pose was obtained, for diagnostics and the regression
        // tools. 0 none, 1 captured, 2 sky fallback, 3 held previous frame.
        uint8_t cameraPoseSource = 0;
        enum CameraPoseSource : uint8_t { PoseNone = 0, PoseCaptured = 1, PoseSky = 2, PoseHeld = 3 };
        // Sky-fallback diagnostics: a zero count means no eligible current-frame
        // candidate survived capture filters; a nonzero count with no fresh pose
        // means corroboration/continuity intentionally kept it out.
        enum class SkyObservationResult : uint8_t
        {
            NotAttempted = 0,
            CameraUnavailable,
            ViewportUnavailable,
            NotFullFrame,
            TransformUnavailable,
            NonFiniteTransform,
            InvalidProjection,
            ViewportMismatch,
            NonRigidBasis,
            EyeIncompatible,
            D3DTransformCandidate,
            RejectedShaderCandidate,
            Captured
        };
        uint8_t skyPoseCandidateCount = 0;
        bool skyPoseDiscEvidence = false;
        SkyObservationResult skyObservationResult = SkyObservationResult::NotAttempted;

        const char* SkyObservationName() const noexcept
        {
            switch (skyObservationResult)
            {
            case SkyObservationResult::CameraUnavailable: return "camera-unavailable";
            case SkyObservationResult::ViewportUnavailable: return "viewport-unavailable";
            case SkyObservationResult::NotFullFrame: return "not-full-frame";
            case SkyObservationResult::TransformUnavailable: return "transform-unavailable";
            case SkyObservationResult::NonFiniteTransform: return "non-finite-transform";
            case SkyObservationResult::InvalidProjection: return "invalid-projection";
            case SkyObservationResult::ViewportMismatch: return "viewport-mismatch";
            case SkyObservationResult::NonRigidBasis: return "non-rigid-basis";
            case SkyObservationResult::EyeIncompatible: return "eye-incompatible";
            case SkyObservationResult::D3DTransformCandidate: return "d3d-transform-candidate";
            case SkyObservationResult::RejectedShaderCandidate: return "shader-candidate";
            case SkyObservationResult::Captured: return "captured";
            case SkyObservationResult::NotAttempted: default: return "not-attempted";
            }
        }

        bool depthAvailable = false;
        bool cameraValid = false;
        bool composed = false;

        // Is there a numerically usable pose, including the short retained-pose
        // window some screen/local-light passes deliberately support? This is not
        // the world-effect safety gate: world-anchored clouds, fog and weather
        // must also require `!cameraPoseStale` through HasPlayableWorldEvidence.
        bool PoseUsable() const
        {
            return cameraValid || (viewRawValid && cameraEyeAgeMs <= kCameraEyeHoldMs);
        }

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

        void ResetWorldSceneFrame() noexcept
        {
            worldTerrainSeenThisFrame = false;
            worldWaterSeenThisFrame = false;
            worldSceneResolved = false;
            worldScene = {};
        }

        void ResetForNewFrame(uint64_t newFrameIndex, IDirect3DDevice9* dev);
    };
}
