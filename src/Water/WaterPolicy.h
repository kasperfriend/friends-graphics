#pragma once
#include <cstdint>

namespace renderer::water
{
    // Captured native water families (including near-camera tiles). The pixel
    // shader is the material family; the vertex shader is NOT a stable part of
    // the liquid's identity: the client swaps vertex variants by camera
    // distance and shadow settings while keeping the same pixel shader (see
    // src/Core/ShaderCache.cpp, found with a WaterDiag capture). Requiring both
    // roles at a noncanonical texture layout is what left whole tiles - most
    // visibly the near-camera ones - with native water.
    //
    // Roles are recorded in WaterEffect/DrawCallClassifier at 11fd29e; the
    // older ShaderCache list mixed both stages in a single isKnownWater flag.
    constexpr bool IsKnownVertexShader(uint64_t hash)
    {
        return hash == 0x206d861fd0a721ddull ||
               hash == 0xfdd9528ed3ac30eaull || hash == 0x48a82796bd612aebull ||
               // Documented in TODO.md from live logs of the Ascension client
               // (the "fourth VS variant" whose rejection produced triangular
               // canal/coast holes and angle-dependent missing chunks) but
               // never actually added to this list.
               hash == 0x70faf83955e2b668ull;
    }

    constexpr bool IsKnownPixelShader(uint64_t hash)
    {
        return hash == 0x17f042a7906ca126ull || hash == 0x7d4f078fa1876a09ull;
    }

    // Every liquid shader family above feeds its fog curve through the same
    // vertex-shader register, verified from captured instructions for
    // 0x206d861fd0a721dd. Both fog paths used to list only that one hash, so
    // tiles drawn with a sibling vertex variant faded at a different distance
    // than the water they are continuous with - visible as a hard-edged square.
    // Consumers must still validate the register's contents (the callers'
    // x<=0, y>=0, z>0 shape guard) before using it.
    constexpr unsigned FogConstantRegister(uint64_t vertexShaderHash)
    {
        return IsKnownVertexShader(vertexShaderHash) ? 4u : 0u;
    }

    // Materials that must never be restyled as water, even when they bind the
    // liquid ripple lookup. The native precipitation vertex shaders are listed
    // here because that lookup is shared with them; they were captured from the
    // same Ascension client by WeatherVisuals' particle scope.
    constexpr bool IsExcludedShader(uint64_t hash)
    {
        return hash == 0xd9e7756460af6296ull || // UI vertex shader
               hash == 0xc29c7060b723c0c6ull || // UI pixel shader
               hash == 0x8832bcc5ed4ff2a6ull || // native rain/sandstorm vertex shader
               hash == 0xec9ee827b8bf5afaull;   // native snow vertex shader
    }

    constexpr bool IsRippleLookup(unsigned width, unsigned height)
    {
        // Base lookup and every possible reduced top mip. Texture quality can
        // remove top levels at load time; GetLevelDesc(0) then reports that mip.
        for (unsigned w = 8, h = 64; h; w = w > 1 ? w / 2 : 1, h /= 2)
            if (width == w && height == h) return true;
        return false;
    }

    constexpr bool IsSquarePowerOfTwo(unsigned width, unsigned height)
    {
        return width != 0 && width == height && (width & (width - 1)) == 0;
    }

    constexpr bool IsMaterialLayout(unsigned width0, unsigned height0,
                                    unsigned width1, unsigned height1,
                                    uint64_t vsHash = 0, uint64_t psHash = 0)
    {
        // The layout captured from the client, accepted without any shader
        // identity so that unknown tile variants keep working.
        if (width0 == 8 && height0 == 64 && width1 == 512 && height1 == 512)
            return true;
        if (IsExcludedShader(vsHash) || IsExcludedShader(psHash) ||
            !IsRippleLookup(width0, height0) ||
            !IsSquarePowerOfTwo(width1, height1))
            return false;
        // A documented liquid pixel shader plus the liquid two-texture
        // signature is the material. Do NOT accept the lookup on its own: it is
        // shared with weather, and square atlases are everywhere.
        return IsKnownPixelShader(psHash);
    }

    // A draw that carries the liquid texture signature but is not approved.
    // Reported once per distinct signature so an unmatched water family in a
    // player's log names the shader hashes instead of staying invisible.
    constexpr bool IsLiquidCandidate(unsigned width0, unsigned height0,
                                     unsigned width1, unsigned height1)
    {
        return IsRippleLookup(width0, height0) && IsSquarePowerOfTwo(width1, height1);
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
