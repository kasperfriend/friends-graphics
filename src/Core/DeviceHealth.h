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
    constexpr bool ShouldHoldLocation(bool worldFrame) noexcept
    {
        return !worldFrame;
    }
}
