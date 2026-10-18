#pragma once

#include <cstdint>

namespace renderer::local_light_detail
{
    // Signature for same-frame consumers that cache transforms/constants for
    // the selected emitters. The selected lights are a set for those caches:
    // swapping equal-priority entries must not invalidate an unchanged set.
    template <typename LightRange>
    std::uint64_t SelectionSignature(const LightRange& selected) noexcept
    {
        std::uint64_t signature = static_cast<std::uint64_t>(selected.size()) * 0x9e3779b97f4a7c15ull;
        for (const auto& light : selected)
        {
            std::uint64_t id = light.stableId;
            id ^= id >> 33;
            id *= 0xff51afd7ed558ccdull;
            id ^= id >> 33;
            signature += id;
        }
        return signature;
    }
}
