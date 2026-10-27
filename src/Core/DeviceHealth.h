#pragma once
// Device-availability and frame-continuity rules shared by the render hooks.
//
// This header deliberately has no Windows or Direct3D includes so that the
// same rules can be exercised on any host by tools/DeviceHealthRegression.cpp.
// The constants below are the D3D9 SDK codes MAKE_D3DHRESULT(2152) and
// MAKE_D3DHRESULT(2153); ModernWoWRenderer.cpp static_asserts them against
// d3d9.h so a mismatch fails the build instead of silently mis-classifying.

#include <cstdint>

namespace renderer
{
    // Same bit pattern as the Windows HRESULT type (signed 32-bit).
    constexpr long kD3DErrDeviceLost = static_cast<long>(0x88760868u);
    constexpr long kD3DErrDeviceNotReset = static_cast<long>(0x88760869u);

    enum class DeviceAvailability : unsigned char
    {
        Operational,
        // Lost, or lost and waiting for IDirect3DDevice9::Reset. Nothing can
        // be drawn until the device reports itself operational again.
        Lost,
    };

    // Maps a Present/Reset/TestCooperativeLevel result onto availability.
    // Other failures are not device loss and must not pause the effects.
    constexpr DeviceAvailability ClassifyDeviceResult(long hr) noexcept
    {
        return (hr == kD3DErrDeviceLost || hr == kD3DErrDeviceNotReset)
            ? DeviceAvailability::Lost
            : DeviceAvailability::Operational;
    }

    // How long an optional pass waits before rebuilding its resources or shaders
    // after a failure. Short enough to recover within seconds of an Alt+Tab,
    // long enough that a persistent failure cannot recompile every frame.
    constexpr uint64_t kOptionalPassRetryMs = 2000;

    // Detects a break in the stream of presented frames (Alt+Tab, a minimised
    // window, a loading screen). The first observation counts as a break so
    // nothing produced before the detector started is reused.
    class FrameGapDetector
    {
    public:
        bool Observe(uint64_t now, uint64_t maxGapMs) noexcept
        {
            const bool gap = !m_primed || now < m_last || now - m_last > maxGapMs;
            m_last = now;
            m_primed = true;
            return gap;
        }

    private:
        uint64_t m_last = 0;
        bool m_primed = false;
    };

    // Retries an optional pass after a cooldown instead of disabling it until
    // the next device reset. A transient failure (for example a render target
    // that cannot be created while the window is switching) must not leave the
    // effect off for the rest of the session.
    class RetryGate
    {
    public:
        bool Open(uint64_t now) const noexcept
        {
            return !m_failed || now >= m_retryAt;
        }

        // Returns true for the first failure of a streak, so callers can log
        // once instead of on every retry.
        bool Fail(uint64_t now, uint64_t cooldownMs) noexcept
        {
            const bool first = !m_failed;
            m_failed = true;
            m_retryAt = now + cooldownMs;
            return first;
        }

        void Succeed() noexcept { m_failed = false; }
        bool Failed() const noexcept { return m_failed; }

    private:
        bool m_failed = false;
        uint64_t m_retryAt = 0;
    };

    // A full-depth capture proves a world frame when a scene-dependent pass
    // requested it. Waves-only rendering need not capture INTZ, so a known
    // terrain draw or an accepted liquid-material draw is independent world
    // evidence in that mode.
    constexpr bool HasWorldContext(bool capturedDepth, bool knownWorldDrawSeen) noexcept
    {
        return capturedDepth || knownWorldDrawSeen;
    }

    // The location provider refuses to read IDs without world context
    // (Sample(database, false) deliberately returns invalid). Focus does not
    // turn a UI/loading/no-depth frame into world context: feeding that invalid
    // sample to the debounce path eventually clears a valid zone and silently
    // disables its map/zone/area tuning. Hold until either trusted render signal
    // says this is a world frame, whether or not the game window has focus.
    //
    // A hold defends a confirmed location, so it needs one: with nothing confirmed yet
    // there is nothing to protect, and holding a no-depth frame also stalled the FIRST
    // read in any session that reached a login/character screen with no terrain draw and
    // no captured depth. The poll cadence and the world-frame precondition in the manager
    // still gate the sample itself, so relaxing the hold cannot make a UI frame resolve a
    // location; it only stops "no location yet" from becoming "no location ever".
    constexpr bool ShouldHoldLocation(bool worldFrame, bool haveConfirmedLocation) noexcept
    {
        return !worldFrame && haveConfirmedLocation;
    }

