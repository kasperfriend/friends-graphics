#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace renderer::local_light_detail
{
    // Signature for same-frame consumers that cache transforms/constants for
    // the selected emitters. The selected lights are a set for those caches:
    // swapping equal-priority entries must not invalidate an unchanged set.
    // The effective intensity is part of it because the pool cross-fades: a
    // light that is dissolving out keeps its slot and its id while its
    // intensity moves, and a cache that only watched the id set would keep
    // uploading the old strength.
    namespace detail
    {
        template <typename Light, typename = void>
        struct HasIntensity : std::false_type {};
        template <typename Light>
        struct HasIntensity<Light, std::void_t<decltype(std::declval<const Light&>().intensity)>>
            : std::true_type {};
    }

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
            // Quantize to 1/256 of the authored range: sub-level changes are
            // invisible, and this keeps the half-float uploads stable. Light
            // types that carry no intensity (identity-only consumers) keep the
            // set semantics they had.
            if constexpr (detail::HasIntensity<typename LightRange::value_type>::value)
            {
                const std::uint64_t weight = static_cast<std::uint64_t>(
                    std::clamp(light.intensity, 0.f, 255.999f) * 256.f);
                id ^= weight * 0x9e3779b97f4a7c15ull;
            }
            signature += id;
        }
        return signature;
    }

    // ---------------------------------------------------------------------
    // Pool membership
    // ---------------------------------------------------------------------
    //
    // Only four or eight emitters fit in the constant table, so which ones is
    // a decision taken every frame - and it used to be taken on the raw score
    // alone. Two lamps of similar strength therefore swapped places as the
    // player walked between them, and every swap was one lamp switching off
    // and another switching on in the same frame: the reported flicker when
    // walking near a group of lights.
    //
    // Incumbents now keep their slot until a challenger beats them by this
    // margin, and only one slot can change hands per frame, so a boundary
    // crossing costs a single deliberate swap instead of a stream of them.
    inline constexpr float kDisplacementMargin = 1.30f;

    // Even a real change is cross-faded: a slot enters and leaves over these
    // times instead of popping. A torch you walk past dims out of the pool.
    inline constexpr float kFadeInMs = 140.f;
    inline constexpr float kFadeOutMs = 220.f;

    // Advances one slot's weight. `present` says whether the light is in this
    // frame's candidate set; a light that left keeps its slot for the fade-out
    // and is then dropped.
    inline float FadeToward(float weight, bool present, float elapsedMs) noexcept
    {
        const float current = std::isfinite(weight) ? std::clamp(weight, 0.f, 1.f) : 0.f;
        const float elapsed = std::isfinite(elapsedMs) ? std::max(elapsedMs, 0.f) : 0.f;
        if (present)
            return std::min(1.f, current + elapsed / kFadeInMs);
        return std::max(0.f, current - elapsed / kFadeOutMs);
    }

    // Reorders `candidates` (any order on entry) so that the first `limit`
    // entries are this frame's pool, with incumbents preferred. `previous` is
    // the pool published last frame. `less(a,b)` must report a stronger light
    // before a weaker one. Returns how many entries the pool holds.
    template <typename Light, typename Less>
    std::size_t StablePool(std::vector<Light>& candidates, const std::vector<Light>& previous,
                           std::size_t limit, Less less)
    {
        std::sort(candidates.begin(), candidates.end(), less);
        if (candidates.size() <= limit)
            return candidates.size();
        if (previous.empty())
            return limit;

        std::vector<char> chosen(candidates.size(), 0);
        std::vector<std::size_t> incumbents;
        std::size_t kept = 0;
        for (const auto& old : previous)
        {
            if (kept >= limit)
                break;
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                if (!chosen[i] && candidates[i].stableId == old.stableId)
                {
                    chosen[i] = 1;
                    incumbents.push_back(i);
                    ++kept;
                    break;
                }
            }
        }

        // One displacement per frame: a challenger has to beat the weakest
        // incumbent by the margin to take its slot.
        if (!incumbents.empty())
        {
            std::size_t weakest = incumbents[0];
            for (std::size_t index : incumbents)
                if (less(candidates[weakest], candidates[index]))
                    weakest = index;
            for (std::size_t i = 0; i < candidates.size() && i < limit + incumbents.size(); ++i)
            {
                if (chosen[i])
                    continue;
                if (candidates[i].score > candidates[weakest].score * kDisplacementMargin &&
                    std::isfinite(candidates[i].score))
                {
                    chosen[weakest] = 0;
                    chosen[i] = 1;
                    break;
                }
            }
        }

        // Fill whatever is left with the strongest remaining candidates.
        for (std::size_t i = 0; i < candidates.size() && kept < limit; ++i)
        {
            if (chosen[i])
                continue;
            chosen[i] = 1;
            ++kept;
        }

        // Compact the chosen entries to the front, strongest first, so the
        // pool stays ordered for the shader's per-slot work.
        std::size_t next = 0;
        for (std::size_t i = 0; i < candidates.size() && next < kept; ++i)
        {
            if (!chosen[i])
                continue;
            if (next != i)
                std::swap(candidates[next], candidates[i]);
            ++next;
        }
        candidates.resize(kept);
        return kept;
    }
}
