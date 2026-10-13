// Checks the performance-counter frame-delta rule that drives fog flow,
// cloud drift and the celestial stabilizers. Host-independent: it only
// includes src/Core/FrameClock.h, whose rule has no Windows dependency.
#include "../src/Core/FrameClock.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace renderer;

namespace
{
    void Require(bool value, const char* name)
    {
        if (!value) throw std::runtime_error(name);
        std::printf("PASS %s\n", name);
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
}

int main() try
{
    constexpr std::int64_t kFrequency = 10'000'000; // 100 ns ticks, like QPC

    {
        FrameClock clock;
        Require(!clock.Primed(), "clock starts unprimed");
        Require(Near(clock.Observe(5'000'000, kFrequency), 1.0f / 60.0f),
                "first frame reports a nominal 60 Hz delta");
        Require(clock.Primed(), "clock is primed after the first frame");
        // A steady 60 Hz cadence measures the same delta every frame:
        // animation integrated from it glides instead of stepping.
        for (int frame = 0; frame < 120; ++frame)
        {
            const float dt = clock.Observe(5'000'000 + (frame + 1) * 166'667, kFrequency);
            if (!Near(dt, 0.0166667f)) throw std::runtime_error("steady cadence measures a steady delta");
        }
        std::printf("PASS steady cadence measures a steady delta\n");
    }

    {
        // The failure this class exists to prevent: deltas quantized to a
        // coarse ~15.6 ms tick. A 144 Hz frame is ~69,444 QPC ticks; every
        // frame must measure that, not 0 ms with an occasional 16 ms lump.
        FrameClock clock;
        clock.Observe(0, kFrequency);
        for (int frame = 1; frame <= 288; ++frame)
        {
            const float dt = clock.Observe(frame * 69'444, kFrequency);
            if (!Near(dt, 0.0069444f)) throw std::runtime_error("high-refresh frames each measure their own delta");
        }
        std::printf("PASS high-refresh frames each measure their own delta\n");
    }

    {
        FrameClock clock;
        clock.Observe(1'000'000, kFrequency);
        Require(clock.Observe(1'000'000, kFrequency) == 0.0f,
                "a stalled counter reports zero, not a negative step");
        Require(clock.Observe(500'000, kFrequency) == 0.0f,
                "a backwards counter reports zero");
        // A five-second Alt+Tab pause is one bounded step, not five
        // seconds of fog and cloud motion in a single frame.
        Require(Near(clock.Observe(500'000 + 50'000'000, kFrequency), 0.1f),
                "a long pause clamps to a tenth of a second");
        Require(Near(clock.Observe(500'000 + 50'000'000 + 166'667, kFrequency), 0.0166667f),
                "the frame after a pause measures normally again");
    }

    {
        FrameClock clock;
        clock.Observe(0, kFrequency);
        clock.Reset();
        Require(!clock.Primed() && Near(clock.Observe(123, kFrequency), 1.0f / 60.0f),
                "reset re-primes the clock");
        Require(clock.Observe(123, 0) == 1.0f / 60.0f,
                "a missing counter frequency falls back to a nominal frame");
    }

    std::printf("ALL FRAME CLOCK TESTS PASSED\n");
    return 0;
}
catch (const std::exception& error)
{
    std::printf("FAIL %s\n", error.what());
    return 1;
}
