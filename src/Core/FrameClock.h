#pragma once
// Frame timing for the animated effects (fog flow, cloud drift, wakes,
// stabilizers).
//
// This header deliberately keeps its rule free of Windows or Direct3D
// includes (the same arrangement as src/Core/DeviceHealth.h) so the rule
// can be exercised on any host by tools/FrameClockRegression.cpp. The
// QueryPerformanceCounter plumbing lives below the _WIN32 guard and is
// only used by the Windows translation units that drive the device.
//
// Why it exists: the effects used to measure frame time with
// GetTickCount64. That clock only advances in system-timer steps of
// roughly 15.6 ms unless some other process has raised the timer
// resolution, so at a 60 Hz refresh the measured delta alternated between
// 0 ms (clamped up to a 1 ms floor) and ~16 ms, and at high refresh rates
// most frames measured no time at all followed by one frame that measured
// a whole tick. Every quantity integrated from that delta - fog noise
// drift, cloud wind, wake decay, the celestial stabilizers - therefore
// advanced in visible steps instead of gliding: the whole screen's haze
// pulsed very slightly even with the camera perfectly still. The
// performance counter advances in sub-microsecond steps, so animation
// integrated from it is smooth at any refresh rate.

#include <cstdint>

namespace renderer
{
    // Turns performance-counter readings into a frame delta in seconds.
    // The first observation reports a nominal 60 Hz frame (there is no
    // previous frame to measure against), a stalled or backwards counter
    // reports zero rather than a negative step, and a long pause (Alt+Tab,
    // loading screen) is clamped to a tenth of a second so the first frame
    // back takes one bounded step instead of teleporting every animated
    // layer - temporal history invalidation handles the rest of a break.
    class FrameClock
    {
    public:
        float Observe(std::int64_t now, std::int64_t frequency) noexcept
        {
            if (frequency <= 0) return 1.0f / 60.0f;
            if (!m_primed)
            {
                m_primed = true;
                m_last = now;
                return 1.0f / 60.0f;
            }
            const std::int64_t elapsed = now - m_last;
            m_last = now;
            if (elapsed <= 0) return 0.0f;
            const double seconds = static_cast<double>(elapsed) /
                                   static_cast<double>(frequency);
            return static_cast<float>(seconds > 0.1 ? 0.1 : seconds);
        }

        void Reset() noexcept
        {
            m_primed = false;
            m_last = 0;
        }

        bool Primed() const noexcept { return m_primed; }

    private:
        std::int64_t m_last = 0;
        bool m_primed = false;
    };

#if defined(_WIN32)
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace renderer
{
    inline std::int64_t PerformanceCounter() noexcept
    {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }

    inline std::int64_t PerformanceFrequency() noexcept
    {
        static const std::int64_t frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return frequency;
    }

    // Seconds on the same counter, for shader time constants that used to
    // be derived from GetTickCount64 and therefore advanced in ~15.6 ms
    // steps (ripples and fog detail visibly ticking instead of flowing).
    inline double PerformanceSeconds() noexcept
    {
        const std::int64_t frequency = PerformanceFrequency();
        return frequency > 0
            ? static_cast<double>(PerformanceCounter()) /
                  static_cast<double>(frequency)
            : 0.0;
    }
#endif
}