    // A valid camera pose is not proof that the client is in a playable world:
    // the login scene can also contain a full-frame sky/camera. On verified
    // clients, coherent map/zone IDs distinguish those states. Unknown clients
    // retain a conservative render-evidence fallback for effects that need it.
    enum class WorldSceneState : uint8_t
    {
        Unknown,
        InWorld,
        OutOfWorld,
    };

    struct WorldSceneObservation
    {
        WorldSceneState state = WorldSceneState::Unknown;
        uint32_t mapId = 0;
        uint32_t zoneId = 0;
        uint32_t areaId = 0;
        bool locationIdsValid = false;
    };

    constexpr WorldSceneObservation ClassifyWorldScene(
        bool verifiedProvider, bool coherentIds, uint32_t mapId, uint32_t zoneId,
        uint32_t areaId, bool knownWorldDrawSeen) noexcept
    {
        WorldSceneObservation out;
        constexpr uint32_t kLargestPlausibleWorldId = 1000000u;
        const bool plausible = mapId <= kLargestPlausibleWorldId &&
            zoneId <= kLargestPlausibleWorldId && areaId <= kLargestPlausibleWorldId;
        if (verifiedProvider && coherentIds && plausible)
        {
            if (zoneId == 0 && areaId == 0)
            {
                out.state = WorldSceneState::OutOfWorld;
                return out;
            }
            out.state = WorldSceneState::InWorld;
            out.mapId = mapId; // map 0 is a real WoW map; zone/area carry presence
            out.zoneId = zoneId;
            out.areaId = areaId;
            out.locationIdsValid = true;
            return out;
        }
        if (knownWorldDrawSeen) out.state = WorldSceneState::InWorld;
        return out;
    }

    // Shared render-safety policy for world effects that reconstruct or place
    // anything through the camera. A current terrain/liquid draw can identify an
    // unverified playable scene, and verified IDs can identify a sky-only view,
    // but neither makes a held camera basis safe: every path also needs a pose
    // accepted for this frame. A verified menu always wins.
    constexpr bool HasPlayableWorldEvidence(WorldSceneState state, bool knownWorldDrawSeen,
                                            bool freshCameraPose = false) noexcept
    {
        if (state == WorldSceneState::OutOfWorld) return false;
        return freshCameraPose && (knownWorldDrawSeen || state == WorldSceneState::InWorld);
    }

    // Sky-only views have no terrain/liquid draw, but verified world IDs plus a
    // fresh camera pose keep world-anchored clouds alive. World draws do not
    // bypass the freshness requirement either.
    constexpr bool CanRenderWorldClouds(WorldSceneState state, bool knownWorldDrawSeen,
                                        bool freshCameraPose = false) noexcept
    {
        return HasPlayableWorldEvidence(state, knownWorldDrawSeen, freshCameraPose);
    }

    // A separate emergency path for the visible sky only. It never substitutes
    // an old eye for a fresh world pose: verified in-world state and a current,
    // corroborated view orientation are mandatory, and a still-fresh complete
    // pose belongs to the normal world-space cloud pass instead.
    constexpr bool CanRenderSkyCloudFallback(WorldSceneState state,
                                             bool freshOrientation,
                                             bool freshCameraPose) noexcept
    {
        return state == WorldSceneState::InWorld && freshOrientation && !freshCameraPose;
    }

    // Remembered submersion must not survive leaving the client world, or a
    // confirmed map/zone transition. The first frame in the new location starts
    // clean, while ordinary camera turns inside one zone preserve the medium.
    class WorldSceneContinuity
    {
    public:
        bool Observe(const WorldSceneObservation& observation) noexcept
        {
            if (observation.state == WorldSceneState::OutOfWorld)
            {
                m_haveLocation = false;
                return true;
            }
            if (observation.state != WorldSceneState::InWorld || !observation.locationIdsValid)
                return false;
            const bool changed = m_haveLocation &&
                (observation.mapId != m_mapId || observation.zoneId != m_zoneId);
            m_mapId = observation.mapId;
            m_zoneId = observation.zoneId;
            m_haveLocation = true;
            return changed;
        }

        void Reset() noexcept
        {
            m_mapId = m_zoneId = 0;
            m_haveLocation = false;
        }

    private:
        uint32_t m_mapId = 0;
        uint32_t m_zoneId = 0;
        bool m_haveLocation = false;
    };
}
