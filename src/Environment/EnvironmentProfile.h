#pragma once
#include <array>
#include <optional>
#include <string>
#include <map>
#include "LocationContext.h"
namespace renderer {
enum EnvironmentParameter : size_t { FogDensity, LocalFog, Atmosphere, Rays, Lighting, Weather, DistanceFogPower,
    CloudCoverage, CloudType, CloudFeatureSize, CloudThickness, CloudDetail, CloudEvolution,
    WaterAgitation, WaterClarity, UnderwaterTurbidity, UnderwaterCaustics, UnderwaterShafts,
    UnderwaterGlow, ParameterCount };
inline constexpr const char* EnvironmentKeys[ParameterCount]={"FogDensityMultiplier","LocalFogMultiplier","AtmosphereMultiplier","RayIntensityMultiplier","LightingMultiplier","WeatherIntensityMultiplier","DistanceFogPowerMultiplier",
    "CloudCoverageMultiplier","CloudTypeOffset","CloudFeatureSizeMultiplier",
    "CloudThicknessMultiplier","CloudDetailMultiplier","CloudEvolutionMultiplier",
    "WaterAgitationMultiplier","WaterClarityMultiplier","UnderwaterTurbidityMultiplier",
    "UnderwaterCausticMultiplier","UnderwaterShaftMultiplier","UnderwaterGlowMultiplier"};
inline constexpr float EnvironmentMinimum[ParameterCount]={0,0,0,0,0,0,0,0,-1,.25f,.25f,0,0,0,.1f,0,0,0,0};
inline constexpr float EnvironmentMaximum[ParameterCount]={4,4,4,4,4,4,4,2,1,4,4,2,4,4,4,4,2,2,2};
struct EnvironmentProfile {
    std::string profile;
    std::array<std::optional<float>,ParameterCount> values;
    // Per-zone water colours, authored as 0xRRGGBB next to WaterAgitationMultiplier.
    // Colours are not multipliers, so they cannot ride the float table above: each
    // channel is optional separately, and Map -> Zone -> Area resolution fills the
    // ones a layer leaves alone from the layer above it, exactly like the floats.
    std::optional<uint32_t> waterTint,waterDeep,waterAbsorption;
};
// Water colours after resolution, in the register space WaterEffect.h uploads:
// tint and absorption are authored bytes divided by 128 (0x808080 is a neutral
// multiply), deep is bytes divided by 255. The defaults are the shipped global
// [Water] colours, so a zone that authors nothing blends from the same base a
// zone that authors everything starts from. `authored` is what lets the water
// pass tell "this zone has its own water" apart from the global setting: an
// authored zone wins over GraphicsEffects.ini, and only an explicit per-location
// override in LocationGraphics.ini wins over the zone.
struct ResolvedWaterColours {
    std::array<float,3> tint{1.f,1.f,1.f};
    std::array<float,3> deep{14.f/255.f,42.f/255.f,40.f/255.f};
    std::array<float,3> absorption{255.f/128.f,140.f/128.f,107.f/128.f};
    bool authored=false;
};
struct ResolvedEnvironmentState {
    std::array<float,ParameterCount> values{1,1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1,1,1};
    ResolvedWaterColours water;
    float operator[](EnvironmentParameter key) const { return values[key]; }
};
struct EnvironmentConfiguration {
    bool enabled=true, debug=false;
    float transitionSeconds=4;
    uint32_t debounceMs=300;
    EnvironmentProfile defaults;
    std::map<std::string,EnvironmentProfile> named;
    std::map<uint32_t,EnvironmentProfile> maps,zones,areas;
};
struct EnvironmentResolution {
    ResolvedEnvironmentState state;
    std::string profile="Default",source="Default";
};
bool ParseEnvironmentConfiguration(const std::string& text,EnvironmentConfiguration& out,std::string& error);
EnvironmentResolution ResolveEnvironment(const EnvironmentConfiguration& config,const LocationContext& location);
ResolvedEnvironmentState BlendEnvironment(const ResolvedEnvironmentState& from,const ResolvedEnvironmentState& to,float t);
}
