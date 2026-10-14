#pragma once
#include <d3d9.h>
#include <cstdint>
#include "../Core/MathTypes.h"

namespace renderer
{
    // Result of intercepting the actual WoW sun/moon billboard draw call
    // this frame - NOT a projection of a lighting vector, and NOT a read
    // from process memory. Both prior approaches were tried and disproven
    // (see PR discussion); this is built from live D3D9 state captured at
    // the exact draw call that paints the visible disc, confirmed with a
    // crosshair pixel-diff tool before this was written.
    //
    // `visible` is true only when that draw call actually happened THIS
    // frame - if the body isn't being rendered (below horizon, culled,
    // whatever WoW's own logic decides), this naturally reports invisible
    // rather than guessing a screen position anyway.
    struct CelestialBody
    {
        bool visible = false;
        Vec3 viewSpaceDirection{};   // normalized, view space
        Vec3 worldSpaceDirection{};  // normalized, world space
        float screenX = -1.0f;
        float screenY = -1.0f;
        uint64_t lastSeenFrame = 0;
    };

    // Two structurally different draw patterns were found (crosshair-
    // confirmed against the actually visible disc in both cases):
    //
    //   SUN:  fixed-function, WORLD has a large scale (it's a big
    //         billboard), VIEW has (near) zero translation - the classic
    //         skybox trick: camera position doesn't move it, only camera
    //         rotation does.
    //   MOON: fixed-function, WORLD is (near) identity, VIEW has a small
    //         but non-zero translation whose xy components track the real
    //         direction as the camera rotates while z stays roughly
    //         constant (~8-16) - a camera-relative billboard placed along
    //         the true light direction at a fixed render distance.
    //
    // In both cases the screen-space projection is the same: transform the
    // origin by WORLD*VIEW to get a view-space position, then project with
    // that same draw's own D3DTS_PROJECTION. No manual offset or scale.
    class CelestialTracker
    {
    public:
        static CelestialTracker& Instance();

        // Call once per frame, before any draw calls for that frame -
        // invalidates last frame's sun/moon state so an object that
        // stopped being drawn (e.g. sun set) correctly reports invisible.
        void BeginFrame(uint64_t frameIndex);

        // Call for every draw call, before it executes. Cheap early-out
        // (two state queries) for the overwhelming majority of draws that
        // don't match either pattern.
        void Observe(IDirect3DDevice9* device, UINT primitiveCount);

        const CelestialBody& Sun() const { return m_sun; }
        const CelestialBody& Moon() const { return m_moon; }

    private:
        CelestialTracker() = default;

        CelestialBody m_sun;
        CelestialBody m_moon;
        uint64_t m_frameIndex = 0;
    };
}
