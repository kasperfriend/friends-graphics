// Checks the device-loss, frame-gap, retry and location-hold rules used by the
// Alt+Tab recovery path. Host-independent: it only includes src/Core/DeviceHealth.h.
#include "../src/Core/DeviceHealth.h"
#include "../src/D3D9/CelestialStability.h"

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
}

int main() try
{
    // D3D9 codes, as declared in the SDK's d3d9.h (MAKE_D3DHRESULT(2152/2153)).
    Require(kD3DErrDeviceLost == static_cast<long>(0x88760868u), "device-lost code matches SDK");
    Require(kD3DErrDeviceNotReset == static_cast<long>(0x88760869u), "device-not-reset code matches SDK");

    Require(ClassifyDeviceResult(0) == DeviceAvailability::Operational, "S_OK keeps effects running");
    Require(ClassifyDeviceResult(kD3DErrDeviceLost) == DeviceAvailability::Lost, "device lost pauses effects");
    Require(ClassifyDeviceResult(kD3DErrDeviceNotReset) == DeviceAvailability::Lost, "device awaiting reset pauses effects");
    Require(ClassifyDeviceResult(static_cast<long>(0x8876086Cu)) == DeviceAvailability::Operational,
            "invalid call is not treated as device loss");
    Require(ClassifyDeviceResult(static_cast<long>(0x8876017Cu)) == DeviceAvailability::Operational,
            "out-of-video-memory is not treated as device loss");

    {
        FrameGapDetector gaps;
        Require(gaps.Observe(10000, 1000), "first frame counts as a break");
        Require(!gaps.Observe(10016, 1000), "normal 60 Hz cadence is continuous");
        Require(!gaps.Observe(11016, 1000), "gap equal to the limit is still continuous");
        Require(gaps.Observe(13000, 1000), "Alt+Tab-length gap is a break");
        Require(!gaps.Observe(13016, 1000), "continuity resumes after a break");
        Require(gaps.Observe(100, 1000), "clock moving backwards is a break");
    }

    {
        RetryGate gate;
        Require(gate.Open(0), "retry gate starts open");
        Require(gate.Fail(1000, 2000), "first failure is reported");
        Require(!gate.Open(1500), "failed pass waits out the cooldown");
        Require(!gate.Fail(1500, 2000), "repeated failure is not reported as new");
        Require(gate.Open(3500), "pass is retried after the cooldown");
        gate.Succeed();
        Require(!gate.Failed() && gate.Open(3501), "success clears the failure streak");
        Require(gate.Fail(4000, 2000), "a new streak after recovery is reported again");
    }

    {
        // The disc-draw signal blinking must not blink the sky lighting.
        CelestialLightStabilizer light;
        CelestialLightInput frame;
        frame.deltaSeconds = 1.0f / 60.0f;
        frame.daylight = 0.8f;
        frame.discVisible = true;
        Require(light.Update(frame).intensity == 1.0f, "confirmed disc primes direct light at full strength");
        // Disc culled by the camera turn / missed for a frame, still day:
        // the clouds stay lit at exactly the same strength.
        frame.discVisible = false;
        Require(light.Update(frame).intensity == 1.0f, "off-screen disc keeps clouds lit during the day");
        for (int i = 0; i < 120; ++i) light.Update(frame);
        Require(light.Update(frame).intensity == 1.0f, "direct light does not decay while daylight remains");
        // Day ends: the light fades out over frames instead of stepping,
        // and is gone once the transition has had time to settle.
        frame.daylight = 0.0f;
        const float fading = light.Update(frame).intensity;
        Require(fading > 0.0f && fading < 1.0f, "sunset fades direct light instead of stepping it off");
        for (int i = 0; i < 600; ++i) light.Update(frame);
        Require(light.Update(frame).intensity == 0.0f, "direct light is off once night has settled");
        // A tracker dropout at night must not resurrect the sun, and a
        // confirmed moon selects the moon instead of the sun.
        frame.moonlight = 0.6f;
        frame.daylight = 0.0f;
        for (int i = 0; i < 600; ++i) light.Update(frame);
        auto night = light.Update(frame);
        Require(night.intensity == 1.0f && night.isMoon, "moonlight period selects the moon for direct light");
        frame.discVisible = true;
        frame.discIsMoon = false;
        auto disc = light.Update(frame);
        Require(disc.intensity == 1.0f && !disc.isMoon, "a confirmed disc overrides the day/night guess");

        // A confirmed body is sticky: tint heuristics alone must not flip
        // a bright noon to moonlight (which would step the moon-strength
        // scaling in full view), while a genuinely dim opposite period can.
        CelestialLightStabilizer sticky;
        CelestialLightInput noon;
        noon.deltaSeconds = 1.0f / 60.0f;
        noon.discVisible = true;
        noon.daylight = 0.8f;
        sticky.Update(noon);
        noon.discVisible = false;
        noon.daylight = 0.0f;
        noon.moonlight = 0.9f;
        noon.directionalLuminance = 1.0f;
        for (int i = 0; i < 600; ++i) sticky.Update(noon);
        Require(!sticky.Update(noon).isMoon, "bright noon is not reclassified as moonlight");
        noon.directionalLuminance = 0.3f;
        for (int i = 0; i < 600; ++i) sticky.Update(noon);
        Require(sticky.Update(noon).isMoon, "a dim opposite period takes over from the confirmed body");
    }

    Require(CloudShadowElevationFade(0.0f) == 0.0f, "cloud shadows are off for a disc below the horizon");
    Require(CloudShadowElevationFade(0.03f) == 0.0f, "cloud shadows fade in from the horizon gate");
    Require(CloudShadowElevationFade(0.15f) == 1.0f, "cloud shadows reach full strength above the fade band");
    Require(CloudShadowElevationFade(0.09f) > 0.0f && CloudShadowElevationFade(0.09f) < 1.0f,
            "cloud shadow strength is continuous inside the fade band");
    Require(CloudShadowElevationFade(1.0f) == 1.0f, "an overhead disc casts full cloud shadows");

    Require(ShouldHoldLocation(false, false), "unfocused frame without world keeps location");
    Require(!ShouldHoldLocation(true, false), "unfocused frame with world still updates location");
    Require(!ShouldHoldLocation(false, true), "focused frame without world keeps the normal zone rules");
    Require(!ShouldHoldLocation(true, true), "focused world frame updates location");

    std::printf("ALL DEVICE HEALTH TESTS PASSED\n");
    return 0;
}
catch (const std::exception& error)
{
    std::printf("FAIL %s\n", error.what());
    return 1;
}
