#pragma once

namespace renderer::water
{
    // The ripple lookup alone also occurs on weather particles. Both slots
    // are required; shader hashes vary between otherwise identical tiles.
    constexpr bool IsMaterialLayout(unsigned width0, unsigned height0,
                                    unsigned width1, unsigned height1)
    {
        return width0 == 8 && height0 == 64 && width1 == 512 && height1 == 512;
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
