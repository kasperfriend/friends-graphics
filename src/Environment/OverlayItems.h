#pragma once
#include <vector>

namespace tuningoverlay {
enum ItemKind { KIND_HEADER, KIND_TOGGLE, KIND_SLIDER };
struct Item { ItemKind kind; const wchar_t* section; const wchar_t* key; const char* label; int lo,hi,step,value; };
inline std::vector<Item> items={
 // === ATMOSPHERE & GOD RAYS ===
 {KIND_HEADER,nullptr,nullptr,"--- ATMOSPHERE & GOD RAYS ---",0,0,0,0},
 {KIND_TOGGLE,L"Atmosphere",L"ShaftsEnabled","SUN RAYS",0,1,1,1},
 {KIND_SLIDER,L"Atmosphere",L"ShaftPercent","RAY STRENGTH",0,400,5,160},
 {KIND_SLIDER,L"Atmosphere",L"SunGlowPercent","SUN GLOW",0,200,5,80},
 {KIND_TOGGLE,L"Atmosphere",L"FogEnabled","FOG",0,1,1,1},
 // Derives fog distance/colour from WoW's own authored values for the
 // current zone/shader (read-only capture) instead of one fixed profile
 // everywhere - FOG DISTANCE still scales it. Off = old fixed profile.
 {KIND_TOGGLE,L"Atmosphere",L"UseEnvironmentFog","WOW FOG BASELINE",0,1,1,0},
 {KIND_SLIDER,L"Atmosphere",L"AtmosphereQuality","ATMOSPHERE QUALITY",0,2,1,1},
 {KIND_SLIDER,L"Atmosphere",L"DensityPermille","FOG DENSITY",0,20,1,5},
 {KIND_SLIDER,L"Atmosphere",L"AtmosphereMaxDistance","FOG DISTANCE",200,1200,20,520},
 {KIND_SLIDER,L"Atmosphere",L"AtmosphereWashPercent","FOG WASH",0,100,2,55},
 // Separate near-ground world-space volume integrated by the atmosphere
 // raymarch. Movement opens a soft wake which gradually fills back in.
 {KIND_HEADER,nullptr,nullptr,"--- LOCAL VOLUMETRIC FOG ---",0,0,0,0},
 {KIND_TOGGLE,L"LocalFog",L"Enabled","LOCAL FOG",0,1,1,1},
 {KIND_SLIDER,L"LocalFog",L"DensityPermille","LOCAL DENSITY",0,100,2,12},
 {KIND_SLIDER,L"LocalFog",L"HeightUnits","LOCAL HEIGHT",1,8,1,3},
 {KIND_SLIDER,L"LocalFog",L"WakeStrengthPercent","PLAYER WAKE",0,150,5,85},
 {KIND_SLIDER,L"LocalFog",L"WakeRadius","WAKE RADIUS",2,14,1,5},
 {KIND_SLIDER,L"LocalFog",L"TrailLength","WAKE TRAIL",4,40,2,18},
 // Rescales the GAME's OWN native distance fog (its real per-shader fog
 // constants, the same ones vanilla WoW uses to hide its own render/
 // streaming distance edge) - independent of the atmosphere controls
 // above. This is what actually guarantees the world edge stays covered
 // regardless of any other setting here.
 {KIND_TOGGLE,L"DistanceFog",L"Enabled","WORLD EDGE FOG",0,1,1,1},
 {KIND_SLIDER,L"DistanceFog",L"DistancePercent","EDGE FOG DISTANCE",10,300,5,130},
 {KIND_SLIDER,L"DistanceFog",L"PowerPercent","EDGE FOG POWER",10,300,5,30},

 // === WEATHER VISUALS ===
 {KIND_HEADER,nullptr,nullptr,"--- WEATHER VISUALS ---",0,0,0,0},
 {KIND_TOGGLE,L"WeatherVisuals",L"Enabled","WEATHER SYSTEM",0,1,1,1},
 {KIND_SLIDER,L"WeatherVisuals",L"Mode","WEATHER HINT (0=AUTO 1=RAIN 2=SNOW)",0,2,1,0},
 {KIND_SLIDER,L"WeatherVisuals",L"IntensityPercent","WEATHER STRENGTH",25,200,5,100},

 // === SHARED WIND (simulation only; no visible wind effect) ===
 {KIND_HEADER,nullptr,nullptr,"--- WIND ---",0,0,0,0},
 {KIND_TOGGLE,L"Wind",L"Enabled","SHARED WIND",0,1,1,1},
 {KIND_TOGGLE,L"Wind",L"Dynamic","TIME VARIATION",0,1,1,1},
 {KIND_SLIDER,L"Wind",L"Direction","BASE WIND FROM",0,7,1,1},
 {KIND_SLIDER,L"Wind",L"StrengthPercent","BASE STRENGTH (%)",0,200,5,100},
 {KIND_SLIDER,L"Wind",L"GustPercent","GUST VARIATION (%)",0,100,5,35},
 {KIND_SLIDER,L"Wind",L"GustSeconds","GUST PERIOD (SECONDS)",2,120,1,18},
 {KIND_SLIDER,L"Wind",L"DirectionChangeSeconds","DIRECTION INTERVAL (SECONDS)",15,600,5,120},
 {KIND_SLIDER,L"Wind",L"TransitionSeconds","RESPONSE TIME (SECONDS)",1,120,1,30},
 {KIND_SLIDER,L"Wind",L"WeatherResponsePercent","WEATHER RESPONSE (%)",0,200,5,100},
 {KIND_TOGGLE,L"Wind",L"AffectWaves","DRIVE WAVES",0,1,1,1},
 {KIND_TOGGLE,L"Wind",L"AffectClouds","DRIVE CLOUDS",0,1,1,1},
 {KIND_TOGGLE,L"Wind",L"DebugView","WIND DEBUG VIEW",0,1,1,0},

 // Raymarched volumetric cloud deck. Advanced scattering knobs stay ini-only.
 {KIND_HEADER,nullptr,nullptr,"--- VOLUMETRIC CLOUDS ---",0,0,0,0},
 {KIND_TOGGLE,L"VolumetricClouds",L"Enabled","CLOUDS",0,1,1,1},
 {KIND_TOGGLE,L"VolumetricClouds",L"ZoneClouds","ZONE CLOUD CHARACTER",0,1,1,1},
 {KIND_TOGGLE,L"VolumetricClouds",L"WeatherClouds","WEATHER-DRIVEN CLOUDS",0,1,1,1},
 {KIND_SLIDER,L"VolumetricClouds",L"EvolutionPercent","CLOUD EVOLUTION",0,400,10,100},
 {KIND_SLIDER,L"VolumetricClouds",L"CoveragePercent","CLOUD COVERAGE",0,100,2,50},
 {KIND_SLIDER,L"VolumetricClouds",L"DensityPercent","CLOUD DENSITY",10,400,5,100},
 {KIND_SLIDER,L"VolumetricClouds",L"CloudTypePercent","CLOUD TYPE (-100 FLAT 100 TOWERING)",-100,100,5,0},
 {KIND_SLIDER,L"VolumetricClouds",L"BaseAltitude","CLOUD HEIGHT",100,20000,50,850},
 {KIND_SLIDER,L"VolumetricClouds",L"Thickness","CLOUD THICKNESS",100,12000,50,2150},
 {KIND_SLIDER,L"VolumetricClouds",L"FeatureSize","CLOUD SIZE",600,40000,200,3000},
 {KIND_SLIDER,L"VolumetricClouds",L"WindSpeed","CLOUD DRIFT AT 100% WIND",0,400,5,26},
 {KIND_SLIDER,L"VolumetricClouds",L"SunIntensityPercent","CLOUD SUNLIGHT",0,400,5,130},
 {KIND_SLIDER,L"VolumetricClouds",L"AerialPerspectivePercent","CLOUD HAZE",0,100,5,60},
 {KIND_SLIDER,L"VolumetricClouds",L"GroundShadowPercent","CLOUD SHADOWS",0,100,5,55},
 {KIND_TOGGLE,L"VolumetricClouds",L"SunRaysEnabled","CLOUD SUN RAYS",0,1,1,1},
 {KIND_SLIDER,L"VolumetricClouds",L"SunRayPercent","CLOUD RAY STRENGTH",0,200,5,80},
 {KIND_SLIDER,L"VolumetricClouds",L"ResolutionPercent","CLOUD RESOLUTION (%)",20,100,5,25},
 {KIND_SLIDER,L"VolumetricClouds",L"Quality","CLOUD QUALITY (0 LOW 3 ULTRA)",0,3,1,2},

 // === WATER & REFLECTIONS ===
 {KIND_HEADER,nullptr,nullptr,"--- WATER & REFLECTIONS ---",0,0,0,0},
 {KIND_TOGGLE,L"Water",L"Enabled","WATER",0,1,1,1},
 {KIND_TOGGLE,L"Water",L"WavesEnabled","WAVES",0,1,1,1},
 // Lifts and slides the surface itself, so a crest is a form the light falls across rather
 // than a normal map pretending to be one. Off leaves the shading intact on a flat sheet.
 {KIND_TOGGLE,L"Water",L"GeometryWavesEnabled","WAVE GEOMETRY",0,1,1,1},
 {KIND_TOGGLE,L"Water",L"ReflectionsEnabled","REFLECTIONS",0,1,1,1},
 {KIND_SLIDER,L"Water",L"ReflectionPercent","SSR STRENGTH",0,150,2,80},
 {KIND_SLIDER,L"Water",L"EnvironmentPercent","SKY REFLECTION",0,150,2,45},
 {KIND_TOGGLE,L"Water",L"SunGlintEnabled","SUN MOON GLINT",0,1,1,1},
 // The upper bound matches the renderer's own clamp in WaterEffect.h
 // (`std::clamp(ReadTuning(L"SunGlintPercent",160),0,400)`): a slider that can be
 // dragged past what the shader accepts silently does nothing above 400.
 {KIND_SLIDER,L"Water",L"SunGlintPercent","GLINT STRENGTH",0,400,5,160},
 {KIND_TOGGLE,L"Water",L"RefractionEnabled","REFRACTION",0,1,1,1},
 {KIND_SLIDER,L"Water",L"RefractionPercent","REFRACT AMOUNT",0,50,1,14},
 {KIND_SLIDER,L"Water",L"DepthAbsorptionPercent","WATER DEPTH",0,100,1,38},
 {KIND_TOGGLE,L"Water",L"FoamEnabled","FOAM",0,1,1,1},
 {KIND_SLIDER,L"Water",L"ShoreFoamPercent","FOAM AMOUNT",0,60,1,28},
 {KIND_SLIDER,L"Water",L"SpecularPercent","WATER SHINE",0,400,5,115},
 {KIND_SLIDER,L"Water",L"WaveScalePercent","WAVE SIZE",25,400,5,100},
 // Percent of the shipped sea: 0 flat, 100 the reference look, 300 a gale. Scaled by the
 // wave size above and by the zone's own sea state, and clamped steep enough to never fold.
 {KIND_SLIDER,L"Water",L"GeometryWavePercent","WAVE HEIGHT",0,300,5,100},
 {KIND_SLIDER,L"Water",L"WaveSofteningPercent","WAVE FADE",0,400,5,100},
 {KIND_SLIDER,L"Water",L"ClarityPercent","WATER CLARITY",5,200,5,100},
 {KIND_SLIDER,L"Water",L"WindStrengthPercent","WATER WIND / FOAM BIAS",0,200,5,100},
 {KIND_SLIDER,L"Water",L"SparklePercent","SUN SPARKLE",25,200,5,100},
 {KIND_SLIDER,L"Water",L"FlowSpeedPercent","WAVE SPEED",0,300,5,100},
 {KIND_SLIDER,L"Water",L"WeatherResponsePercent","WIND / WEATHER RESPONSE",0,200,5,100},
 {KIND_TOGGLE,L"Water",L"ZoneWaves","PER-ZONE SEA STATE",0,1,1,1},

 // === LOCAL LIGHTING ===
 {KIND_HEADER,nullptr,nullptr,"--- LOCAL LIGHTING ---",0,0,0,0},
 {KIND_TOGGLE,L"DynamicLighting",L"Enabled","DYNAMIC LIGHTS",0,1,1,1},
 {KIND_SLIDER,L"DynamicLighting",L"IntensityPercent","LIGHT INTENSITY",0,250,5,100},
 {KIND_SLIDER,L"DynamicLighting",L"RayPercent","LIGHT RAYS",0,200,5,75},
 {KIND_SLIDER,L"DynamicLighting",L"Quality","LIGHT QUALITY (0 LOW 1 BAL 2 HIGH)",0,2,1,1},
 {KIND_SLIDER,L"DynamicLighting",L"DebugMode","LIGHT DEBUG",0,4,1,0},

 // === DYNAMIC SHADOWS ===
 {KIND_HEADER,nullptr,nullptr,"--- DYNAMIC SHADOWS ---",0,0,0,0},
 {KIND_TOGGLE,L"NativeShadows",L"Enabled","NATIVE SHADOWS",0,1,1,1},
 {KIND_SLIDER,L"NativeShadows",L"SoftnessPercent","SHADOW SOFTNESS",50,220,5,145},
 // SHADOW STRENGTH: c12 (the darkening constant) is a `def`-declared shader
 // literal, so writing the constant register at runtime is a no-op - this
 // slider instead swaps in a byte-patched COPY of the shader (only the two
 // c12 floats changed, everything else byte-identical) for the 4 confirmed
 // receiver hashes, restoring the original shader after the draw.
 {KIND_SLIDER,L"NativeShadows",L"StrengthPercent","SHADOW STRENGTH",30,250,10,100},

 // === AI MATERIALS & RELIEF ===
 {KIND_HEADER,nullptr,nullptr,"--- AI MATERIALS & RELIEF ---",0,0,0,0},
 {KIND_TOGGLE,L"AIMaterials",L"Enabled","AI MATERIALS",0,1,1,1},
 {KIND_SLIDER,L"AIMaterials",L"NormalStrengthPercent","NORMAL BUMP",0,200,5,90},
 {KIND_SLIDER,L"AIMaterials",L"ParallaxDepthPercent","PARALLAX DEPTH",0,100,2,35},
 {KIND_SLIDER,L"AIMaterials",L"SelfShadowStrength","SELF SHADOWS",0,150,5,90},

 // === IMAGE & COLOR ===
 {KIND_HEADER,nullptr,nullptr,"--- IMAGE & COLOR (GLOBAL) ---",0,0,0,0},
 {KIND_TOGGLE,L"PostProcess",L"Enabled","POST PROCESS",0,1,1,0},
 {KIND_SLIDER,L"PostProcess",L"BrightnessPercent","BRIGHTNESS",-50,50,1,0},
 {KIND_SLIDER,L"PostProcess",L"ContrastPercent","CONTRAST",50,180,2,100},
 {KIND_SLIDER,L"PostProcess",L"GammaPercent","GAMMA",50,180,2,100},
 {KIND_SLIDER,L"PostProcess",L"SharpnessPercent","SHARPNESS",0,100,2,35}
};
inline const std::vector<int> defaults=[](){std::vector<int> result;for(const auto& i:items)result.push_back(i.value);return result;}();
}
