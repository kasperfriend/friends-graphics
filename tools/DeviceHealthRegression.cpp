// Host-independent checks for device recovery, celestial stabilization, and
// the row-vector matrix conventions used by tracked light and temporal passes.
#include "../src/Core/DeviceHealth.h"
#include "../src/D3D9/CelestialStability.h"

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

    {
        // CelestialTracker must undo the row-vector VIEW rotation, not apply
        // it a second time. A fixed world-space moon should produce different
        // view directions as the camera yaws, but always recover the same
        // world direction (the old column/row mix made the light rotate with
        // the camera and also drove cloud-shadow elevation toward zero).
        const Vec3 worldDirection = Normalize({.31f, -.47f, .82f});
        constexpr float yawPitchDegrees[][2] = {
            {-105.f, 0.f}, {-45.f, 25.f}, {0.f, 0.f}, {35.f, -40.f}, {90.f, 65.f}};
        bool invariant = true;
        for (const auto& angles : yawPitchDegrees)
        {
            const float yaw = angles[0] * 3.14159265358979f / 180.f;
            const float pitch = angles[1] * 3.14159265358979f / 180.f;
            const float cy = std::cos(yaw), sy = std::sin(yaw);
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            Matrix4 yawView, pitchView;
            yawView.m[0][0] = cy;  yawView.m[0][1] = sy;
            yawView.m[1][0] = -sy; yawView.m[1][1] = cy;
            pitchView.m[1][1] = cp;  pitchView.m[1][2] = sp;
            pitchView.m[2][1] = -sp; pitchView.m[2][2] = cp;
            const Matrix4 view = Matrix4::Multiply(yawView, pitchView);
            const Vec3 viewDirection = TransformDirectionRowVector(worldDirection, view.m);
            const Vec3 recovered = ViewDirectionToWorld(viewDirection, view.m);
            invariant = invariant && Dot(recovered, worldDirection) >= .99999f;
        }
        Require(invariant, "confirmed celestial world direction is invariant under camera yaw and pitch");

        // The temporal shader's previous-frame position transform must use
        // these same row-vector columns plus row-three translation. A 90
        // degree yaw and non-zero camera translation make a row/column swap
        // obvious rather than accidentally passing under an identity view.
        Matrix4 translatedView;
        translatedView.m[0][0] = 0.f; translatedView.m[0][1] = 1.f;
        translatedView.m[1][0] = -1.f; translatedView.m[1][1] = 0.f;
        translatedView.m[3][0] = -10.f; translatedView.m[3][1] = 5.f;
        translatedView.m[3][2] = -2.f;
        const Vec3 viewPoint = TransformPointRowVector({2.f, 3.f, 4.f}, translatedView.m);
        Require(std::fabs(viewPoint.x + 13.f) < 1e-5f &&
                std::fabs(viewPoint.y - 7.f) < 1e-5f &&
                std::fabs(viewPoint.z - 2.f) < 1e-5f,
                "row-vector view transform uses matrix columns and translation");
    }

    {
        CelestialDirectionStabilizer direction;
        const Vec3 sun = Normalize({.6f, .2f, .8f});
        auto stable = direction.Update({1, 0, .04f}, &sun, false, 1.f/60.f);
        Require(Length(stable-sun)<1e-6f, "confirmed world direction primes the sun");
        for (int i=0;i<600;++i)
        {
            const Vec3 fallback = i%2 ? Vec3{1,0,.04f} : Vec3{-1,0,.8f};
            stable=direction.Update(fallback, nullptr, false, 1.f/60.f);
            if (Length(stable-sun)>=1e-6f) throw std::runtime_error("camera fallback moved sunlight");
        }
        Require(Length(stable-sun)<1e-6f, "camera-dependent fallback cannot move confirmed sunlight");
        const Vec3 moon=Normalize({-.2f,-.6f,.7f});
        Require(Length(direction.Update({}, &moon, true, .016f)-moon)<1e-6f,
                "moon keeps a separate confirmed direction");
        Require(Length(direction.Update({}, nullptr, false, .016f)-sun)<1e-6f,
                "returning to sunlight restores its own direction");
        const Vec3 moved=Normalize({.7f,.2f,.7f});
        stable=direction.Update({}, &moved, false, .016f);
        Require(Length(stable-sun)<Length(moved-sun) && std::fabs(Length(stable)-1.f)<1e-5f,
                "real celestial movement is smoothed and normalized");
        direction.Reset();
        Require(Length(direction.Update({0,0,1}, nullptr, false, .016f)-Vec3{0,0,1})<1e-6f,
                "loading reset allows a fresh world direction");
    }

    {
        // Before the tracker has ever confirmed a body there is no world-space
        // truth to hold, and the fallback is derived from the frame's own
        // light constant - so it moves with the camera. It may steer the deck
        // only as fast as a real body could: a camera swing is not a sunset.
        CelestialDirectionStabilizer direction;
        const Vec3 start = Normalize({0, 0, 1});
        direction.Update(start, nullptr, false, 1.f/60.f);
        const Vec3 swung = Normalize({1, 0, 0});          // 90 degrees in one frame
        const float moved = std::acos(std::clamp(Dot(direction.Update(swung, nullptr, false, 1.f/60.f), start), -1.f, 1.f));
        Require(moved < 3.f * 3.14159265358979f / 180.f,
                "a camera-sized swing of the fallback barely moves the light");
        // The limit is exact: ten seconds of a 90-degree-wrong fallback moves
        // the light by ten seconds of the fastest real celestial motion, and
        // no more, however long the source keeps insisting.
        for (int i = 0; i < 600; ++i) direction.Update(swung, nullptr, false, 1.f/60.f);
        const Vec3 arrived = direction.Update(swung, nullptr, false, 1.f/60.f);
        const float travelled = std::acos(std::clamp(Dot(arrived, start), -1.f, 1.f)) * 180.f / 3.14159265358979f;
        Require(travelled > 22.f && travelled < 28.f,
                "a wrong fallback can only steer the light at the celestial rate");

        // One frame of the limit is the threshold: a disagreement inside it is
        // resolved at once, so an honest source is not lagged by the cap.
        CelestialDirectionStabilizer close;
        const Vec3 near = Normalize({0.f, .0003f, 1.f});   // ~0.017 degrees away
        close.Update(start, nullptr, false, 1.f/60.f);
        Require(std::acos(std::clamp(Dot(close.Update(near, nullptr, false, 1.f/60.f), near), -1.f, 1.f)) < 1e-4f,
                "a fallback inside one frame of the celestial rate is followed at once");

        // A real, slow drift of the same source is followed as it happens.
        CelestialDirectionStabilizer drift;
        drift.Update(start, nullptr, false, 1.f/60.f);
        const Vec3 target = Normalize({.2f, 0, 1});
        for (int i = 0; i < 600; ++i) drift.Update(target, nullptr, false, 1.f/60.f);
        const float driftAngle = std::acos(std::clamp(Dot(drift.Update(target, nullptr, false, 1.f/60.f), start), -1.f, 1.f));
        Require(driftAngle > .01f, "slow real celestial motion is still tracked before confirmation");
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
