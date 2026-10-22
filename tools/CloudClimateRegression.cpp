// Portable production-policy and density-field regression. Run from repo root.
// g++ -O2 -std=c++20 -pthread tools/CloudClimateRegression.cpp \
//   src/Environment/EnvironmentProfile.cpp src/Environment/EnvironmentDatabase.cpp -o build/cloud-climate-test
#define CLOUD_PREVIEW_NO_MAIN
#include "CloudPreview.cpp"
#include "../src/Environment/EnvironmentDatabase.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
using namespace renderer;

static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
static bool Near(double a, double b, double eps = 1e-4) { return std::abs(a-b) < eps; }
static std::string Read(const char* path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error(std::string("Missing ") + path);
    return {(std::istreambuf_iterator<char>(in)), {}};
}
static CloudConfig Preview(const CloudShape& s, float time = 0)
{
    CloudConfig c;
    c.coverage=s.coverage;c.density=s.density;c.typeBias=s.cloudType;
    c.bottom=s.baseAltitude;c.top=c.bottom+s.thickness;
    c.featureSize=s.featureSize;c.shapeZScale=s.shapeAspect;
    c.detailSize=s.detailSize;c.weatherSize=s.weatherSize;c.detailStrength=s.detailStrength;
    c.windSpeed=s.windSpeed;c.windAngle=s.windAngle;c.windShear=s.windShear;
    c.evolutionSpeed=s.evolutionSpeed;c.time=time;
    return c;
}
static float ColumnCoverage(const CloudConfig& c, const CloudResources& r)
{
    Marcher m(c,r,{});
    int covered=0;
    // Across a full weather tile, not a lucky camera position. Production
    // detailed density integrated vertically through each cloud column.
    for (int y=0;y<24;++y) for (int x=0;x<24;++x)
    {
        float optical=0;
        for (int z=0;z<48;++z)
        {
            const float h=(z+.5f)/48.f;
            optical+=m.Density({(x+.5f)*c.weatherSize/24.f,(y+.5f)*c.weatherSize/24.f,
                               c.bottom+(c.top-c.bottom)*h},h,true)*(c.top-c.bottom)/48.f;
        }
        covered+=std::exp(-optical)<.9f;
    }
    return covered/(24.f*24.f);
}

