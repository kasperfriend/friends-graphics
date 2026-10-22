#pragma once
#include <algorithm>
#include <array>

namespace renderer::manualcolor {

// A manual colour is a tint over the colour already supplied by the current
// zone. Alpha is the strength of that override (0 preserves the inherited
// colour, 100% selects the manual colour); brightness scales the chosen RGB.
struct Override {
    std::array<float,3> rgb{1.f,1.f,1.f};
    float alpha=0.f;
    float brightness=1.f;

    static Override FromPercent(int red,int green,int blue,int alphaPercent,int brightnessPercent) noexcept {
        const auto channel=[](int value) {
            return std::clamp(float(value)/255.f,0.f,1.f);
        };
        return {{channel(red),channel(green),channel(blue)},
                std::clamp(float(alphaPercent)*.01f,0.f,1.f),
                std::clamp(float(brightnessPercent)*.01f,0.f,3.f)};
    }

    std::array<float,3> Target() const noexcept {
        return {rgb[0]*brightness,rgb[1]*brightness,rgb[2]*brightness};
    }
};

inline std::array<float,3> Blend(const std::array<float,3>& base,
                                 const std::array<float,3>& target,
                                 float alpha) noexcept {
    alpha=std::clamp(alpha,0.f,1.f);
    return {base[0]+(target[0]-base[0])*alpha,
            base[1]+(target[1]-base[1])*alpha,
            base[2]+(target[2]-base[2])*alpha};
}

inline std::array<float,3> Apply(const std::array<float,3>& base,
                                 const Override& colour) noexcept {
    return Blend(base,colour.Target(),colour.alpha);
}

// Clouds use the manual colour as a radiance tint: white at 100% brightness
// is neutral, while a selected hue can progressively replace the zone tint.
inline std::array<float,3> Multiplier(const Override& colour) noexcept {
    const auto target=colour.Target();
    return {1.f+(target[0]-1.f)*colour.alpha,
            1.f+(target[1]-1.f)*colour.alpha,
            1.f+(target[2]-1.f)*colour.alpha};
}

} // namespace renderer::manualcolor
