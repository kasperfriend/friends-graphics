// Portable reference checks for fixed cloud height and flying through the layer.
// g++ -O2 -std=c++20 -pthread tools/CloudLayerRegression.cpp -o build/cloud-layer-test
// Run from the repository root. The GPU/state checks live in AtmosphereRegression.
#define CLOUD_PREVIEW_NO_MAIN
#include "CloudPreview.cpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
static bool Near(float a, float b, float epsilon = .001f) { return std::fabs(a-b) < epsilon; }

int main() try
{
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
    const auto skyFallbackStart=text.find("const char* kCloudSkyFallbackSource = R\"HLSL(");
    const auto skyFallbackEnd=text.find(")HLSL\";",skyFallbackStart);
    const auto skyCompositeStart=text.find("const char* kCloudSkyCompositeSource = R\"HLSL(");
    const auto skyCompositeEnd=text.find(")HLSL\";",skyCompositeStart);
    const auto skyRenderStart=text.find("bool VolumetricClouds::RenderSkyFallback(");
    const auto skyRenderEnd=text.find("bool VolumetricClouds::Render(",skyRenderStart);
    const auto normalShaderStart=text.find("bool VolumetricClouds::EnsureShaders(");
    const auto optionalShaderStart=text.find("bool VolumetricClouds::EnsureSkyFallbackShaders(",normalShaderStart);
    Require(skyFallbackStart!=std::string::npos&&skyFallbackEnd!=std::string::npos&&
            skyCompositeStart!=std::string::npos&&skyCompositeEnd!=std::string::npos&&
            skyRenderStart!=std::string::npos&&skyRenderEnd!=std::string::npos&&
            skyFallbackEnd>skyFallbackStart&&skyCompositeEnd>skyCompositeStart&&
            skyRenderEnd>skyRenderStart,
            "the isolated sky fallback shaders and render path are present");
    const std::string skyFallback=text.substr(skyFallbackStart,skyFallbackEnd-skyFallbackStart);
    const std::string skyComposite=text.substr(skyCompositeStart,skyCompositeEnd-skyCompositeStart);
    const std::string skyRender=text.substr(skyRenderStart,skyRenderEnd-skyRenderStart);
    Require(skyFallback.find("cameraPos")==std::string::npos&&
            skyFallback.find("cameraPosition")==std::string::npos&&
            skyFallback.find("shadowMap")==std::string::npos&&
            skyFallback.find("invView0")!=std::string::npos&&
            skyFallback.find("lerp(material.rgb,float3(1,1,1),.15)")!=std::string::npos&&
            skyComposite.find("cameraPos")==std::string::npos&&
            skyComposite.find("shadowMap")==std::string::npos&&
            skyComposite.find("float sky=step((depthRange.z+1)*.5,rawDepth);")!=std::string::npos&&
            skyComposite.find("float waterPixel=step(.35,max(water.r,water.g));")!=std::string::npos&&
            skyRender.find("cameraPosition")==std::string::npos&&
            skyRender.find("m_shadowValid=false;")!=std::string::npos&&
            skyRender.find("m_shadowSurface")==std::string::npos&&
            skyRender.find("m_shadowTexture")==std::string::npos,
            "the fallback is direction-only, excludes water/geometry, and cannot read a held eye or render a world shadow");
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
    Require(text.find("fade*=1-smoothstep(layer.w*.9,layer.w,entry);") != std::string::npos,
            "distance fade preserves cloud opacity until the final tenth of the march range");
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
    for(float pitchDegrees : {.5f,.75f,1.f,2.f,3.f,5.f,15.f,45.f,89.f}) {
        const float pitch=pitchDegrees*3.14159265358979323846f/180.f;
        const V3 ray=Normalize({0,std::cos(pitch),std::sin(pitch)});
        upward.March({0,0,0},ray,{0,0,1},.5f,transmittance,depth);
        if(transmittance>=.99f)
            throw std::runtime_error("cloud deck disappeared during the upward camera-elevation sweep");
        ++visibleElevations;
    }
    Require(visibleElevations==9,
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