int main() try
{
    EnvironmentConfiguration config;
    std::string error;
    Require(ParseEnvironmentConfiguration(Read("EnvironmentProfiles.ini"),config,error),
            "shipped cloud climate configuration parses");
    EnvironmentDatabase db;
    Require(db.Load(L"data/areas.txt",error)&&db.Authoritative(),"zone IDs validated against shipped authoritative database");
    constexpr std::array<uint32_t,4> outdoorMaps{0,1,530,571};
    size_t rootAreas=0,outdoorRoots=0;
    for (const auto& [id,area]:db.Areas())
    {
        if (area.parentId) continue;
        ++rootAreas;
        if (std::find(outdoorMaps.begin(),outdoorMaps.end(),area.mapId)!=outdoorMaps.end()) ++outdoorRoots;
        const auto authored=config.zones.find(id);
        if (authored==config.zones.end() || !authored->second.values[WaterAgitation])
            throw std::runtime_error("Missing readable sea state for root " + std::to_string(id) + " on map "
                                     + std::to_string(area.mapId) + " (" + area.name + ")");
        const auto resolved=ResolveEnvironment(config,{area.mapId,id,0,true});
        if (!Near(resolved.state[WaterAgitation],*authored->second.values[WaterAgitation]))
            throw std::runtime_error("The resolver did not apply the sea state for root " + std::to_string(id));
    }
    Require(rootAreas==414&&outdoorRoots==165&&config.zones.size()==414,
            "every one of the 414 AreaTable roots has a directly resolved per-zone sea state");
    for (const auto& [id,profile]:config.zones)
    {
        const auto* area=db.Area(id);
        if (!area || area->parentId) throw std::runtime_error("Invalid root zone " + std::to_string(id));
        const auto resolved=ResolveEnvironment(config,{area->mapId,id,0,true});
        for (size_t k=0;k<ParameterCount;++k)
            if (!std::isfinite(resolved.state.values[k]) || resolved.state.values[k]<EnvironmentMinimum[k] ||
                resolved.state.values[k]>EnvironmentMaximum[k]) throw std::runtime_error("Invalid resolved climate");
        for (size_t k=0;k<=DistanceFogPower;++k)
            if (resolved.state.values[k]!=1.f) throw std::runtime_error("Cloud preset changed unrelated effects");
    }
    Require(true,"every assigned zone exists and cloud presets leave fog/light/weather tuning intact");
    auto env=[&](uint32_t zone) {const auto* a=db.Area(zone);return ResolveEnvironment(config,{a->mapId,zone,0,true}).state;};
    const auto fair=env(12), desert=env(440), coast=env(148), mountain=env(67), open=env(3523);
    Require(fair[CloudCoverage]>desert[CloudCoverage]&&coast[CloudType]<0&&mountain[CloudType]>0&&open[CloudCoverage]==0,
            "zone art directions are distinct, including a genuinely open sky");
    const auto northernFallback=env(4201);
    Require(Near(northernFallback[WaterAgitation],.85f)&&northernFallback[CloudType]<0,
            "a water-only Northrend zone keeps its map-level CloudCold profile");
    Require(ResolveEnvironment(config,{9999,9999,0,true}).state[CloudCoverage]==1&&
            ResolveEnvironment(config,{}).state[CloudType]==0,"unknown/invalid locations have neutral fallback");
    // Water rides the same authoring layer, so the same two things have to hold: a zone line that
    // only names a sea state keeps its climate, and a climate cannot move the water on its own.
    Require(Near(env(33)[WaterAgitation],1.45f)&&Near(env(38)[WaterAgitation],.65f)&&
            Near(env(3518)[WaterAgitation],.9f)&&Near(env(12)[WaterAgitation],.45f)&&
            Near(env(1497)[WaterAgitation],.3f),
            "Stranglethorn stays exposed while Loch Modan, Nagrand and Undercity are calmer");
    Require(env(33)[CloudCoverage]>1.f&&Near(env(33)[CloudFeatureSize],.9f),
            "a water-only zone line inherits its climate profile instead of replacing it");
    Require(env(214)[WaterAgitation]>1.3f&&env(308)[WaterAgitation]>1.3f&&
            env(1638)[WaterAgitation]<.6f&&env(1537)[WaterAgitation]<.6f&&env(618)[WaterAgitation]<1.f,
            "open seas are rough and landlocked cities are glass");
    Require(ResolveEnvironment(config,{9999,9999,0,true}).state[WaterAgitation]==1.f,
            "an unknown zone keeps the neutral sea state");
    {
        // The water-zone sweep: 199 root zones author a complete colour set and a
        // complete underwater tuple together (clarity / turbidity / caustics /
        // shafts / glow, each a multiplier on the player's own dial). Pin the
        // count and the shape on names a reader can check against the world:
        // red contaminated shallows, swamp soup, a clear dammed reservoir, and a
        // lava zone that authors nothing and must stay exactly neutral.
        size_t swept=0;
        for (const auto& [id,profile]:config.zones)
        {
            const bool colours=profile.waterTint&&profile.waterDeep&&profile.waterAbsorption;
            const bool dials=profile.values[WaterClarity]&&profile.values[UnderwaterTurbidity]
                &&profile.values[UnderwaterCaustics]&&profile.values[UnderwaterShafts]
                &&profile.values[UnderwaterGlow];
            if (colours!=dials) throw std::runtime_error("Zone authors colours without underwater dials (or the reverse): "+std::to_string(id));
            swept+=colours?1:0;
        }
        Require(swept==199,"199 water-bearing root zones carry colours and underwater dials as one authored set");
        const auto blood=env(3525),swamp=env(8),loch=env(38),dry=env(46);
        Require(Near(blood[WaterClarity],.45f)&&Near(blood[UnderwaterTurbidity],1.9f)
                &&blood.water.authored&&blood.water.tint[0]>blood.water.tint[1],
                "Bloodmyst is murky and its authored tint is red");
        Require(swamp[WaterClarity]<.4f&&swamp[UnderwaterTurbidity]>2.f
                &&loch[WaterClarity]>1.4f&&loch[UnderwaterTurbidity]<.6f,
                "Swamp of Sorrows is soup and Loch Modan is a clear reservoir");
        Require(dry[WaterClarity]==1.f&&dry[UnderwaterTurbidity]==1.f&&dry[UnderwaterCaustics]==1.f
                &&dry[UnderwaterShafts]==1.f&&dry[UnderwaterGlow]==1.f&&!dry.water.authored,
                "a zone outside the sweep keeps the player's dials and the global colours untouched");
        const auto booty=ResolveEnvironment(config,{0,33,35,true}).state;
        Require(Near(booty[WaterClarity],1.7f)&&Near(booty[UnderwaterTurbidity],.45f)
                &&booty.water.authored&&booty.water.tint[2]>booty.water.tint[0],
                "Booty Bay's area override gives the harbour its azure water inside Stranglethorn");
        Require(Near(BlendEnvironment(swamp,loch,.5f)[WaterClarity],
                     (swamp[WaterClarity]+loch[WaterClarity])*.5f),
                "zone borders blend the underwater dials rather than swapping the medium");
    }
    {
        EnvironmentConfiguration tuned;std::string waterError;
        Require(ParseEnvironmentConfiguration("[Default]\nWaterAgitationMultiplier=4\n",tuned,waterError)&&
                tuned.defaults.values[WaterAgitation]&&*tuned.defaults.values[WaterAgitation]==4.f,
                "the accepted top of the sea-state range parses exactly");
        Require(!ParseEnvironmentConfiguration("[Default]\nWaterAgitationMultiplier=41\n",tuned,waterError),
                "an out-of-range sea state is refused rather than quietly clamped");
        Require(ParseEnvironmentConfiguration(
                    "[Default]\nWaterClarityMultiplier=4\nUnderwaterTurbidityMultiplier=4\n"
                    "UnderwaterCausticMultiplier=2\nUnderwaterShaftMultiplier=2\nUnderwaterGlowMultiplier=2\n",
                    tuned,waterError)
                &&*tuned.defaults.values[WaterClarity]==4.f
                &&*tuned.defaults.values[UnderwaterGlow]==2.f,
                "the tops of the underwater dial ranges parse exactly");
        for (const auto* invalid:{"WaterClarityMultiplier=0.05","UnderwaterTurbidityMultiplier=4.1",
             "UnderwaterCausticMultiplier=2.1","UnderwaterShaftMultiplier=-0.1","UnderwaterGlowMultiplier=nan"})
            if (ParseEnvironmentConfiguration(std::string("[Default]\n")+invalid,tuned,waterError))
                throw std::runtime_error("Accepted corrupt underwater dial");
        Require(*tuned.defaults.values[WaterClarity]==4.f,
                "corrupt underwater dials are rejected transactionally");
    }
    config.enabled=false;
    Require(ResolveEnvironment(config,{1,440,0,true}).state[CloudCoverage]==1,"disabled environment is neutral");
    config.enabled=true;
    EnvironmentConfiguration layered;
    Require(ParseEnvironmentConfiguration("[Environment.Base]\nCloudTypeOffset=-0.8\nCloudCoverageMultiplier=0.7\n"
        "[Environment.Child]\nProfile=Base\nCloudEvolutionMultiplier=1.5\n"
        "[Map.0]\nProfile=Child\nWaterAgitationMultiplier=1.3\n"
        "[Zone.12]\nCloudCoverageMultiplier=0\nWaterAgitationMultiplier=0.7\n"
        "[Area.87]\nCloudTypeOffset=0.6\nWaterAgitationMultiplier=0.35\n",layered,error),"cloud parameters support named inheritance and signed type");
    auto state=ResolveEnvironment(layered,{0,12,87,true}).state;
    Require(state[CloudCoverage]==0&&Near(state[CloudType],.6)&&Near(state[CloudEvolution],1.5)
                &&Near(state[WaterAgitation],.35),
            "Default < Map < Zone < Area preserves zero and per-area sea-state overrides");
    Require(Near(ResolveEnvironment(layered,{0,12,999,true}).state[WaterAgitation],.7)
                &&Near(ResolveEnvironment(layered,{0,999,999,true}).state[WaterAgitation],1.3),
            "unknown areas inherit zone sea state and unknown zones inherit map sea state");
    for (const auto* invalid:{"CloudTypeOffset=-1.1","CloudCoverageMultiplier=2.1","CloudFeatureSizeMultiplier=0",
         "CloudThicknessMultiplier=5","CloudDetailMultiplier=nan","CloudEvolutionMultiplier=inf","UnknownCloud=1"})
        if (ParseEnvironmentConfiguration(std::string("[Default]\n")+invalid,layered,error))
            throw std::runtime_error("Accepted corrupt cloud setting");
    Require(layered.named.count("Child")==1,"invalid cloud reloads are rejected transactionally");
    Require(Near(BlendEnvironment(desert,coast,.5f)[CloudType],(desert[CloudType]+coast[CloudType])*.5f),
            "environment interpolation includes signed cloud shape parameters");

    CloudShape base;
    const auto manual=ResolveCloudClimate(base,desert,false);
    Require(manual.coverage==base.coverage&&manual.featureSize==base.featureSize,"zone opt-out preserves manual cloud baseline");
    CloudDynamics transition;
    const auto start=transition.Update(base,fair,true,CloudWeather::Clear,0,12,0);
    const auto changed=transition.Update(base,desert,true,CloudWeather::Clear,0,12,1.f/60);
    Require(changed.coverage<start.coverage&&changed.coverage>start.coverage-.002f,
            "zone border changes begin with a bounded step, not a sky replacement");
    const auto reversed=transition.Update(base,fair,true,CloudWeather::Clear,0,12,1.f/60);
    Require(reversed.coverage>changed.coverage&&reversed.coverage<start.coverage,"border reversal continues from visible state");
    auto weatherAt=[&](int fps) {
        CloudDynamics dynamics;
        auto result=dynamics.Update(base,desert,true,CloudWeather::Clear,0,12,0);
        for(int i=0;i<fps*18;++i) result=dynamics.Update(base,desert,true,CloudWeather::Rain,1,12,1.f/fps);
        return result;
    };
    Require(Near(weatherAt(30).coverage,weatherAt(144).coverage,2e-5),"weather response is framerate-independent at 30/144 Hz");
    CloudDynamics weather;
    weather.Update(base,open,true,CloudWeather::Clear,0,12,0);
    auto wet=weather.Update(base,open,true,CloudWeather::Rain,1,12,1.f/60);
    Require(wet.coverage>0&&wet.coverage<.004f,"storms develop even from a completely empty sky");
    for(int i=0;i<1800;++i) wet=weather.Update(base,open,true,CloudWeather::Rain,1,12,.1f);
    Require(wet.coverage==1&&wet.cloudType>.7f,"sustained rain reaches full overcast and deep billows");
    const auto clearStep=weather.Update(base,open,true,CloudWeather::Clear,0,12,1.f/60);
    Require(clearStep.coverage>.998f&&clearStep.coverage<1,"weather cessation cannot pop the deck away");
    for(int i=0;i<2000;++i) wet=weather.Update(base,open,true,CloudWeather::Clear,0,12,.1f);
    Require(wet.coverage==0&&weather.WeatherAmount()==0,"clearing eventually reaches exact zero and permits the GPU early-out");
    // A zone change is a discontinuity, not a trend. Rain the client was emitting in the
    // old zone is not evidence about the new one, so neither the detected weather nor the
    // wet vector that forced coverage to 1 may cross it. Carrying them across meant a full
    // overcast hung over a clear zone for the 30 s no-evidence hold plus the slow clearing
    // ramp - "stuck covering the whole sky, no transition between locations" after a fast
    // teleport. The new zone's own particles re-wet the deck, and the climate filter below
    // still eases the field, so the sky changes without popping.
    {
        CloudDynamics crossed;
        crossed.Update(base,open,true,CloudWeather::Clear,0,12,0);
        for(int i=0;i<1800;++i) crossed.Update(base,open,true,CloudWeather::Rain,1,12,.1f);
        Require(Near(crossed.WeatherAmount(),1.f,1e-4)&&
                Near(crossed.Update(base,open,true,CloudWeather::Rain,1,12,.1f).coverage,1.f,1e-5),
                "rain still reaches a full deck before the move");
        crossed.ForgetWeather();
        Require(crossed.WeatherAmount()==0,"a zone change drops the previous zone's precipitation");
        const auto landed=crossed.Update(base,open,true,CloudWeather::Clear,0,12,.1f);
        const auto climate=ResolveCloudClimate(base,open,true);
        Require(Near(landed.coverage,climate.coverage)&&landed.coverage<1.f,
                "the new zone starts from its own climate, not the old zone's overcast");
        auto rebuilt=landed;
        for(int i=0;i<180;++i) rebuilt=crossed.Update(base,open,true,CloudWeather::Rain,1,12,.1f);
        Require(rebuilt.coverage>landed.coverage,"rain in the new zone still builds its own deck");
    }
    for(int i=0;i<1000;++i)
    {
        wet=weather.Update(base,desert,true,i%2?CloudWeather::Rain:CloudWeather::Snow,1,12,.1f);
        if (wet.coverage>1||wet.cloudType< -1||wet.cloudType>1||weather.WeatherAmount()>1.00001f)
            throw std::runtime_error("Weather blend overshot");
    }
    Require(true,"rain/snow changes never overshoot coverage or cloud type");
    CloudDynamics pauseA=weather,pauseB=weather;
    const auto bounded=pauseA.Update(base,fair,true,CloudWeather::Clear,0,12,10000);
    const auto normal=pauseB.Update(base,fair,true,CloudWeather::Clear,0,12,.1f);
    Require(bounded.coverage==normal.coverage&&pauseA.Motion().shapeZ==pauseB.Motion().shapeZ,
            "focus/loading stalls take one bounded step");
    const auto before=weather.Motion();
    weather.Update(base,desert,true,CloudWeather::Rain,1,12,std::numeric_limits<float>::quiet_NaN());
    Require(weather.Motion().shapeZ==before.shapeZ,"invalid frame delta cannot poison animation phases");

    CloudShape still=base;still.windSpeed=0;
    CloudMotion motion;
    motion.Advance(still,300);
    Require(motion.shapeZ>0&&motion.detailZ>0&&motion.weather[0]>0&&motion.shape[0]==0,
            "zero-wind clouds evolve internally instead of only translating");
    still.evolutionSpeed=0;const auto frozen=motion;
    motion.Advance(still,600);
    Require(motion.shapeZ==frozen.shapeZ&&motion.detailZ==frozen.detailZ&&motion.weather==frozen.weather,
            "zero wind plus zero evolution freezes the field");
    still.windSpeed=100;motion.Advance(still,1.f/60);
    Require(motion.shape[0]>0&&motion.shape[0]<.001f,"editing wind after a long session cannot multiply total elapsed time");
    still.evolutionSpeed=4;motion.Advance(still,86400*30.f);
    Require(motion.shapeZ>=0&&motion.shapeZ<1&&motion.detailZ>=0&&motion.detailZ<1&&
            motion.weather[0]>=0&&motion.weather[0]<1,"long-session offsets remain finite and wrapped");
    CloudDynamics angle;
    base.windAngle=179;angle.Update(base,{},false,CloudWeather::Clear,0,12,0);
    base.windAngle=-179;
    Require(std::abs(angle.Update(base,{},false,CloudWeather::Clear,0,12,.1f).windAngle)>179,
            "wind direction takes the short path across the angle seam");
    base=CloudShape{};

    CloudResources resources;resources.Build(64,32,512);
    for(const auto* name:{"Fair","Arid","Dark","Coastal","Cold","Mountain","Lush","Open"})
    {
        EnvironmentConfiguration c=config;
        c.defaults.profile=std::string("Cloud")+name;
        const auto climate=ResolveEnvironment(c,{}).state;
        CloudDynamics clear;
        const auto shape=clear.Update(base,climate,true,CloudWeather::Clear,0,12,0);
        const float coverage=ColumnCoverage(Preview(shape),resources);
        std::cout<<"MEASURE "<<name<<" clear columns="<<coverage<<'\n';
        if (std::string(name)=="Open") Require(coverage==0,"open climate contains no density");
        if (std::string(name)=="Fair") Require(coverage>.01f&&coverage<.65f,"fair climate has small patches and substantial clear sky");
        for(const auto type:{CloudWeather::Rain,CloudWeather::Snow,CloudWeather::Sand})
        {
            CloudDynamics storm;
            const auto stormShape=storm.Update(base,climate,true,type,1,12,0);
            const float stormCoverage=ColumnCoverage(Preview(stormShape,300),resources);
            std::cout<<"MEASURE "<<name<<" weather="<<int(type)<<" columns="<<stormCoverage<<'\n';
            Require(stormCoverage>.98f,"storm density closes the sky even in sparse/open climates");
        }
    }
    auto c=Preview(ResolveCloudClimate(base,fair,true));c.windSpeed=0;
    Marcher t0(c,resources,{});
    auto later=c;later.time=300;Marcher t300(later,resources,{});
    auto next=c;next.time=1.f/60;Marcher tNext(next,resources,{});
    double change=0,frameChange=0;
    for(int y=0;y<32;++y) for(int x=0;x<32;++x) for(int z=0;z<16;++z)
    {
        const float h=(z+.5f)/16;
        const V3 p{(x+.5f)*c.weatherSize/32,(y+.5f)*c.weatherSize/32,c.bottom+(c.top-c.bottom)*h};
        const float d=t0.Density(p,h,true);
        change+=std::abs(d-t300.Density(p,h,true));frameChange+=std::abs(d-tNext.Density(p,h,true));
    }
    Require(change>.01&&frameChange<change*.01,"real noise density reshapes over minutes but stays coherent frame-to-frame");
    Require(Marcher::HeightGradient(.7f,0)==0&&Marcher::HeightGradient(.7f,1)>.9f,
            "flat and towering endpoint profiles are genuinely distinct");
    auto shadowConfig=c;shadowConfig.coverage=1;shadowConfig.time=300;
    Marcher shadow(shadowConfig,resources,{});
    const V3 light=Normalize({.4f,.2f,1.f});
    auto denseConfig=shadowConfig;denseConfig.density=4;
    Marcher denseShadow(denseConfig,resources,{});
    Require(shadow.SunShadow({1234,-4321,0},light)==denseShadow.SunShadow({1234,-4321,0},light),
            "shadow amount remains independent of optical density");
    Require(shadow.SunShadow({0,0,shadowConfig.top+100},light)==1,
            "above-deck receiver cannot be shadowed by clouds below it");
    // A receiver at sea level and its point on the base plane share a sun
    // ray, so they must integrate exactly the same evolving cloud column.
    const V3 receiver{1234,-4321,0};
    const V3 atBase=receiver+light*(shadowConfig.bottom/light.z);
    Require(Near(shadow.SunShadow(receiver,light),shadow.SunShadow(atBase,light),2e-5),
            "shadow sampling is anchored to the actual visible cloud altitude");
    const auto weatherSource=Read("WeatherVisuals.h");
    const auto detected=weatherSource.find("const int detected = ClassifyNativeWeather(primitiveCount, desc.Width, desc.Height,");
    const auto observed=weatherSource.find("nativeWeatherSeenThisFrame = detected",detected);
    const auto replacement=weatherSource.find("EnsureShaders(device)",detected);
    Require(detected!=std::string::npos&&observed<replacement&&
            weatherSource.find("if (!enabled || !effectEnabled || !active) return;",observed)<replacement,
            "native weather observation precedes optional effect gates and resource loading");
    {
        const auto rendererSource=Read("ModernWoWRenderer.cpp");
        const auto volume=Read("VolumeIntegration.h");
        const auto clouds=Read("src/Effects/VolumetricClouds.h");
        Require(rendererSource.find("if(place.valid&&(place.mapId!=g_weatherMapId||place.zoneId!=g_weatherZoneId))")
                    !=std::string::npos&&
                rendererSource.find("weathervisuals::ForgetLocation();")!=std::string::npos&&
                rendererSource.find("volume::ForgetWeather();")!=std::string::npos&&
                weatherSource.find("inline void ForgetLocation() noexcept")!=std::string::npos&&
                volume.find("inline void ForgetWeather() noexcept")!=std::string::npos&&
                clouds.find("void ForgetWeather() noexcept")!=std::string::npos,
                "a confirmed zone change re-arms both the weather detection and the deck it forced");
    }
    const auto source=Read("src/Effects/VolumetricClouds.cpp");
    const auto equation="float cov=saturate((w.r+(w.b-.5)*evolution.z-window.x)*window.y);";
    const auto first=source.find(equation);
    Require(first!=std::string::npos&&source.find(equation,first+1)!=std::string::npos,
            "visible and shadow shaders share the evolving weather-map equation");
    Require(source.find("Cloud(p+light*(baseDistance+(h-low)*reach),h)")!=std::string::npos,
            "production shadow integrates from the visible world-height cloud base");
    Require(source.find("windSpeed*m_elapsed")==std::string::npos,"production no longer recomputes wind from session age");
    // ----- Per-zone water colours: resolution, layering, blending -------------
    {
        Require(config.areas.size()==1,"water colours add an area layer, never a new zone");
        const auto booty=ResolveEnvironment(config,{530,33,35,true}).state;
        Require(booty.water.authored,"Booty Bay resolves authored water colours");
        // Area beats zone on every channel, and the bay is bluer than the teal coast
        // around it: that contrast is the whole point of authoring the area.
        Require(Near(booty.water.tint[2],0xCC/128.f)&&Near(booty.water.deep[1],0x44/255.f),
                "Booty Bay area colours win over its Stranglethorn zone");
        const auto vale=ResolveEnvironment(config,{530,33,0,true}).state;
        Require(vale.water.authored&&Near(vale.water.deep[2],0x4D/255.f)&&vale.water.tint[2]<booty.water.tint[2],
                "Stranglethorn keeps its own teal under the Booty Bay override");
        const auto eversong=ResolveEnvironment(config,{530,3430,0,true}).state;
        const auto quelThalas=ResolveEnvironment(config,{530,2037,0,true}).state;
        Require(eversong.water.authored&&Near(eversong.water.tint[2],0xB8/128.f)&&
                Near(eversong.water.deep[2],0x4A/255.f)&&Near(eversong.water.absorption[2],0x78/128.f)&&
                eversong.water.tint[2]<quelThalas.water.tint[2]&&eversong.water.deep[2]<quelThalas.water.deep[2],
                "Eversong's water profile is less blue than the shared Quel'thalas tint");
        // A zone that authors nothing keeps the shipped global base, unauthored,
        // so WaterEffect leaves GraphicsEffects.ini in charge there.
        const auto plain=ResolveEnvironment(config,{0,46,0,true}).state;
        Require(!plain.water.authored&&Near(plain.water.deep[1],42.f/255.f),
                "an unauthored zone keeps the global water colours");
        for (const uint32_t darkZone : {10u,8u,139u,361u,3520u,210u}) {
            const auto dark=ResolveEnvironment(config,{0,darkZone,0,true}).state;
            Require(dark.water.authored&&dark.water.deep[1]<42.f/255.f,
                    "gloomy zones author a darker deep colour than the global base");
        }
        const auto halfway=BlendEnvironment(plain,vale,.5f);
        Require(halfway.water.authored&&halfway.water.deep[2]>plain.water.deep[2]&&halfway.water.deep[2]<vale.water.deep[2],
                "water colours blend across a zone border instead of stepping");
        EnvironmentConfiguration broken;std::string colourError;
        Require(!ParseEnvironmentConfiguration("[Default]\nWaterColour=0xZZZZZZ\n",broken,colourError)&&
                !ParseEnvironmentConfiguration("[Zone.33]\nDeepColour=red\n",broken,colourError),
                "malformed water colours are rejected, not guessed at");
        // The precedence itself, pinned where it is now written: one resolver takes the zone
        // layer as an argument (so the draw and the underwater medium cannot resolve their
        // colours from different frames), replaces the global GraphicsEffects values with it
        // only where the zone authored one, and only where LocationGraphics.ini did not scope
        // an explicit override for this exact place.
        const auto waterEffect=Read("WaterEffect.h");
        Require(waterEffect.find("ResolveWaterColourSet(const renderer::ResolvedWaterColours& zoneWater)")!=std::string::npos&&
                waterEffect.find("if(zoneWater.authored) {")!=std::string::npos&&
                waterEffect.find("ResolveWaterColourSet(waveFrame.environment.water)")!=std::string::npos&&
                waterEffect.find("HasScopedValue(L\"Water\",L\"WaterColour\")")!=std::string::npos&&
                waterEffect.find("HasScopedValue(L\"Water\",L\"DeepColour\")")!=std::string::npos&&
                waterEffect.find("HasScopedValue(L\"Water\",L\"AbsorptionColour\")")!=std::string::npos,
                "zone water colours replace the global ones unless the location overrides them");
    }
    return 0;
}
catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; }
