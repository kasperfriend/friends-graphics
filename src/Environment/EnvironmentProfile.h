#pragma once
#include <array>
#include <optional>
#include <string>
#include <map>
#include "LocationContext.h"
namespace renderer {
enum EnvironmentParameter : size_t { FogDensity, LocalFog, Atmosphere, Rays, Lighting, Weather, DistanceFogPower,
    CloudCoverage, CloudType, CloudFeatureSize, CloudThickness, CloudDetail, CloudEvolution, ParameterCount };
inline constexpr const char* EnvironmentKeys[ParameterCount]={"FogDensityMultiplier","LocalFogMultiplier","AtmosphereMultiplier","RayIntensityMultiplier","LightingMultiplier","WeatherIntensityMultiplier","DistanceFogPowerMultiplier",
    "CloudCoverageMultiplier","CloudTypeOffset","CloudFeatureSizeMultiplier",
    "CloudThicknessMultiplier","CloudDetailMultiplier","CloudEvolutionMultiplier"};
inline constexpr float EnvironmentMinimum[ParameterCount]={0,0,0,0,0,0,0,0,-1,.25f,.25f,0,0};
inline constexpr float EnvironmentMaximum[ParameterCount]={4,4,4,4,4,4,4,2,1,4,4,2,4};
struct EnvironmentProfile {
    std::string profile;
    std::array<std::optional<float>,ParameterCount> values;
};
struct ResolvedEnvironmentState {
    std::array<float,ParameterCount> values{1,1,1,1,1,1,1,1,0,1,1,1,1};
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
