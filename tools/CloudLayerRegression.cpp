// Portable reference checks for fixed cloud height and flying through the layer.
// g++ -O2 -std=c++20 -pthread tools/CloudLayerRegression.cpp -o build/cloud-layer-test
// Run from the repository root. The GPU/state checks live in AtmosphereRegression.
#define CLOUD_PREVIEW_NO_MAIN
#include "CloudPreview.cpp"
#include "../src/Effects/CloudDynamics.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

using renderer::CloudShape;
using renderer::SkyFallbackCloudType;
using renderer::SkyFallbackCoverage;

static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
static bool Near(float a, float b, float epsilon = .001f) { return std::fabs(a-b) < epsilon; }

int main() try
{
    // The direction-only fallback cannot query the world weather map without
    // inventing a world coordinate. It must preserve the climate-resolved
    // mean controls rather than making screen patches depend on yaw/pitch.
    {
        CloudShape shape;
        shape.coverage=-1.f;
        Require(SkyFallbackCoverage(shape)==0.f,
                "sky fallback coverage clamps clear-climate input to zero");
        shape.coverage=.5f;
        Require(Near(SkyFallbackCoverage(shape),.5f),
                "sky fallback uses the climate-resolved mean coverage directly");
        shape.coverage=2.f;
        Require(SkyFallbackCoverage(shape)==1.f,
                "sky fallback coverage clamps overcast-climate input to one");
        shape.coverage=std::numeric_limits<float>::quiet_NaN();
        Require(SkyFallbackCoverage(shape)==0.f,
                "invalid sky fallback coverage fails closed");
        shape.cloudType=-1.f;
        Require(SkyFallbackCloudType(shape)==0.f,
                "sky fallback maps the stratus climate endpoint");
        shape.cloudType=0.f;
        Require(Near(SkyFallbackCloudType(shape),.5f),
                "sky fallback maps neutral climate type to mean cumulus");
        shape.cloudType=1.f;
        Require(SkyFallbackCloudType(shape)==1.f,
                "sky fallback maps the cumulonimbus climate endpoint");
        shape.cloudType=std::numeric_limits<float>::infinity();
        Require(Near(SkyFallbackCloudType(shape),.5f),
                "invalid sky fallback type uses a neutral safe profile");
    }

    // Guard the production height profile as well as the reference math.
    std::ifstream source("src/Effects/VolumetricClouds.cpp");
    Require(bool(source), "production shader source is available");
    const std::string text((std::istreambuf_iterator<char>(source)), {});
    Require(text.find("float HeightFraction(float3 p) { return saturate((p.z-layer.x)*layer.z); }") != std::string::npos,
            "production height profile uses world Z, not camera-relative curvature");
    Require(text.find("if(SunDiscPixel(uv,linearRaw)>.5) return scene;") == std::string::npos &&
            text.find("float sunCentre=SunDiscPixel(uv,linearRaw);") != std::string::npos &&
            text.find("float sky=max(step((depthRange.z+1)*.5,linearRaw),max(moonCentre,sunCentre));") != std::string::npos &&
            text.find("if(moonCentre>.5)return scene;") == std::string::npos &&
            text.find("float filterCentreZ=lerp(centreZ,clearZ,max(moonCentre,sunCentre));") != std::string::npos &&
            text.find("float sunPixel=SunDiscPixel(uv,rawDepth);") != std::string::npos,
            "clouds shade verified sun and moon pixels as sky without treating their billboards as depth edges");
    const auto cloudMarchStart=text.find("const char* kCloudMarchSource = R\"HLSL(");
    const auto cloudMarchEnd=text.find(")HLSL\";",cloudMarchStart);
    const auto skyFallbackStart=text.find("const char* kCloudSkyFallbackSource = R\"HLSL(");
    const auto skyFallbackEnd=text.find(")HLSL\";",skyFallbackStart);
    const auto skyCompositeStart=text.find("const char* kCloudSkyCompositeSource = R\"HLSL(");
    const auto skyCompositeEnd=text.find(")HLSL\";",skyCompositeStart);
    const auto skyRenderStart=text.find("bool VolumetricClouds::RenderSkyFallback(");
    const auto skyRenderEnd=text.find("bool VolumetricClouds::Render(",skyRenderStart);
    const auto normalShaderStart=text.find("bool VolumetricClouds::EnsureShaders(");
    const auto optionalShaderStart=text.find("bool VolumetricClouds::EnsureSkyFallbackShaders(",normalShaderStart);
    Require(cloudMarchStart!=std::string::npos&&cloudMarchEnd!=std::string::npos&&
            skyFallbackStart!=std::string::npos&&skyFallbackEnd!=std::string::npos&&
            skyCompositeStart!=std::string::npos&&skyCompositeEnd!=std::string::npos&&
            skyRenderStart!=std::string::npos&&skyRenderEnd!=std::string::npos&&
            cloudMarchEnd>cloudMarchStart&&skyFallbackEnd>skyFallbackStart&&
            skyCompositeEnd>skyCompositeStart&&skyRenderEnd>skyRenderStart,
            "the production marcher, isolated sky fallback shaders, and render path are present");
    const std::string cloudMarch=text.substr(cloudMarchStart,cloudMarchEnd-cloudMarchStart);
    const std::string skyFallback=text.substr(skyFallbackStart,skyFallbackEnd-skyFallbackStart);
    const std::string skyComposite=text.substr(skyCompositeStart,skyCompositeEnd-skyCompositeStart);
    const std::string skyRender=text.substr(skyRenderStart,skyRenderEnd-skyRenderStart);
    Require(skyFallback.find("cameraPos")==std::string::npos&&
            skyFallback.find("cameraPosition")==std::string::npos&&
            skyFallback.find("shadowMap")==std::string::npos&&
            skyFallback.find("weatherMap")==std::string::npos&&
            skyFallback.find("weatherUv")==std::string::npos&&
            cloudMarch.find("sampler2D weatherMap:register(s3)")!=std::string::npos&&
            cloudMarch.find("tex2Dlod(weatherMap,float4(p.xy*motion.z+motion2.xy,0,0))")!=std::string::npos&&
            skyFallback.find("float cov=saturate(coverage.x);")!=std::string::npos&&
            skyFallback.find("float cloudType=saturate(coverage.y);")!=std::string::npos&&
            skyFallback.find("invView0")!=std::string::npos&&
            skyFallback.find("float4 volume:register(c10)")!=std::string::npos&&
            skyFallback.find("float4 shear:register(c11)")!=std::string::npos&&
            skyFallback.find("Density(float3 ray,float h,float cov,float cloudType,out float shapeSignal,out float detailSignal)")!=std::string::npos&&
            skyFallback.find("float gradient=Gradient(h,cloudType);")!=std::string::npos&&
            skyFallback.find("shapeSignal=saturate((shape.r-fbm+1)/max(2-fbm,1e-4));")!=std::string::npos&&
            skyFallback.find("float density=shapeSignal*gradient;")!=std::string::npos&&
            skyFallback.find("ray.x*scales.x+shear.x*h")!=std::string::npos&&
            skyFallback.find("ray.x*scales.z+shear.z*h")!=std::string::npos&&
            skyFallback.find("detailSignal=dfbm;")!=std::string::npos&&
            skyFallback.find("[loop] for(int i=0;i<16;++i)")!=std::string::npos&&
            skyFallback.find("float h=(i+.5)/16;")!=std::string::npos&&
            skyFallback.find("2.25/16")!=std::string::npos&&
            skyFallback.find("float bodyLight=.75+.25*saturate(shapeSignal);")!=std::string::npos&&
            skyFallback.find("float detailLight=.95+.05*saturate(detailSignal);")!=std::string::npos&&
            skyFallback.find("float verticalLight=.75+.25*h;")!=std::string::npos&&
            skyFallback.find("lerp(material.rgb,float3(1,1,1),.12+.20*h)")!=std::string::npos&&
            skyFallback.find("density=saturate((density-lerp(1-dfbm,dfbm,h)*erosion)")!=std::string::npos&&
            skyFallback.find("smoothstep(-.08,.16,ray.z)")==std::string::npos&&
            skyFallback.find("scattered*=fade")==std::string::npos&&
            cloudMarch.find("smoothstep(0,max(groundColor.w,.001),ray.z)")==std::string::npos&&
            cloudMarch.find("float fadeWidth=max(groundColor.w,.1);")!=std::string::npos&&
            cloudMarch.find("float fadeStart=layer.w*(1-fadeWidth);")!=std::string::npos&&
            cloudMarch.find("float coarseStride=max(min(stepLength*stepping.z,.75/scales.x),")!=std::string::npos&&
            cloudMarch.find("(exitDistance-t)/(depthRange.w-i)")!=std::string::npos&&
            cloudMarch.find("d=lerp(d,eroded,saturate(h*10));")!=std::string::npos&&
            skyComposite.find("cameraPos")==std::string::npos&&
            skyComposite.find("shadowMap")==std::string::npos&&
            skyComposite.find("float sky=step((depthRange.z+1)*.5,rawDepth);")!=std::string::npos&&
            skyComposite.find("float waterPixel=step(.35,max(water.r,water.g));")!=std::string::npos&&
            skyRender.find("cameraPosition")==std::string::npos&&
            skyRender.find("m_shadowValid=false;")!=std::string::npos&&
            skyRender.find("m_shadowSurface")==std::string::npos&&
            skyRender.find("m_shadowTexture")==std::string::npos&&
            skyRender.find("m_weatherTexture")==std::string::npos,
            "the fallback is direction-only, uses climate-resolved coverage/type without weather-map projection, and excludes geometry, water, held eyes, and world shadows");
    Require(text.find("constexpr float kSkyFallbackReferenceDistance = 6000.f;")!=std::string::npos&&
            skyRender.find("settings.planetRadius")==std::string::npos&&
            skyRender.find("kSkyFallbackReferenceDistance/featureSize")!=std::string::npos&&
            skyRender.find("kSkyFallbackReferenceDistance/detailSize")!=std::string::npos&&
            skyRender.find("const float volume[4]={depthSpan/featureSize,depthSpan/detailSize,0.f,0.f};")!=std::string::npos&&
            skyRender.find("const float shear[4]={std::cos(windRadians)*settings.windShear")!=std::string::npos&&
            skyRender.find("std::sin(windRadians)*settings.windShear*featureSize/detailSize")!=std::string::npos&&
            skyRender.find("d->SetPixelShaderConstantF(10,volume,1);d->SetPixelShaderConstantF(11,shear,1);")!=std::string::npos&&
            skyRender.find("SkyFallbackCoverage(settings)")!=std::string::npos&&
            skyRender.find("SkyFallbackCloudType(settings)")!=std::string::npos&&
            skyRender.find("const float coverageConstants[4]={fallbackCoverage,fallbackCloudType,0.f,0.f};")!=std::string::npos&&
            skyRender.find("std::max(skyR*ambientLevel,kCloudSkyAmbientFloor[0])*color[0]")!=std::string::npos&&
            skyRender.find("f.directLightValid&&f.directLightDirectionConfirmed")!=std::string::npos&&
            text.find("<<\",angularScale=\"<<kSkyFallbackReferenceDistance/std::max(settings.featureSize,1.f)")!=std::string::npos&&
            text.find("<<\",shapeDepth=\"<<std::clamp(settings.thickness,100.f,12000.f)/std::max(settings.featureSize,1.f)")!=std::string::npos&&
            text.find("<<\",windShear=\"<<settings.windShear")!=std::string::npos&&
            text.find("<<\",cloudAmbient=\"<<fallbackCloudR")!=std::string::npos&&
            text.find("skyField=(samples=16,coverageSource=climate-mean,coverage=")!=std::string::npos&&
            text.find("weatherField=unused")!=std::string::npos,
            "screen-space noise uses a medium fixed scale, climate-resolved coverage/type, layer-depth shear, and the existing zone-tinted ambient without planet-radius or weather-map projection");
    {
        const float height=.2f,shapeSignal=.5f,detailSignal=.5f;
        const float nightMaterialG=std::max(.211765f*.5f,.10f);
        const float whiteMix=.12f+.20f*height;
        const float nightCloudRadiance=(nightMaterialG+(1-nightMaterialG)*whiteMix)*
            (.75f+.25f*height)*(.75f+.25f*shapeSignal)*(.95f+.05f*detailSignal);
        Require(nightCloudRadiance>.16f,
                "night fallback keeps a visible ambient cloud radiance while preserving shape and detail modulation");
    }
    Require(text.find("constexpr float kCloudSkyAmbientFloor[3] = {.08f,.10f,.14f};")!=std::string::npos&&
            text.find("constexpr float kCloudGroundAmbientFloor[3] = {.05f,.065f,.09f};")!=std::string::npos&&
            text.find("cloudSkyR=std::max(skyR*ambientLevel,kCloudSkyAmbientFloor[0])")!=std::string::npos&&
            text.find("cloudGroundR=std::max(skyR*ambientLevel*.62f,kCloudGroundAmbientFloor[0])")!=std::string::npos&&
            text.find("settings.ambientIntensity*ambientOcclusion")!=std::string::npos&&
            text.find("settings.ambientIntensity*ambientLevel*ambientOcclusion") == std::string::npos&&
            text.find("const float c16[4]={cloudSkyR*cloudColourMultiplier[0]")!=std::string::npos&&
            text.find("const float c17[4]={cloudGroundR*cloudColourMultiplier[0]")!=std::string::npos&&
            text.find("cloudAmbient=(")!=std::string::npos,
            "normal cloud ambient applies day/night once and keeps a subdued tinted floor in zones with black native ambient");
    Require(normalShaderStart!=std::string::npos&&optionalShaderStart!=std::string::npos&&
            text.substr(normalShaderStart,optionalShaderStart-normalShaderStart).find("kCloudSkyFallbackSource")==std::string::npos&&
            text.find("m_skyShaderRetry.Fail(now,kOptionalPassRetryMs);")!=std::string::npos,
            "optional sky shader compilation failure cannot disable normal cloud shaders");
    Require(text.find("m_fieldsDevice=d;") != std::string::npos &&
            text.find("device==nullptr || (m_fieldsDevice && m_fieldsDevice!=device)") != std::string::npos,
            "cloud noise textures are recreated when the D3D device implementation changes");
    const auto resize = text.find("// A backbuffer resize only invalidates size-dependent targets.");
    Require(resize != std::string::npos &&
            text.find("Reset(d);m_owner=d;m_fullWidth=w", resize) == std::string::npos,
            "cloud target resize preserves compiled shaders instead of forcing a full pass reset");
    Require(text.find("// A device recreation must release resources owned by the discarded device;") != std::string::npos,
            "device replacement releases stale cloud COM resources rather than retaining the old device");
    Require(text.find("if(!m_shaderRetry.Open(now))return false;") != std::string::npos &&
            text.find("m_shaderRetry.Fail(now,kOptionalPassRetryMs);") != std::string::npos,
            "transient cloud shader creation failures retry after a cooldown instead of disabling clouds until device reset");
    const auto workerWait = text.find("const bool generating=m_fieldWorker->generating.load");
    const auto cacheRead = text.find("if(!LoadFields(m_cachePath,shape,detail,weather))");
    Require(text.find("std::thread([worker,cachePath]") != std::string::npos &&
            workerWait != std::string::npos && cacheRead != std::string::npos && workerWait < cacheRead,
            "cold-start cloud generation owns its async state and avoids per-frame cache I/O while the worker runs");
    Require(text.find("const float layer[4]={settings.baseAltitude,layerTop,1.f/thickness,settings.maxDistance};") != std::string::npos,
            "production march receives configured absolute layer bounds");
    Require(text.find("cameraLayer=") != std::string::npos &&
            text.find("forwardZ=") != std::string::npos &&
            text.find("top=\"<<layerTop") != std::string::npos,
            "cloud diagnostics identify whether a pitched camera is above the fixed world-Z layer");
    Require(text.find("++m_renderFrames==1||m_renderFrames%600==0") != std::string::npos &&
            text.find("m_lastDiagnosticState!=event") != std::string::npos &&
            text.find("logState(\"render=active\")") != std::string::npos,
            "cloud diagnostics record immediately, on skip transitions, and when rendering recovers");
    const auto cloudDynamicsUpdate=text.find("static_cast<CloudShape&>(settings)=m_dynamics.Update");
    const auto layerRefresh=text.find("refreshLayerDiagnostic();",cloudDynamicsUpdate);
    Require(cloudDynamicsUpdate!=std::string::npos && layerRefresh!=std::string::npos &&
            layerRefresh>cloudDynamicsUpdate,
            "the logged and rendered layer bounds follow the live cloud transition settings");
    Require(cloudMarch.find("float fadeWidth=max(groundColor.w,.1);")!=std::string::npos&&
            cloudMarch.find("float fade=1-smoothstep(fadeStart,layer.w,entry);")!=std::string::npos,
            "cloud opacity is not pitched off at the horizon and fades only at the finite-distance boundary");
    Require(text.find("if(maxDistance==26000)maxDistance=200000;") != std::string::npos,
            "existing installs migrate the former 26,000-unit default to the wider sky reach");

    CloudConfig c;
    c.bottom = 900; c.top = 2300; c.windSpeed = 0;
    CloudResources resources;
    resources.Build(64, 32, 512);
    Marcher ground(c, resources, {0,0,0}), flying(c, resources, {6000,-4000,8000});
    const V3 sample{4500,-3000,1600};
    Require(Near(ground.HeightFraction(sample), .5f) &&
            Near(ground.HeightFraction(sample), flying.HeightFraction(sample)),
            "cloud height at a fixed world point is independent of camera XYZ");
    Require(ground.Density(sample, .5f, true) == flying.Density(sample, .5f, true),
            "flying cannot change density at a fixed world point");
    float entry, exit;
    Require(ground.LayerRange({0,0,0}, {0,0,1}, entry, exit) && Near(entry,900) && Near(exit,2300),
            "below deck: ray reaches configured absolute base and top");
    Require(ground.LayerRange({7000,-4000,800}, {0,0,1}, entry, exit) && Near(entry,100),
            "approaching cloud base reduces distance instead of lifting the deck");
    Require(flying.LayerRange({7000,-4000,1600}, {0,0,1}, entry, exit) && Near(entry,0) && Near(exit,700),
            "inside deck: upward ray starts in cloud");
    Require(flying.LayerRange({7000,-4000,3000}, {0,0,-1}, entry, exit) && Near(entry,700) && Near(exit,2100),
            "above deck: downward ray hits cloud tops");
    Require(!flying.LayerRange({0,0,3000}, {0,0,1}, entry, exit), "above deck: upward sky is clear");
    Require(!ground.LayerRange({0,0,0}, {1,0,0}, entry, exit), "horizontal ray below layer misses");
    Require(flying.LayerRange({0,0,1600}, {1,0,0}, entry, exit) && Near(entry,0) && Near(exit,c.maxDistance),
            "horizontal ray inside layer stays finite and reaches march limit");
    Require(ground.LayerRange({0,0,c.bottom}, {0,0,1}, entry, exit) && Near(entry,0),
            "exact cloud base has no intersection gap");
    Require(flying.LayerRange({0,0,c.top}, {0,0,-1}, entry, exit) && Near(entry,0),
            "exact cloud top has no intersection gap");

    for (const V3 view : {Normalize({1,0,.2f}), Normalize({-1,1,.8f}), V3{0,0,1}}) {
        const V3 camera{7000,-4000,800};
        Require(ground.LayerRange(camera,view,entry,exit) &&
                Near((camera+view*entry).z,c.bottom) && Near((camera+view*exit).z,c.top),
                "changing view direction does not move the cloud base or top");
    }
    {
        constexpr float pi=3.14159265358979323846f;
        const float heights[]={0.f,899.f,900.f,1600.f,2300.f,2301.f,8000.f};
        unsigned checked=0;
        for (float height : heights) for (int yawDeg=0;yawDeg<360;yawDeg+=15)
            for (int pitchDeg=-88;pitchDeg<=88;pitchDeg+=4) {
                const float yaw=float(yawDeg)*pi/180.f,pitch=float(pitchDeg)*pi/180.f;
                const V3 ray=Normalize({std::cos(yaw)*std::cos(pitch),
                                        std::sin(yaw)*std::cos(pitch),std::sin(pitch)});
                const V3 camera{7000,-4000,height};
                bool expected=false;
                if (std::fabs(ray.z)<1e-6f) {
                    expected=height>=c.bottom&&height<=c.top;
                } else {
                    const float a=(c.bottom-height)/ray.z,b=(c.top-height)/ray.z;
                    const float first=std::max(0.f,std::min(a,b)),last=std::max(a,b);
                    expected=last>first&&first<c.maxDistance;
                }
                const bool hit=ground.LayerRange(camera,ray,entry,exit);
                if (hit!=expected) throw std::runtime_error("cloud intersection changed across camera height/view sweep");
                if (hit && exit<=entry) throw std::runtime_error("cloud layer sweep produced an empty interval");
                ++checked;
            }
        Require(checked>4000,"absolute cloud-layer intersections survive a dense yaw/pitch/height sweep");
    }

    c.coverage=1.f;
    float transmittance, depth;
    Marcher upward(c,resources,{0,0,0});
    unsigned visibleElevations=0;
    for(float pitchDegrees : {.3f,.35f,.5f,.75f,1.f,2.f,3.f,5.f,15.f,45.f,89.f}) {
        const float pitch=pitchDegrees*3.14159265358979323846f/180.f;
        const V3 ray=Normalize({0,std::cos(pitch),std::sin(pitch)});
        upward.March({0,0,0},ray,{0,0,1},.5f,transmittance,depth);
        if(transmittance>=.99f)
            throw std::runtime_error("cloud deck disappeared during the upward camera-elevation sweep");
        ++visibleElevations;
    }
    Require(visibleElevations==11,
            "cloud opacity survives shallow-above-horizon through near-zenith views");
    const V3 light{0,0,1};
    flying.March({0,0,3000}, {0,0,1}, light, 0, transmittance, depth);
    Require(transmittance==1.f, "above the deck, the view march leaves overhead sky clear");
    ground.March({0,0,0}, {0,0,1}, light, 0, transmittance, depth, 100);
    Require(transmittance==1.f, "near geometry clips the march before the cloud base");
    int insideCloud=0, aboveCloud=0;
    for(int x=-4;x<=4;++x) {
        flying.March({float(x)*700,0,1600}, {0,0,1}, light, 0, transmittance, depth);
        insideCloud += transmittance < .95f;
        flying.March({float(x)*700,0,3000}, {0,0,-1}, light, 0, transmittance, depth, 5000);
        aboveCloud += transmittance < .95f;
    }
    Require(insideCloud>0, "camera inside the deck integrates cloud around it");
    Require(aboveCloud>0, "camera above the deck sees clouds in front of distant terrain");
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
