#pragma once
#include <cstdint>

namespace renderer::water
{
    // Captured native water families (including near-camera tiles). Keep the
    // vertex and pixel roles separate: one known shader is not enough.
    constexpr bool IsKnownVertexShader(uint64_t hash)
    {
        return hash == 0x17f042a7906ca126ull || hash == 0x206d861fd0a721ddull ||
               hash == 0xfdd9528ed3ac30eaull || hash == 0x48a82796bd612aebull;
    }

    constexpr bool IsKnownPixelShader(uint64_t hash)
    {
        return hash == 0x7d4f078fa1876a09ull;
    }

    constexpr bool IsRippleLookup(unsigned width, unsigned height)
    {
        // Base lookup and every possible reduced top mip. Texture quality can
        // remove top levels at load time; GetLevelDesc(0) then reports that mip.
        for (unsigned w = 8, h = 64; h; w = w > 1 ? w / 2 : 1, h /= 2)
            if (width == w && height == h) return true;
        return false;
    }

    constexpr bool IsMaterialLayout(unsigned width0, unsigned height0,
                                    unsigned width1, unsigned height1,
                                    uint64_t vsHash = 0, uint64_t psHash = 0)
    {
        // Preserve support for as-yet unknown tile shader variants at the
        // original layout. Do NOT accept arbitrary square textures on the
        // ripple lookup alone: weather particles use that lookup too.
        if (width0 == 8 && height0 == 64 && width1 == 512 && height1 == 512)
            return true;
        const bool squarePowerOfTwo = width1 && width1 == height1 &&
                                      (width1 & (width1 - 1)) == 0;
        return IsKnownVertexShader(vsHash) && IsKnownPixelShader(psHash) &&
               IsRippleLookup(width0, height0) && squarePowerOfTwo;
    }

    constexpr bool NeedsScene(bool reflections, bool refraction, bool depth,
                              bool foam, bool sunGlint)
    {
        return reflections || refraction || depth || foam || sunGlint;
    }

    constexpr bool TargetsMatch(unsigned width, unsigned height,
                                unsigned otherWidth, unsigned otherHeight)
    {
        return width != 0 && height != 0 && width == otherWidth && height == otherHeight;
    }
}
