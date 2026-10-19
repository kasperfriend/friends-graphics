// Portable policy regressions. Run from any directory; no Windows SDK needed.
#include "../src/Water/WaterPolicy.h"
#include "../src/Water/WaveField.h"
#include "../src/Water/WaterNoise.h"
#include "../src/Water/WaterSeaState.h"
#include "../src/Water/WaterQuality.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <initializer_list>
#include <string>
#include <vector>

using namespace renderer::water;

// Read a source file that sits next to this test or next to the object it builds. The two
// normalizations below live only in shader text, and text is the only thing a portable test can
// hold a production shader to without a D3D9 device.
static std::string ReadAlongside(const char* name)
{
    const std::string here(__FILE__);
    const size_t slash = here.find_last_of("/\\");
    std::vector<std::string> candidates;
    if (slash != std::string::npos) candidates.push_back(here.substr(0, slash + 1) + name);
    candidates.push_back(name);
    candidates.push_back(std::string("tools/") + name);
    for (const std::string& path : candidates)
    {
        std::ifstream in(path);
        if (in) return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

void Require(bool value, const char* name)
{
    if (!value) throw std::runtime_error(name);
    std::printf("PASS %s\n", name);
}

static std::array<float,3> RotateView(const std::array<float,3>& v,float yaw,float pitch,float roll)
{
    const float cr=std::cos(roll),sr=std::sin(roll),cp=std::cos(pitch),sp=std::sin(pitch),cy=std::cos(yaw),sy=std::sin(yaw);
    const std::array<float,3> r={v[0],cr*v[1]-sr*v[2],sr*v[1]+cr*v[2]};
    const std::array<float,3> p={cp*r[0]+sp*r[2],r[1],-sp*r[0]+cp*r[2]};
    return {cy*p[0]-sy*p[1],sy*p[0]+cy*p[1],p[2]};
}

static float WorldPlaneSlope(const std::array<float,3>& normal,const std::array<float,3>& up)
{
    const float upLength=std::sqrt(up[0]*up[0]+up[1]*up[1]+up[2]*up[2]);
    const std::array<float,3> u={up[0]/upLength,up[1]/upLength,up[2]/upLength};
    const float vertical=normal[0]*u[0]+normal[1]*u[1]+normal[2]*u[2];
    const float hx=normal[0]-u[0]*vertical,hy=normal[1]-u[1]*vertical,hz=normal[2]-u[2]*vertical;
    return std::sqrt(hx*hx+hy*hy+hz*hz)/std::max(std::fabs(vertical),1e-6f);
}

int main() try
{
    const float validFog[4]={-.001f,1.f,1.f,0.f}, invalidFog[4]={0.f,0.f,0.f,0.f};
    const float nanFog[4]={std::numeric_limits<float>::quiet_NaN(),0.f,1.f,0.f};
    Require(HasFogCurve(validFog)&&!HasFogCurve(invalidFog)&&!HasFogCurve(nanFog),
            "water c4 accepts only a finite fog-curve shape; malformed camera variants use the no-fog fallback");
    {
        const std::array<float,3> up={0.f,0.f,1.f},normal={-.31f,.22f,1.f};
        const float expected=std::hypot(.31f,.22f);
        for(const auto angles: {std::array<float,3>{0.f,0.f,0.f},
                                std::array<float,3>{.7f,.4f,-.3f},
                                std::array<float,3>{-1.1f,.9f,.6f},
                                std::array<float,3>{2.2f,-.65f,1.4f}}) {
            const auto viewUp=RotateView(up,angles[0],angles[1],angles[2]);
            const float measured=WorldPlaneSlope(RotateView(normal,angles[0],angles[1],angles[2]),viewUp);
            Require(std::fabs(measured-expected)<2e-5f,
                    "foam bed slope is unchanged by camera orientation when computed from the world-up normal");
            const auto surface=RotateView({5.f,-8.f,12.f},angles[0],angles[1],angles[2]);
            const auto bed=RotateView({5.f,-8.f,4.f},angles[0],angles[1],angles[2]);
            const float verticalDepth=(surface[0]-bed[0])*viewUp[0]
                +(surface[1]-bed[1])*viewUp[1]+(surface[2]-bed[2])*viewUp[2];
            Require(std::fabs(verticalDepth-8.f)<2e-5f,
                    "shore foam depth is the same world vertical distance under every camera orientation");
        }
    }
    {
        // The bed slope is measured over a yard-scale baseline, not over one pixel: the
        // captured depth this pass reads back is not exact (an INTZ fetch can hand back a
        // floating-point depth with a short mantissa), and a one-pixel stencil divides
        // that storage step by a bed footprint inches across, so the slope - and the
        // shoal/beach foam thresholds riding on it - jittered a derivative quad at a
        // time. Model the round trip on a plane of known slope at a real resolution's
        // footprint and pin the property the shader's taps exist for: the storage-step
        // jitter is suppressed by an order of magnitude while the reading stays true.
        const int gw=512, gh=320;
        const float pitch=0.384f;                      // 22 degrees down
        const std::array<float,3> up{0.f,std::cos(pitch),-std::sin(pitch)};
        const float trueSlope=0.28f, tilt=std::atan(trueSlope);
        const float ct=std::cos(-tilt), st=std::sin(-tilt);
        std::array<float,3> bedN{0.f,ct*up[1]-st*up[2],st*up[1]+ct*up[2]};
        const float nl=std::sqrt(bedN[0]*bedN[0]+bedN[1]*bedN[1]+bedN[2]*bedN[2]);
        bedN={bedN[0]/nl,bedN[1]/nl,bedN[2]/nl};
        const double planeD=bedN[0]*2.0+bedN[1]*-6.0+bedN[2]*55.0;
        const double p11=2.1445, p00=p11/(double(gw)/double(gh)), nearZ=0.5, farZ=2000.0;
        std::vector<std::array<float,3>> bed(size_t(gw)*size_t(gh));
        std::vector<std::array<float,3>> exact(size_t(gw)*size_t(gh));
        for(int j=0;j<gh;++j) for(int i=0;i<gw;++i) {
            const double dx=(2.0*(i+0.5)/gw-1.0)/p00, dy=(1.0-2.0*(j+0.5)/gh)/p11;
            const double t=planeD/(bedN[0]*dx+bedN[1]*dy+bedN[2]);
            exact[size_t(j)*gw+i]={float(dx*t),float(dy*t),float(t)};
            // Nonlinear depth stored the way a short-mantissa float format stores it.
            double raw=farZ/(farZ-nearZ)*(t-nearZ)/t;
            const double step=std::ldexp(1.0,int(std::floor(std::log2(raw)))-16);
            raw=std::floor(raw/step+0.5)*step;
            const float tq=float(farZ*nearZ/(farZ-raw*(farZ-nearZ)));
            bed[size_t(j)*gw+i]={float(dx)*tq,float(dy)*tq,tq};
        }
        const auto sub=[](const std::array<float,3>& a,const std::array<float,3>& b){
            return std::array<float,3>{a[0]-b[0],a[1]-b[1],a[2]-b[2]}; };
        const auto slopeOf=[&](const std::array<float,3>& tx,const std::array<float,3>& ty){
            const std::array<float,3> n{tx[1]*ty[2]-tx[2]*ty[1],tx[2]*ty[0]-tx[0]*ty[2],tx[0]*ty[1]-tx[1]*ty[0]};
            return WorldPlaneSlope(n,up); };
        const auto at=[&](const std::vector<std::array<float,3>>& f,int i,int j)->const std::array<float,3>&{
            return f[size_t(j)*gw+i]; };
        // The tap distance the shader derives from the same footprint at the centre.
        const auto cxStep=sub(at(exact,gw/2+1,gh/2),at(exact,gw/2,gh/2));
        const auto cyStep=sub(at(exact,gw/2,gh/2+1),at(exact,gw/2,gh/2));
        const float yardsPerPixel=std::max(std::sqrt(cxStep[0]*cxStep[0]+cxStep[1]*cxStep[1]+cxStep[2]*cxStep[2]),
                                           std::sqrt(cyStep[0]*cyStep[0]+cyStep[1]*cyStep[1]+cyStep[2]*cyStep[2]));
        const int tap=std::clamp(int(std::lround(1.5f/yardsPerPixel)),1,16);
        std::vector<float> oldSlope(size_t(gw)*gh,-1.f), newSlope(size_t(gw)*gh,-1.f);
        for(int j=0;j+1<gh;++j) for(int i=0;i+1<gw;++i)
            oldSlope[size_t(j)*gw+i]=slopeOf(sub(at(bed,i+1,j),at(bed,i,j)),sub(at(bed,i,j+1),at(bed,i,j)));
        for(int j=tap;j+tap<gh;++j) for(int i=tap;i+tap<gw;++i)
            newSlope[size_t(j)*gw+i]=slopeOf(sub(at(bed,i+tap,j),at(bed,i-tap,j)),sub(at(bed,i,j+tap),at(bed,i,j-tap)));
        const auto jitter=[&](const std::vector<float>& f){
            double sum=0; size_t n=0;
            for(int j=0;j<gh;++j) for(int i=0;i<gw;++i) {
                const float v=f[size_t(j)*gw+i]; if(v<0) continue;
                if(i+1<gw&&f[size_t(j)*gw+i+1]>=0){ sum+=std::fabs(f[size_t(j)*gw+i+1]-v); ++n; }
                if(j+1<gh&&f[size_t(j+1)*gw+i]>=0){ sum+=std::fabs(f[size_t(j+1)*gw+i]-v); ++n; }
            }
            return sum/double(n); };
        std::vector<float> newErr;
        for(const float v: newSlope) if(v>=0) newErr.push_back(std::fabs(v-trueSlope));
        std::sort(newErr.begin(),newErr.end());
        const double oldJit=jitter(oldSlope), newJit=jitter(newSlope);
        Require(oldJit>0.004,
                "one-pixel bed-slope derivatives jitter on short-mantissa captured depth (the foam tick mechanism)");
        Require(newJit*6.0<oldJit,
                "the yard-scale bed-slope baseline suppresses captured-depth storage jitter by an order of magnitude");
        Require(newErr[newErr.size()/2]<0.015f,
                "the yard-scale bed-slope baseline still reads the bed's true slope");
    }
    Require(IsMaterialLayout(8,64,512,512), "canonical liquid material accepted");
    Require(!IsMaterialLayout(8,64,64,64), "weather ripple LUT alone is not water");
    Require(!IsMaterialLayout(512,512,8,64), "swapped slots are not the supported shader ABI");
    Require(!IsMaterialLayout(0,0,512,512), "missing base texture rejected");
    Require(!IsMaterialLayout(8,64,0,0), "missing surface texture rejected");
    Require(!IsMaterialLayout(512,512,512,512), "ordinary terrain material rejected");
    for (uint64_t pixel : {0x17f042a7906ca126ull, 0x7d4f078fa1876a09ull})
    for (uint64_t vertex : {0x206d861fd0a721ddull, 0xfdd9528ed3ac30eaull,
                            0x48a82796bd612aebull, 0x70faf83955e2b668ull})
    {
        for (unsigned size : {16u, 64u, 128u, 256u, 512u, 1024u})
            for (unsigned w=8,h=64; h; w=w>1?w/2:1,h/=2)
                Require(IsMaterialLayout(w,h,size,size,vertex,pixel),
                        "captured water families survive texture resolution and top-mip changes");
        Require(!IsMaterialLayout(8,64,256,256,vertex,0), "known VS alone cannot identify water");
        Require(!IsMaterialLayout(8,64,256,256,pixel,vertex), "shader roles cannot be swapped");
        Require(!IsMaterialLayout(64,64,256,256,vertex,pixel), "known pair with wrong lookup rejected");
        Require(!IsMaterialLayout(4,32,256,128,vertex,pixel), "non-square surface rejected");
        Require(!IsMaterialLayout(4,32,127,127,vertex,pixel), "non-power-of-two surface rejected");
        Require(!IsMaterialLayout(0,0,256,256,vertex,pixel), "known pair cannot bypass missing lookup");
        Require(!IsMaterialLayout(4,32,0,0,vertex,pixel), "known pair cannot bypass missing surface");
    }
    constexpr uint64_t pixel = 0x7d4f078fa1876a09ull;
    constexpr uint64_t otherPixel = 0x17f042a7906ca126ull;
    Require(!IsMaterialLayout(8,64,256,256,0,0) && !IsMaterialLayout(4,32,256,256,0,0),
            "unidentified shaders at a reduced layout stay native");
    Require(IsMaterialLayout(8,64,256,256,0x17f042a7906ca126ull,pixel),
            "liquid pixel shader carries unlisted vertex variants");
    Require(IsMaterialLayout(8,64,128,128,0,otherPixel) && IsMaterialLayout(8,64,1024,1024,0,otherPixel),
            "reduced and enlarged liquid surfaces are one material family");
    Require(!IsMaterialLayout(8,64,256,256,0x8832bcc5ed4ff2a6ull,pixel),
            "native rain vertex shader cannot borrow the liquid pixel shader");
    Require(!IsMaterialLayout(8,64,256,256,0xec9ee827b8bf5afaull,pixel),
            "native snow vertex shader cannot borrow the liquid pixel shader");
    Require(!IsMaterialLayout(8,64,64,64,0xd9e7756460af6296ull,pixel), "UI shader with ripple texture rejected");
    Require(!IsMaterialLayout(8,64,64,64,0,0xc29c7060b723c0c6ull), "UI pixel shader is never liquid");
    Require(IsLiquidCandidate(8,64,64,64) && IsLiquidCandidate(4,32,256,256),
            "an unapproved liquid layout is still reported as a candidate");
    Require(!IsLiquidCandidate(512,512,256,256) && !IsLiquidCandidate(8,64,300,300),
            "candidate reporting does not widen what is restyled");
    for (uint64_t vertex : {0x206d861fd0a721ddull, 0xfdd9528ed3ac30eaull,
                            0x48a82796bd612aebull, 0x70faf83955e2b668ull})
        Require(FogConstantRegister(vertex)==4u,
                "every documented liquid vertex shader shares one fog register");
    Require(FogConstantRegister(0x2437f34e978bb37full)==0u && FogConstantRegister(pixel)==0u,
            "fog capture stays closed to unlisted shaders");
    for (unsigned bits=0; bits<32; ++bits)
        Require(NeedsScene(bits&1,bits&2,bits&4,bits&8,bits&16)==(bits!=0),
                "every depth-dependent feature requests capture independently");
    Require(TargetsMatch(1920,1080,1920,1080), "same-size mask/depth pair can be reused");
    Require(!TargetsMatch(1280,720,1920,1080), "window resize invalidates BOTH targets");
    Require(!TargetsMatch(1920,720,1920,1080), "height-only resize invalidates targets");
    Require(!TargetsMatch(1280,1080,1920,1080), "width-only resize invalidates targets");
    Require(!TargetsMatch(0,0,0,0), "uninitialized targets are never valid");

    // Wave field. The shader evaluates the lattice from the position relative to
    // the camera plus a phase that carries the camera's own world position. Two
    // statements make that correct and neither holds by construction:
    //
    //  * the same terrain point seen from two cameras must land on the same
    //    lattice coordinate, otherwise the pattern slides over the world;
    //  * that coordinate must stay inside the noise wrap, because a lattice
    //    index outside it is where the previous shader lost the float32
    //    fraction bits and hardened the noise into flat squares.
    auto coordinate=[](const WaveField& field,float relativeX,float relativeY) {
        return WrapLattice(relativeX*field.lattice[0]+relativeY*field.lattice[1]+field.ripplePhase[0]);
    };
    auto warpCoordinate=[](const WaveField& field,float relativeX,float relativeY) {
        const float x=relativeX*field.lattice[0]+relativeY*field.lattice[1];
        const float y=relativeX*field.lattice[2]+relativeY*field.lattice[3];
        return std::array<float,2>{
            WrapLattice((x*kWaveWarpCos+y*kWaveWarpSin)*kWaveWarpFrequency+field.warpPhase[0]),
            WrapLattice((-x*kWaveWarpSin+y*kWaveWarpCos)*kWaveWarpFrequency+field.warpPhase[1])};
    };
    auto distance=[](float a,float b) {
        const float difference=std::fabs(a-b);
        return std::fmin(difference,kLatticeWrap-difference);
    };
    for (double time : {0.0, 3.7, 61.0, 3600.0, 100000.0})
    for (float counter : {0.f, .32f, 1.f})
    for (float scale : {25.f, 100.f, 400.f})
    {
        const auto reference=BuildWaveField(time,1.f,counter,scale,0.f,0.f,true,true);
        // Any camera position on any continent, at the same distance and in the
        // same direction from the wave as the reference point.
        for (float camera : {-46321.f, -13.5f, 0.f, 977.25f, 51000.f})
        {
            const auto field=BuildWaveField(time,1.f,counter,scale,camera,camera*2.f,true,true);
            const float relativeX=4213.f-camera, relativeY=-7781.f-camera*2.f;
            Require(distance(coordinate(field,relativeX,relativeY),
                              coordinate(reference,4213.f,-7781.f))<2e-2f,
                    "the same terrain point lands on the same wave lattice from any camera");
            const auto warpAtCamera=warpCoordinate(field,relativeX,relativeY);
            const auto warpAtOrigin=warpCoordinate(reference,4213.f,-7781.f);
            Require(distance(warpAtCamera[0],warpAtOrigin[0])<2e-2f&&
                    distance(warpAtCamera[1],warpAtOrigin[1])<2e-2f,
                    "the randomized swell warp stays world-anchored across camera movement");
            const float cellX=relativeX*field.lattice[0]+relativeY*field.lattice[1];
            const float cellY=relativeX*field.lattice[2]+relativeY*field.lattice[3];
            const float worldCellX=4213.f*reference.lattice[0]+(-7781.f)*reference.lattice[1];
            const float worldCellY=4213.f*reference.lattice[2]+(-7781.f)*reference.lattice[3];
            const auto warped=WarpWavePlane(field,cellX,cellY);
            const auto warpedReference=WarpWavePlane(reference,worldCellX,worldCellY);
            Require(std::fabs((warped.plane.x-cellX)-(warpedReference.plane.x-worldCellX))<2e-2f&&
                    std::fabs((warped.plane.y-cellY)-(warpedReference.plane.y-worldCellY))<2e-2f,
                    "the same world point receives the same smooth random crest displacement");
            // The foam lace's fleck, anchored like the warp above. It used to read the fine
            // chop octave, whose counter-phased coordinate moves with the camera at a fixed
            // world point (measured mean value change .22 across these same cameras), so the
            // flecks crawled over shoal foam as hash. The integer-matrix sample must hold
            // still to phase precision, and its footprint fade must be full where a fleck cell
            // spans five pixels and gone where it spans two.
            Require(std::fabs(FoamLaceFineValue(field,cellX,cellY,.37f)-
                              FoamLaceFineValue(reference,worldCellX,worldCellY,.37f))<.06f,
                    "the foam lace fleck stays on the same world point from any camera");
            Require(FoamLaceFilter(0.f,0.f)==1.f&&FoamLaceFilter(.08f,.03f)==1.f&&
                    FoamLaceFilter(.12f,.05f)>.5f&&FoamLaceFilter(.12f,.05f)<1.f&&
                    FoamLaceFilter(.21f,.1f)==0.f,
                    "the lace fleck fades out between five and two pixels per fleck cell");
            Require(std::fabs(field.ripplePhase[0])<=kLatticeWrap&&
                    std::fabs(field.ripplePhase[1])<=kLatticeWrap&&
                    std::fabs(field.warpPhase[0])<=kLatticeWrap&&
                    std::fabs(field.warpPhase[1])<=kLatticeWrap&&
                    std::fabs(field.groupPhase[0])<=kLatticeWrap&&
                    std::fabs(field.groupPhase[1])<=kLatticeWrap,
                    "chop, domain-warp and grouping phases stay inside their own noise wraps");
            for (int train=0;train<kTrainCount;++train)
                Require(field.swellPhase[train]>=0.f&&field.swellPhase[train]<kTrainCells[train],
                        "each swell train wraps its phase into its own wavelength");
            Require(std::isfinite(field.lattice[0])&&std::isfinite(field.ripplePhase[0])&&
                    std::isfinite(field.swellPhase[2])&&std::isfinite(field.uvDrift[0])&&
                    std::isfinite(field.uvDrift[1]),
                    "wave constants stay finite after hours of animation");
        }
    }
    {
        const auto still=BuildWaveField(12.0,1.f,.32f,100.f,0.f,0.f,false,false);
        Require(still.uvDrift[0]==0.f&&still.uvDrift[1]==0.f,
                "switching waves off also stops the texture creep");
        const auto moving=BuildWaveField(12.0,1.f,.32f,100.f,0.f,0.f,false,true);
        Require(moving.ripplePhase[0]!=still.ripplePhase[0], "flow animation drives the wave field");
        const float cell=kRippleCellYards*100.f*.01f;
        Require(std::fabs(std::hypot(moving.lattice[0],moving.lattice[1])-1.f/cell)<1e-5f,
                "the lattice frequency is one cell of the configured size");
        Require(std::fabs(moving.lattice[0]*moving.lattice[2]+
                          moving.lattice[1]*moving.lattice[3])<1e-5f,
                "the two lattice axes stay perpendicular");
        Require(std::fabs(std::atan2(moving.lattice[1],moving.lattice[0])-kLatticeYaw)<1e-5f,
                "the lattice keeps the one orientation the chop drifts along");
    }
    // ---- the pace, and the rotation that used to steal it --------------------------
    // Two statements a screenshot cannot hold a shader to, and the two that made the
    // shipped sea read as boiling:
    //
    //  * the lattice orientation used to be `time * kLatticeRotation`. Rotating a basis
    //    about the WORLD ORIGIN translates every point that is not at the origin, so a
    //    camera six hundred yards out watched the sea sweep past at hundreds of yards a
    //    second while a camera at the origin saw nothing wrong at all. The orientation is
    //    a constant now, so what is tested is that it stays one.
    //  * how fast a crest travels is a property of its wavelength: deep-water dispersion,
    //    scaled to a pace that reads as water. The long swell is slow and heavy, the short
    //    chop quicker, and neither is a blur.
    for (float scale : {25.f,100.f,400.f})
    for (float counter : {0.f,1.f})
    {
        const auto early=BuildWaveField(0.0,1.f,counter,scale,4213.f,-7781.f,true,true);
        const auto late=BuildWaveField(3600.0,1.f,counter,scale,4213.f,-7781.f,true,true);
        Require(early.lattice[0]==late.lattice[0]&&early.lattice[1]==late.lattice[1]&&
                early.lattice[2]==late.lattice[2]&&early.lattice[3]==late.lattice[3],
                "the wave lattice keeps one orientation: nothing spins about the world origin");
    }
    {
        const float swellSpeed=PhaseSpeedYards(kTrainCells[0]*kRippleCellYards);   // forty yards
        const float chopSpeed=PhaseSpeedYards(kTrainCells[2]*kRippleCellYards);     // ten yards
        Require(swellSpeed>2.f&&swellSpeed<6.f,
                "the long swell crosses at a pace that reads as water, not as a boil");
        Require(chopSpeed<swellSpeed,"shorter waves travel slower, as deep-water dispersion says");
        Require(swellSpeed<.2f*kTrainCells[0]*kRippleCellYards,
                "one second of travel is a fraction of the wavelength it belongs to");
        // A bigger wave size means longer waves, and longer waves travel faster: the knob
        // changes the sea rather than the frame rate of the same sea.
        Require(PhaseSpeedYards(kTrainCells[0]*kRippleCellYards*4.f)>swellSpeed,
                "a larger wave size carries a faster, longer swell");
        // Weather puts a cross-current on the trains that run across it. The dominant train
        // runs with the wind, so it is left alone: a squall chops the sea, it does not drag
        // the whole swell sideways.
        const auto sheared=BuildWaveField(60.0,1.f,1.f,100.f,0.f,0.f,true,true);
        const auto calm=BuildWaveField(60.0,1.f,0.f,100.f,0.f,0.f,true,true);
        Require(std::fabs(sheared.swellPhase[0]-calm.swellPhase[0])<1e-3f,
                "weather chops across the swell instead of dragging the long train with it");
        Require(sheared.swellPhase[1]!=calm.swellPhase[1]&&sheared.ripplePhase[1]!=calm.ripplePhase[1],
                "weather does put a cross-current on the water running across it");
    }
    // ---- the anchor: the camera the draw itself carries ----------------------------
    // A world-anchored wave is two halves that have to agree: the camera-relative
    // coordinate the vertex stage computes from modelView, and the global camera position
    // folded into the phase. The phase must come from this draw's live VIEW transform, not
    // a frame capture that may be stale or refer to the player's camera during reflections.
    // A translated tile makes modelView's extracted camera local, so the helper below also
    // tests that global view coordinates survive distinct tile origins.
    //
    // The array is the client's row-vector matrix: world axis i arrives in view space as
    // row i, and the camera is -dot(row i, translation). Reading the components ACROSS the
    // rows gives the view axes in world coordinates instead, and dotting those with a
    // view-space position applies the camera's rotation twice - which is why the field used
    // to turn when the camera turned.
    auto viewMatrix=[](float yawDegrees,float pitchDegrees,const float camera[3],float m[4][4]) {
        const float deg=3.14159265358979f/180.f;
        const float cy=std::cos(yawDegrees*deg),sy=std::sin(yawDegrees*deg);
        const float cp=std::cos(pitchDegrees*deg),sp=std::sin(pitchDegrees*deg);
        const float forward[3]={sy*cp,cy*cp,sp},right[3]={cy,-sy,0.f},up[3]={-sy*sp,-cy*sp,cp};
        // Row j is world axis j in view space, so the row holds that axis' component of
        // each view axis: right, up, forward.
        for (int j=0;j<3;++j) { m[j][0]=right[j]; m[j][1]=up[j]; m[j][2]=forward[j]; m[j][3]=0.f; }
        for (int i=0;i<3;++i)
            m[3][i]=-(camera[0]*m[0][i]+camera[1]*m[1][i]+camera[2]*m[2][i]);
        m[3][3]=1.f;
    };
    for (float yaw : {0.f,37.f,90.f,-144.f,271.f})
    for (float pitch : {-32.f,-8.f,0.f,17.f})
    for (const float cameraZ : {12.f,180.f})
    {
        const float camera[3]={4213.f,-7781.f,cameraZ};
        float m[4][4];
        viewMatrix(yaw,pitch,camera,m);
        const auto anchor=AnchorFromModelView(m);
        Require(anchor.valid,"a real liquid draw's matrix is accepted as an anchor");
        Require(std::fabs(anchor.camera[0]-camera[0])<2e-2f&&std::fabs(anchor.camera[1]-camera[1])<2e-2f,
                "the camera read out of a draw is the camera the draw was rendered from");
        // A water tile can carry a translation in WORLD while D3DTS_VIEW remains global.
        // Extracting camera from VIEW*WORLD makes it tile-local and makes foam restart at
        // every mesh origin. AnchorForWaterDraw must use the live VIEW camera when bases match.
        const float tile[3]={-937.f,2145.f,0.f};
        float tileModelView[4][4];
        std::memcpy(tileModelView,m,sizeof(tileModelView));
        for(int component=0;component<3;++component)
            tileModelView[3][component]=m[3][component]+tile[0]*m[0][component]
                +tile[1]*m[1][component]+tile[2]*m[2][component];
        const auto tileAnchor=AnchorForWaterDraw(tileModelView,m);
        Require(tileAnchor.valid&&tileAnchor.worldAligned&&
                std::fabs(tileAnchor.camera[0]-camera[0])<2e-2f&&
                std::fabs(tileAnchor.camera[1]-camera[1])<2e-2f,
                "translated water tiles share the live global camera anchor instead of a tile-local phase");
        const float localPoint[3]={4640.f-tile[0],-7405.f-tile[1],0.f};
        float tileView[3]={tileModelView[3][0],tileModelView[3][1],tileModelView[3][2]};
        for(int i=0;i<3;++i)
            for(int j=0;j<3;++j)tileView[i]+=localPoint[j]*tileModelView[j][i];
        const float tileRelX=tileModelView[0][0]*tileView[0]+tileModelView[0][1]*tileView[1]+tileModelView[0][2]*tileView[2];
        const float tileRelY=tileModelView[1][0]*tileView[0]+tileModelView[1][1]*tileView[1]+tileModelView[1][2]*tileView[2];
        Require(std::fabs(tileRelX+tileAnchor.camera[0]-4640.f)<3e-2f&&
                std::fabs(tileRelY+tileAnchor.camera[1]+7405.f)<3e-2f,
                "the same world point has the same foam phase across translated liquid tiles");
        // The pairing itself: the coordinate the vertex stage computes from this matrix,
        // plus the camera the CPU read out of the SAME matrix, is the world position of the
        // point - at every yaw and pitch, which is exactly what the transposed reading
        // could not do.
        // The point on the camera, and two more out where a tile is actually drawn.
        static const float kPoints[3][3]={{4213.f,-7781.f,0.f},{4640.f,-7405.f,0.f},{3866.f,-8233.f,0.f}};
        for (const auto& world : kPoints)
        {
            const float offset[3]={world[0]-camera[0],world[1]-camera[1],world[2]-camera[2]};
            float view[3]={0.f,0.f,0.f};
            for (int i=0;i<3;++i)
                for (int j=0;j<3;++j) view[i]+=offset[j]*m[j][i];   // the stage's own p
            const float relX=m[0][0]*view[0]+m[0][1]*view[1]+m[0][2]*view[2];   // dot(axisX,p)
            const float relY=m[1][0]*view[0]+m[1][1]*view[1]+m[1][2]*view[2];   // dot(axisY,p)
            Require(std::fabs(relX+anchor.camera[0]-world[0])<2e-2f&&
                    std::fabs(relY+anchor.camera[1]-world[1])<2e-2f,
                    "the drawn camera-relative coordinate plus the drawn camera is the world point");
        }
        // And the up axis the shading is handed has to be world up. Taken across the rows it
        // is the direction the camera looks, which makes the sky gradient, the sun elevation
        // and the wind term turn with the camera instead of staying with the world.
        float worldUp[3]={0.f,0.f,0.f};
        for (int j=0;j<3;++j)
            for (int i=0;i<3;++i) worldUp[j]+=anchor.up[i]*m[j][i];
        Require(std::fabs(worldUp[0])<1e-4f&&std::fabs(worldUp[1])<1e-4f&&std::fabs(worldUp[2]-1.f)<1e-4f,
                "the up axis the water is shaded against is world up, not the view direction");
    }
    {
        // A mirrored camera (the client's own water reflection pass) is still a rigid basis,
        // so it is still an anchor: the reflected sea has to be anchored to the same world
        // as the one it mirrors, or the two disagree at the shoreline.
        const float camera[3]={900.f,-120.f,60.f};
        float m[4][4];
        viewMatrix(48.f,-11.f,camera,m);
        m[1][0]=-m[1][0];m[1][1]=-m[1][1];m[1][2]=-m[1][2];   // flip the view's up axis
        for (int i=0;i<3;++i)
            m[3][i]=-(camera[0]*m[0][i]+camera[1]*m[1][i]+camera[2]*m[2][i]);
        const auto mirrored=AnchorFromModelView(m);
        Require(mirrored.valid&&std::fabs(mirrored.camera[0]-camera[0])<2e-2f,
                "a mirrored reflection camera is anchored to the same world it reflects");
    }
    {
        float zero[4][4]={};
        Require(!AnchorFromModelView(zero).valid,"an unfilled constant register is not an anchor");
        float scaled[4][4]={{10.f,0.f,0.f,0.f},{0.f,10.f,0.f,0.f},{0.f,0.f,10.f,0.f},{0.f,0.f,0.f,1.f}};
        Require(!AnchorFromModelView(scaled).valid,"a scaled model matrix is not an anchor");
        float sheared[4][4]={{1.f,.6f,0.f,0.f},{0.f,1.f,0.f,0.f},{0.f,0.f,1.f,0.f},{0.f,0.f,0.f,1.f}};
        Require(!AnchorFromModelView(sheared).valid,"a sheared basis is not an anchor");
        float broken[4][4]={{1.f,0.f,0.f,0.f},{0.f,1.f,0.f,0.f},{0.f,0.f,1.f,0.f},{NAN,0.f,0.f,1.f}};
        Require(!AnchorFromModelView(broken).valid,"a NaN in the draw's matrix cannot reach the wave field");
        // And the fallback the caller takes instead: a frame without a usable matrix builds
        // the field a camera at the origin would see, which is still animated and bounded.
        const auto blind=BuildWaveField(9.0,1.f,.32f,100.f,12000.f,9000.f,false,true);
        const auto origin=BuildWaveField(9.0,1.f,.32f,100.f,0.f,0.f,true,true);
        Require(blind.ripplePhase[0]==origin.ripplePhase[0]&&
                blind.swellPhase[2]==origin.swellPhase[2],
                "a missing camera falls back to the origin-anchored field, not to garbage");
        Require(std::isfinite(blind.lattice[0])&&std::isfinite(blind.swellPhase[1]),
                "the anchor fallback stays usable by both shader stages");
    }
    // The vertex stage displaces the very waves the pixel stage lights, from one shared set
    // of numbers: a phase per train, in cells along that train's own direction. Sharing them
    // is only legal because each phase is reduced into its OWN wavelength. Wrapping a
    // directional train's phase with the lattice's 64-cell wrap instead would slide it by
    // k*64*direction.x and put the lit crest on a different wave than the displaced one -
    // the artefact class this whole field exists to remove.
    // Eight trains, not three. Three sinusoids in commensurate wavelengths are a comb, and a
    // comb is what "lines of waves going and going" is: forty yard crests at a fixed spacing,
    // repeating forever within the few hundred yards a player can see. Eight wavelengths that
    // are NOT octaves of each other, in eight directions, each starting at an irregular point of
    // its own, interfere into a surface that does not repeat inside anything a player can see.
    // The band now runs down to a five yard wave, so the short trains cross the long ones at
    // the scale the chop lives at instead of leaving six crest families to regroup into lines.
    // Two of the eight are displaced and forwarded to shading; six are evaluated only there.
    // A third - the twenty yard mid wave - joins them at WATER QUALITY 8 and above, where the
    // mesh is allowed to spend the vertices on it; the two-train count stays the DEFAULT the
    // ladder ships at, which is why both numbers are pinned here and the ceiling is a separate
    // constant rather than a change to this one.
    static_assert(kTrainCount==8,"the swell is eight trains: two the mesh can hold, six for the shading");
    static_assert(kGeometryTrainCount==2,"the vertex stage carries the two longest trains only");
    static_assert(kGeometryTrainCountMax==3&&kGeometryTrainCount<kGeometryTrainCountMax,
        "the mesh can be handed one more train, the mid wave, and does not ship with it");
    Require(MeshNormalization(kGeometryTrainCount)==1.f,
        "the shipped two-train mesh is not renormalized, so WAVE HEIGHT is the height it always was");
    Require(MeshNormalization(kGeometryTrainCountMax)<1.f&&MeshNormalization(kGeometryTrainCountMax)>.7f,
        "the three-train mesh scales its lift and slide down together, so it adds shape and not size");
    for (int train=0;train<kTrainCount;++train)
        Require(std::fabs(kTrainRadians[train]*kTrainCells[train]-kTwoPi)<1e-4f&&
                std::fabs(kTrainDirX[train]*kTrainDirX[train]+kTrainDirY[train]*kTrainDirY[train]-1.f)<1e-4f,
                "every swell train is 2pi over its own cell count, in a unit direction");
    // Three of these tables were the bug. Wavelengths that are octaves of each other realign
    // into one comb every few crests; directions within a few degrees of each other (or of the
    // exact opposite, which is the same pair of crests) do the same; and six crests crossing
    // lattice zero at once read as a line drawn across the sea. So: strictly decreasing,
    // deliberately non-doubling wavelengths, no two trains parallel or anti-parallel, weights
    // that fall away, and a start offset that is inside its own wavelength and shared by
    // nothing else.
    for (int train=1;train<kTrainCount;++train) {
        Require(kTrainCells[train]<kTrainCells[train-1],"the swell table runs long to short");
        Require(kTrainWeight[train]<kTrainWeight[train-1],"each shorter train carries less of the height");
        const double ratio=double(kTrainCells[train-1])/double(kTrainCells[train]);
        Require(std::fabs(ratio-2.f)>.15f&&std::fabs(ratio-1.f)>.15f,
                "no two neighbouring trains are an octave or a unison apart, so crests do not realign");
        Require(kTrainPhaseCells[train]>0.f&&kTrainPhaseCells[train]<kTrainCells[train],
                "every train but the first starts partway along its own wavelength");
    }
    for (int a=0;a<kTrainCount;++a)
    for (int b=a+1;b<kTrainCount;++b) {
        const double across=std::fabs(double(kTrainDirX[a])*kTrainDirY[b]-double(kTrainDirY[a])*kTrainDirX[b]);
        Require(across>.12,"no two swell trains run parallel, which is a comb with two teeth");
        const double apart=std::fabs(kTrainCells[a]-kTrainCells[b]);
        Require(apart>.2f,"no two swell trains share a wavelength");
    }
    Require(kMaxPinch>1.65f&&kMaxPinch<1.85f,"the eight-train pinch budget is the one the shading clamp assumes");
    Require(kGeometryPinch>kTrainWeight[0]*kTrainRadians[0]&&kGeometryPinch<kMaxPinch,
            "the mesh folds against the two trains it displaces, not the eight it shades");
    // The mid wave the highest quality levels also displace: its pinch, after the
    // renormalization that keeps the peak lift authored, is still far inside the eight-train
    // budget the shading clamp assumes, and the renormalization makes the fold budget SMALLER
    // than the two-train one times its extra weight would suggest. Both are pinned because a
    // level that lifted a third train without rescaling it would be the one place a slider
    // could change how tall the authored waves are.
    Require(kMeshPinchMax>kGeometryPinch&&kMeshPinchMax<kMaxPinch,
            "the three-train mesh folds against the trains it displaces, and stays inside the shading budget");
    Require(GeometryPinch(kGeometryTrainCountMax)*MeshNormalization(kGeometryTrainCountMax)<kMeshPinchMax+1e-6f,
            "the three-train pinch is the renormalized one the ceiling is written against");
    // The envelope multiplies the slide as well as the lift, so the worst Jacobian the mesh
    // can be handed is the shipped steepness at the TOP of a set, not at its average. Written
    // against kGeometryPinch (not kMaxPinch) because the four short trains never move a
    // triangle; 1 - that is how far the shading's .3 floor can be trusted to stay honest.
    Require(kGerstnerSigmaMax*kGeometryPinch*kGroupMax<1.f,"the Gerstner mesh cannot fold at the top of a set");
    // ...and the same bound for the three-train mesh a high level asks the vertex stage to
    // displace, which is the case the shipped two-train number cannot speak for.
    Require(kGerstnerSigmaMax*kMeshPinchMax*kGroupMax<1.f,
            "the Gerstner mesh cannot fold at the top of a set with the mid wave displaced too");
    Require(kGroupBase>0.f&&kGroupBase+kGroupDepth<=1.15f,"grouping only ever dims a patch, never doubles it");
    {
        // The optimized shader sends the two long trains' sin/cos pairs from the
        // vertices and normalizes each interpolated pair in the pixel stage. Quantify
        // that approximation on the client's roughly four-yard liquid grid, including
        // the production domain warp: it must remain much closer to the analytic field
        // than directly interpolating height/slope would be. Four outputs are height,
        // x/y slope and Gerstner pinch, exactly the quantities shading reconstructs.
        const WaveField fields[]={
            BuildWaveField(3.25,1.f,.32f,100.f,4213.f,-7781.f,true,true),
            BuildWaveField(117.0,.65f,.8f,100.f,-921.f,1537.f,true,true),
            BuildWaveField(901.0,1.4f,.05f,100.f,11800.f,7600.f,true,true)};
        const auto trigAt=[](const WaveField& field,float x,float y) {
            const auto warped=WarpWavePlane(field,x,y);
            std::array<float,4> trig{};
            for(int train=0;train<kGeometryTrainCount;++train) {
                const float theta=SwellTheta(train,warped.plane.x,warped.plane.y,field.swellPhase);
                trig[train*2]=std::sin(theta);trig[train*2+1]=std::cos(theta);
            }
            return trig;
        };
        const auto assemble=[](std::array<float,4> trig) {
            std::array<float,4> out{};
            for(int train=0;train<kGeometryTrainCount;++train) {
                const float length=std::sqrt(trig[train*2]*trig[train*2]
                    +trig[train*2+1]*trig[train*2+1]);
                const float sine=trig[train*2]/std::max(length,.01f);
                const float cosine=trig[train*2+1]/std::max(length,.01f);
                out[0]+=kTrainWeight[train]*sine;
                out[1]+=kTrainWeight[train]*kTrainRadians[train]*cosine*kTrainDirX[train];
                out[2]+=kTrainWeight[train]*kTrainRadians[train]*cosine*kTrainDirY[train];
                out[3]+=kTrainWeight[train]*kTrainRadians[train]*sine;
            }
            return out;
        };
        constexpr float meshPitch=4.f/kRippleCellYards;
        const std::array<std::array<float,2>,6> probes={{{.1f,.1f},{.25f,.2f},{.6f,.1f},
            {.9f,.8f},{.4f,.8f},{.7f,.6f}}};
        std::array<float,4> worst{};std::array<double,4> squared{};size_t sampleCount=0;
        for(const auto& field:fields)for(int gy=-20;gy<20;++gy)for(int gx=-20;gx<20;++gx)
        for(const auto& probe:probes) {
            const float u=probe[0],v=probe[1];
            std::array<std::array<float,2>,3> vertices{};float bary[3]{};
            if(u+v<=1.f) {
                vertices={{{0,0},{1,0},{0,1}}};bary[0]=1-u-v;bary[1]=u;bary[2]=v;
            } else {
                vertices={{{1,1},{0,1},{1,0}}};bary[0]=u+v-1;bary[1]=1-u;bary[2]=1-v;
            }
            std::array<float,4> interpolatedTrig{};
            for(int corner=0;corner<3;++corner) {
                const auto trig=trigAt(field,(gx+vertices[corner][0])*meshPitch,
                    (gy+vertices[corner][1])*meshPitch);
                for(int term=0;term<4;++term)interpolatedTrig[term]+=bary[corner]*trig[term];
            }
            const auto reconstructed=assemble(interpolatedTrig);
            const auto exact=assemble(trigAt(field,(gx+u)*meshPitch,(gy+v)*meshPitch));
            for(int term=0;term<4;++term) {
                const float error=std::fabs(reconstructed[term]-exact[term]);
                worst[term]=std::max(worst[term],error);squared[term]+=double(error)*error;
            }
            ++sampleCount;
        }
        Require(worst[0]<.03f&&worst[1]<.015f&&worst[2]<.008f&&worst[3]<.015f&&
                std::sqrt(squared[0]/sampleCount)<.008&&std::sqrt(squared[1]/sampleCount)<.004&&
                std::sqrt(squared[2]/sampleCount)<.003&&std::sqrt(squared[3]/sampleCount)<.0045,
                "normalized interpolated long waves preserve the analytic field on the liquid grid");
    }
    {
        // "Lines of waves going and going", measured instead of argued about. Walk along
        // where the dominant train's crest would run if it were alone (the lattice y axis
        // at a fixed phase of train A): a lone sine is exactly constant there, and a
        // comb of a few trains is nearly so. A band of eight warped, incommensurate
        // trains has to vary along its own crest by a real fraction of how much it varies
        // everywhere - that variation is crests growing, doubling and dying instead of
        // marching. Sampled on the CPU mirror the preview renders from, so the number
        // describes the same field the pixel stage shades.
        const auto field=BuildWaveField(3.25,1.f,.32f,100.f,4213.f,-7781.f,true,true);
        auto heightAt=[&](float cellX,float cellY) {
            const auto warped=WarpWavePlane(field,cellX,cellY);
            return EvaluateSwell(warped.plane.x,warped.plane.y,field.swellPhase).height;
        };
        double crestMean=0,crestSquare=0,allMean=0,allSquare=0;int crestCount=0,allCount=0;
        for (int i=0;i<400;++i) {
            const float along=-60.f+i*.3f;
            const float crest=heightAt(2.5f,along);
            crestMean+=crest;crestSquare+=double(crest)*crest;++crestCount;
            const float everywhere=heightAt(-60.f+float(i%20)*6.f,-60.f+float(i/20)*6.f);
            allMean+=everywhere;allSquare+=double(everywhere)*everywhere;++allCount;
        }
        crestMean/=crestCount;allMean/=allCount;
        const double crestSpread=std::sqrt(std::max(crestSquare/crestCount-crestMean*crestMean,0.0));
        const double allSpread=std::sqrt(std::max(allSquare/allCount-allMean*allMean,0.0));
        Require(allSpread>.2&&crestSpread>.3*allSpread,
                "the surface varies along its own crest line, so crests break instead of marching in lines");
    }
    auto trainAngle=[](const WaveField& field,int train,float relativeX,float relativeY) {
        const float cellX=relativeX*field.lattice[0]+relativeY*field.lattice[1];
        const float cellY=relativeX*field.lattice[2]+relativeY*field.lattice[3];
        const auto warped=WarpWavePlane(field,cellX,cellY);
        return kTrainRadians[train]*(kTrainDirX[train]*warped.plane.x+kTrainDirY[train]*warped.plane.y
                                     +field.swellPhase[train]);
    };
    {
        const auto field=BuildWaveField(0.0,1.f,.32f,100.f,4213.f,-7781.f,true,true);
        constexpr float x=7.31f,y=-4.27f,step=.002f;
        const auto scalarWarp=[&](float px,float py) {
            const auto sample=WarpWavePlane(field,px,py);
            return (sample.plane.x-px)/(kWaveWarpDirectionX*kWaveWarpAmplitude);
        };
        const auto center=WarpWavePlane(field,x,y);
        const float dx=(scalarWarp(x+step,y)-scalarWarp(x-step,y))/(2.f*step);
        const float dy=(scalarWarp(x,y+step)-scalarWarp(x,y-step))/(2.f*step);
        Require(std::fabs(dx-center.gradientX)<2e-3f&&std::fabs(dy-center.gradientY)<2e-3f,
                "the CPU domain-warp gradient matches finite differences of its displacement");
    }
    const auto turn=[](float value){return value-kTwoPi*std::floor(value/kTwoPi);};
    // (1) Translation invariance, measured where a tile is actually visible: the reference
    // camera sits on the water it is testing, and each other camera sees the same point from
    // up to six hundred yards away.
    for (float offset : {-600.f, -133.f, 0.f, 271.f, 640.f})
    for (float counter : {0.f, .32f, 1.f})
    for (int train=0;train<kTrainCount;++train)
    {
        const auto field=BuildWaveField(17.5,1.f,counter,100.f,4213.f+offset,-7781.f+offset*.5f,true,true);
        const auto reference=BuildWaveField(17.5,1.f,counter,100.f,4213.f,-7781.f,true,true);
        const float relativeX=4213.f-(4213.f+offset), relativeY=-7781.f-(-7781.f+offset*.5f);
        // sin() is what both stages feed, and arguments that differ by a whole number of
        // turns are the same wave. Both sides are wrapped with floor (NOT by taking an
        // absolute value first: |a| and |b| are not congruent when the signs differ, which
        // is how an earlier version of this check invented a failure), and compared circularly.
        const float a=turn(trainAngle(field,train,relativeX,relativeY));
        const float b=turn(trainAngle(reference,train,0.f,0.f));
        const float apart=std::fabs(a-b);
        Require(std::fmin(apart,kTwoPi-apart)<1e-2f,
                "every swell train lands the same wave under every camera");
    }
    // (2) And the argument each stage evaluates stays inside a range where float32 still has
    // fraction bits to spare even at the far corner of a continent, because the camera's own
    // coordinate is subtracted first and the phase is wrapped into one wavelength.
    {
        const auto far=BuildWaveField(41.25,1.f,.32f,100.f,46321.f,-51000.f,true,true);
        // Both stages evaluate sincos of these angles in float32. What has to stay small is
        // not the angle - a point far along a train is many turns in - but the coordinate it
        // is built from: the camera's own position comes off before anything is multiplied,
        // so what reaches sincos is the offset of a drawn tile and not of a continent. Below
        // 2^12 radians float32 still resolves a turn to better than a thousandth of a
        // radian, which is a thousandth of a yard of crest at the shipped wave height.
        const float cellX=421.f*far.lattice[0]-307.f*far.lattice[1];
        const float cellY=421.f*far.lattice[2]-307.f*far.lattice[3];
        Require(std::fabs(cellX)<400.f&&std::fabs(cellY)<400.f,
                "the lattice coordinate a stage evaluates is a tile offset, not a world coordinate");
        for (int train=0;train<kTrainCount;++train)
        {
            const float angle=std::fabs(trainAngle(far,train,421.f,-307.f));
            Require(angle<4096.f,"the geometry wave argument stays inside float32's sine precision");
            Require(std::isfinite(angle)&&std::isfinite(far.swellPhase[train]),
                    "the swell phase is finite for hours of flow and any camera");
        }
        const float ripple=std::fabs(421.f*far.lattice[0]-307.f*far.lattice[1]+far.ripplePhase[0]);
        Require(ripple<=kLatticeWrap*2.f,"the shading argument never leaves the wrapped lattice twice over");
    }
    // ---- geometry waves: three dimensions, held fold-safe --------------------------
    // A Gerstner surface folds over itself when its steepness passes one, and a folded wave
    // is not a steep wave - it is a surface that has turned inside out, with normals pointing
    // into the water and foam where there is no crest. The steepness is therefore authored on
    // the CPU, clamped, and shipped as a constant, so no combination of sliders can reach it.
    for (float height : {0.f, 25.f, 100.f, 300.f, 5000.f, NAN})
    for (float scale : {25.f, 100.f, 400.f})
    for (float agitation : {.12f, 1.f, 1.45f, 2.6f, 40.f, NAN})
    {
        const auto waves=BuildGeometryWaves(height,scale,agitation,true,true);
        const float cell=kRippleCellYards*(scale>1.f?scale:100.f)*.01f;
        Require(std::isfinite(waves.amplitude)&&std::isfinite(waves.lateral)&&std::isfinite(waves.sigma),
                "no authored wave height, size or sea state produces a NaN");
        Require(waves.sigma<=kGerstnerSigmaMax&&waves.sigma*kGeometryPinch*kGroupMax<1.f,
                "the Gerstner steepness stays below the fold, whatever is authored or enveloped");
        Require(waves.amplitude<=1.2f*cell+1e-4f,
                "a wave never lifts higher than the cell it is drawn on");
        Require(waves.lateral==waves.sigma*cell||waves.lateral==0.f,
                "the slide toward a crest is the authored steepness times the cell");
        Require(std::fabs(waves.cellYards-cell)<1e-4f,"the cell length the stage is given is the wave size");
    }
    {
        // Self-similar crests: the wave SIZE changes the wavelength and the height together,
        // so the shape a player sees is the same sea at another scale rather than a stretched
        // one. Growing the height alone would grow the steepness with it, which is the fold.
        const auto small=BuildGeometryWaves(100.f,25.f,1.f,true,true);
        const auto shipped=BuildGeometryWaves(100.f,100.f,1.f,true,true);
        const auto large=BuildGeometryWaves(100.f,400.f,1.f,true,true);
        Require(small.sigma==shipped.sigma&&shipped.sigma==large.sigma,
                "the crest shape is the same at every wave size");
        Require(small.amplitude<shipped.amplitude&&shipped.amplitude<large.amplitude,
                "a larger wave size is a larger wave");
        Require(shipped.amplitude>.4f&&shipped.amplitude<1.2f,
                "the shipped wave height is a body of water a player can see from the shore");
        // Per-zone and per-weather authoring still reaches the geometry: an exposed coast
        // lifts more than a sheltered pond, and a storm lifts both. This is the requirement
        // the whole feature has to keep - calm zones stay calm, and weather changes water.
        const auto exposed=BuildGeometryWaves(100.f,100.f,1.45f,true,true);
        const auto sheltered=BuildGeometryWaves(100.f,100.f,.45f,true,true);
        Require(exposed.amplitude>shipped.amplitude&&sheltered.amplitude<shipped.amplitude,
                "an authored sea state lifts and lowers the geometry waves");
        // The switches, in both directions. Waves off is flat water - not a small wave, flat -
        // because a zone authored calm has to look calm from every camera. Geometry waves off
        // keeps the shading's crest sharpening, since that is a property of the sea and not of
        // the mesh: the whitecaps stay where a crest is, they just stop being three dimensional.
        Require(BuildGeometryWaves(100.f,100.f,1.f,false,false).amplitude==0.f&&
                BuildGeometryWaves(100.f,100.f,1.f,false,false).sigma==0.f,
                "waves off is a flat, unsharpened sheet");
        const auto shadingOnly=BuildGeometryWaves(100.f,100.f,1.f,false,true);
        Require(shadingOnly.amplitude==0.f&&shadingOnly.lateral==0.f&&shadingOnly.sigma>0.f,
                "geometry waves off still sharpens the crests the shading lights");
        Require(BuildGeometryWaves(0.f,100.f,1.f,true,true).amplitude==0.f,
                "a wave height of zero is flat water even with the geometry switched on");
        // The pinch the pixel stage divides its slope by is never so large that a crest
        // sharpens into a spike: the Jacobian is floored in the shader, and the coefficient
        // that reaches it is bounded here.
        Require(kGerstnerSigma*kGeometryPinch*kGroupMax<.7f,
            "a fully pinched crest at the top of a set still leaves most of its Jacobian");
    }
    // ---- zone and weather driven sea state ----------------------------------------------
    {
        using renderer::water::SeaTuning; using renderer::water::SeaWeather;
        using renderer::water::TargetSeaState; using renderer::water::SeaStateFilter;
        using renderer::water::SeaMotionClock;
        const SeaState calm = TargetSeaState(SeaTuning{}, SeaWeather{});
        // An untuned zone under a clear sky has to be exactly the old water: every multiplier 1
        // and no shear. This is what lets the feature ship switched on.
        Require(calm.agitation == 1.f && calm.motion == 1.f && calm.shear == 0.f && calm.wind == 1.f,
                "an untuned zone in clear weather is the identity, so per-zone waves default on safely");
        const SeaTuning exposed{1.45f, 1.f, 1.f}, sheltered{.45f, 1.f, 1.f};
        const SeaWeather rain{1, .95f};
        const SeaState e = TargetSeaState(exposed, SeaWeather{}), s = TargetSeaState(sheltered, SeaWeather{});
        Require(e.agitation > 1.f && e.motion > 1.f && e.wind > 1.f
                && s.agitation < 1.f && s.motion < 1.f && s.wind < 1.f,
                "an exposed coast carries more water than a sheltered pond, in every term");
        const SeaState eStorm = TargetSeaState(exposed, rain), sStorm = TargetSeaState(sheltered, rain);
        Require(eStorm.agitation > e.agitation && sStorm.agitation > s.agitation
                && eStorm.motion > e.motion && sStorm.motion > s.motion,
                "weather roughens both, and quickens both");
        Require(eStorm.motion > e.motion * 1.5f && eStorm.motion <= 3.f,
                "a blow at least halves the time between crests, and stays inside the clock's range");
        Require(eStorm.shear > e.shear && sStorm.shear > s.shear && eStorm.shear <= 1.f,
                "weather puts a cross-chop on top of the swell instead of just scaling it");
        // A storm must not be able to make a sheltered pond rougher than an exposed coast at rest:
        // that inversion is what would make the whole idea read as a random brightness of waves.
        Require(sStorm.agitation < e.agitation,
                "a storm on still water stays calmer than an exposed coast in calm air");
        // The crisp form of "off": with no weather response, a storm is indistinguishable from a
        // clear sky in every term - not merely smaller, which is what a clamp would also satisfy.
        const SeaTuning damped{1.45f, 0.f, 1.f};
        const SeaState off = TargetSeaState(damped, rain), offClear = TargetSeaState(damped, SeaWeather{});
        Require(off.agitation == offClear.agitation && off.motion == offClear.motion
                && off.shear == offClear.shear && off.wind == offClear.wind && off.agitation == 1.45f,
                "WeatherResponsePercent=0 pins a zone to its authored waves whatever the sky does");
        const SeaTuning noWind{1.45f, 1.f, 0.f};
        Require(TargetSeaState(noWind, rain).wind == 0.f,
                "an authored WIND of zero keeps a river foamless through a storm");
        const SeaState huge = TargetSeaState(SeaTuning{1e9f, 1e9f, 1e9f}, SeaWeather{9, -5.f});
        Require(std::isfinite(huge.agitation) && huge.agitation <= 2.6f && huge.motion <= 3.f
                && huge.shear >= 0.f && huge.shear <= 1.f && huge.wind >= 0.f && huge.wind <= 2.5f,
                "absurd authored values stay finite and inside the shader's range");
        const SeaState nan = TargetSeaState(SeaTuning{NAN, NAN, NAN}, SeaWeather{1, NAN});
        Require(std::isfinite(nan.agitation) && std::isfinite(nan.motion) && std::isfinite(nan.wind),
                "a NaN tuning value cannot reach the shader");

        SeaStateFilter filter;
        const float build = 14.f;
        const SeaState afterOneStep = filter.Observe(exposed, SeaWeather{}, build, build);
        const float covered = 1.f - std::abs(afterOneStep.agitation - e.agitation) / std::abs(e.agitation - 1.f);
        Require(covered > .949f && covered < 1.001f,
                "one time constant of easing covers 95% of the step, whatever the frame rate");
        SeaStateFilter fine;
        float per = 1.f;
        for (int i = 0; i < 100; ++i) per = fine.Observe(exposed, SeaWeather{}, build / 100.f, build).agitation;
        Require(std::abs(per - afterOneStep.agitation) < .01f * std::abs(e.agitation - 1.f),
                "the same transition at 100 fps lands within a hundredth of a step");
        SeaStateFilter down;
        for (int i = 0; i < 100; ++i) down.Observe(exposed, SeaWeather{}, build / 100.f, build);
        const float clearing = down.Observe(SeaTuning{}, SeaWeather{}, build / 100.f, build).agitation;
        Require(clearing > e.agitation - .06f * (e.agitation - 1.f),
                "clearing drains slower than a squall arrives, so open water does not snap flat");
        Require(std::isfinite(down.Observe(exposed, SeaWeather{}, -1.f, build).agitation)
                && std::isfinite(down.Observe(exposed, SeaWeather{}, 1e6f, build).agitation),
                "a stalled or backwards frame cannot break the eased state");

        SeaMotionClock clock;
        for (int i = 0; i < 60; ++i) clock.Observe(1.f / 60.f, 1.f);
        const double beforeChange = clock.Time();
        Require(std::abs(beforeChange - 1.0) < 1e-5,
                "the wave clock integrates one second of calm to a float's precision");
        for (int i = 0; i < 60; ++i) clock.Observe(1.f / 60.f, 2.f);
        Require(clock.Time() > beforeChange * 2.5, "doubling the speed doubles future travel only");
        const double held = clock.Time();
        clock.Observe(1e9f, 1.f); clock.Observe(NAN, 1.f); clock.Observe(.01f, NAN);
        Require(clock.Time() >= held && clock.Time() < held + .2f,
                "a loading screen or a NaN cannot teleport the field forward");
    }
    {
        const std::string shader = ReadAlongside("../WaterEffect.h");
        Require(bool(shader.size() > 4096), "production water shader source is readable");
        // The in-scatter lobe must stay the normalized Henyey-Greenstein one. An earlier shape,
        // (1-g^2)/den^2 with no normalization, put a nine-times multiplier on water facing the sun
        // and washed a whole sea pale; it was cut for instruction slots and the look went with it.
        Require(shader.find("absorption*(1-g*g)*.17/(phaseDen*sqrt(phaseDen))") != std::string::npos,
                "in-scatter keeps the normalized HG lobe, not a bare squared denominator");
        // The glint keeps the compensation for its own width, and the output gain keeps the half
        // of the fold that belongs there: both halves together are one normalization, not two.
        Require(shader.find("pow(NoH,sharpness)*(sharpness+2.0)*fresnel") != std::string::npos,
                "sun glint is compensated for its own lobe width");
        Require(shader.find("celestialPath*controls.w*.0992") != std::string::npos,
                "the glint's output gain does not double-count the normalization");
        // The per-body tint is a multiply whose identity is 0x808080, because the CPU uploads
        // the colour bytes divided by 128. Any gain applied to it in the shader is a gain applied
        // to every zone in the game, and it is exactly what an already-bright scene cannot
        // survive: the device sweep samples a captured 128 gray through the whole shader and
        // requires 128 back, and a `*2.0` here turned that into 255.
        Require(shader.find("waterBody*=waterTint.rgb;") != std::string::npos,
                "the per-body water tint stays a neutral-at-0x808080 multiply");
        // The look normalizations and the sea state are only trustworthy while the shader and the
        // previewer keep deriving their numbers from the same place.
        Require(shader.find("seaFilter.Observe(tuning,seaWeather,dt,weatherEaseSeconds)") != std::string::npos
                && shader.find("watereffect::FeedWeather") == std::string::npos,
                "water is eased once per frame and is fed its weather rather than reading the client");
        Require(shader.find("waveMotion.Advance(dt,flowSpeed*seaMotion,") != std::string::npos
                && shader.find("waveMotion.Build(anchor.camera[0],anchor.camera[1],anchor.valid,wavesEnabled)") != std::string::npos
                && shader.find("BuildWaveField(elapsed,") == std::string::npos,
                "each wave phase is integrated once, and draws do not re-multiply speed/shear into session age");

        // ---- the three defects this round exists to remove, pinned as text -------------
        // Text is the only thing a portable test can hold a production shader to, and these
        // are the lines whose spelling is the fix: a wave that follows the camera, a wave
        // that travels at a readable pace, and a wave that is a body rather than a curve.
        // Comments come off before any spelling is searched for. The vertex stage documents
        // the transposition it must NOT do by writing the wrong expression down next to the
        // right one, and a pin that matched the warning instead of the code would pass on the
        // exact bug it describes.
        const auto stripComments=[](const std::string& text) {
            std::string out;
            out.reserve(text.size());
            for (size_t at=0; at<text.size(); )
            {
                if (text.compare(at,2,"//")==0) { while (at<text.size()&&text[at]!='\n') ++at; }
                else if (text.compare(at,2,"/*")==0)
                {
                    at+=2;
                    while (at+1<text.size()&&text.compare(at,2,"*/")!=0) ++at;
                    at=at+1<text.size()?at+2:text.size();
                }
                else out.push_back(text[at++]);
            }
            return out;
        };
        const auto literal=[&](const std::string& text,const char* name) {
            const std::string head=std::string(name)+"=R\"HLSL(";
            const size_t start=text.find(head);
            if (start==std::string::npos) return std::string();
            const size_t end=text.find(")HLSL\"",start);
            return end==std::string::npos?std::string():stripComments(text.substr(start,end-start));
        };
        const std::string vertex=literal(shader,"vertexSource"), pixel=literal(shader,"source");
        Require(shader.find("renderer::water::HasFogCurve(oldVertexFog)")!=std::string::npos&&
                shader.find("const float noFog[4]={0.f,1.f,1.f,0.f};")!=std::string::npos&&
                shader.find("if(vertexFogFallback)device->SetVertexShaderConstantF(4,oldVertexFog,1);")!=std::string::npos,
                "an angle-dependent water VS with non-fog c4 cannot fog the replacement surface away, and c4 is restored on every exit");
        // The whole file with its comments off, for the pins that assert an expression is
        // NOT there: both sources explain what the lattice orientation used to be by name.
        // The header is read too, because the wave model itself - the train table, the
        // dispersion, the orientation - lives there and is spelled into both stages.
        const std::string code=stripComments(shader);
        const std::string header=stripComments(ReadAlongside("../src/Water/WaveField.h"));
        // The CPU mirror is read once, up here, because pins on both the shader text and the
        // mirror's own spelling of the same formula are interleaved through this section: a
        // formula that must be faded in the stage must be faded in the mirror too, and that is
        // one assertion rather than two in different places.
        const std::string mirror = ReadAlongside("WaterPreview.cpp");
        Require(header.size()>4096,"the wave field header both stages mirror is readable");
        Require(vertex.size()>1024&&pixel.size()>8192,"both water stages are readable as text");
        // (0) The sea's re-anchoring pivot must be THIS place's camera. WaveMotion::Advance
        // compensates every heading and WAVE SIZE edit about the pivot it is handed, which
        // keeps the field still at the player and nowhere else: a point d away from the pivot
        // shifts by dot(d, the lattice change). A teleport leaves the previous zone's draw
        // anchor behind (a loading screen draws nothing, and the new zone's first frames
        // capture no camera), so compensating about that anchor sweeps every crest across the
        // surface by the distance between the two zones - the reported "water races after a
        // teleport, then slows back to normal", because the heading eases over ~12 s. The
        // anchor is therefore only allowed to stand in for the frame camera while it is FRESH,
        // and a stale one freezes the lattice for those frames instead of re-anchoring it.
        Require(code.find("constexpr double kAnchorFreshSeconds=.25;")!=std::string::npos&&
                code.find("inline bool LastWaveAnchorFresh() noexcept {")!=std::string::npos&&
                code.find("const bool pivotValid=frame.cameraValid||anchorFresh;")!=std::string::npos&&
                code.find("waveScalePercent,frame.wind,pivotX,pivotY,pivotValid,wavesEnabled);")
                    !=std::string::npos,
                "the sea re-anchors only about a fresh, local pivot, never about the previous zone's anchor");
        Require(code.find("const bool anchorValid=cameraValid||LastWaveAnchorFresh();")!=std::string::npos&&
                code.find("lastWaveAnchorSeconds=renderer::PerformanceSeconds();")!=std::string::npos&&
                code.find("lastWaveAnchorSeconds=-1.0;")!=std::string::npos,
                "the full-screen medium's field and the Reset path obey the same freshness rule");
        // (1) The waves followed the camera. Two causes, both a matrix read the wrong way
        // round: the lattice was built from the view axes expressed in world coordinates, so
        // a camera-relative position was rotated by the camera twice and the field turned
        // when the camera turned; and the anchor was read from a camera captured on an
        // earlier draw, so the field slid by however far the player had moved since.
        Require(vertex.find("float3 axisX=modelView[0].xyz;")!=std::string::npos&&
                vertex.find("float3 axisY=modelView[1].xyz;")!=std::string::npos&&
                vertex.find("float3 axisZ=modelView[2].xyz;")!=std::string::npos,
                "the water vertex stage reads the world axes as the rows of its own matrix");
        Require(vertex.find("float3(modelView[0].z,modelView[1].z,modelView[2].z)")==std::string::npos&&
                vertex.find("float3(modelView[0].x,modelView[1].x,modelView[2].x)")==std::string::npos,
                "no wave coordinate is built by reading the draw's matrix across its rows");
        Require(vertex.find("float2 rel=float2(dot(axisX,p.xyz),dot(axisY,p.xyz));")!=std::string::npos,
                "the lattice coordinate is the world offset from this draw's own camera");
        Require(code.find("GetVertexShaderConstantF(5,nativeVertexConstants[0],31)")!=std::string::npos&&
                code.find("memcpy(modelView,nativeVertexConstants,sizeof(modelView))")!=std::string::npos&&
                code.find("GetTransform(D3DTS_VIEW,&liveView)")!=std::string::npos&&
                code.find("AnchorForWaterDraw(modelView,viewRows)")!=std::string::npos&&
                code.find("anchor.camera[0],anchor.camera[1],anchor.valid")!=std::string::npos,
                "the wave and foam phase combines this draw's camera-relative vertex constants with its live global view anchor");
        Require(code.find("waveFrame.cameraPosition.x,waveFrame.cameraPosition.y")==std::string::npos,
                "no water draw is anchored to a camera captured on some other draw");
        Require(code.find("controls[48]=anchor.up[0]")!=std::string::npos&&
                code.find("viewMatrix[2][0]")==std::string::npos,
                "the up axis the water is shaded against comes from the same draw");
        // (2) The waves were far too fast: the lattice orientation was `time * rotation`,
        // which spins the basis about the WORLD ORIGIN and translates every point that is
        // not at the origin by hundreds of yards a second. It is a constant now, and the
        // pace comes from dispersion over a wavelength instead.
        Require(code.find("kLatticeRotation")==std::string::npos&&
                header.find("kLatticeRotation")==std::string::npos&&
                header.find("inline constexpr float kLatticeYaw = 0.62f;")!=std::string::npos,
                "the wave lattice has one orientation, so nothing spins about the world origin");
        Require(header.find("inline constexpr float kPhaseSpeedScale = .44f;")!=std::string::npos&&
                header.find("kPhaseSpeedScale * std::sqrt(kGravityYards * wavelength / kTwoPi)")!=std::string::npos,
                "a crest travels at the deep-water speed of its own wavelength");
        // (3) The waves were flat: curves drawn on a plane. The lift has to go along world
        // up, and the slide toward the crest is what makes a wave a body - crests pinch and
        // steepen, troughs open out - so both are pinned, plus the sign of the shading
        // normal, which is the negative gradient of the height field the geometry lifted.
        Require(vertex.find("p.xyz+=axisZ*lift+axisX*lateral.x+axisY*lateral.y;")!=std::string::npos,
                "geometry waves lift the surface along world up and slide it toward the crest");
        Require(vertex.find("float2 warpedDirA=kDirA+6.5*dot(kDirA,float2(.6,.8))*warpGradient;")
                    !=std::string::npos&&
                vertex.find("float2 slide=waveControl.y*meshScale*gain")!=std::string::npos&&
                vertex.find("*(kWeightA*cosA*warpedDirA+kWeightB*cosB*warpedDirB+midGate*kWeightC*cosC*warpedDirC);")
                    !=std::string::npos,
                "the Gerstner slide follows the domain-warped crest with its chain-rule direction, inside the set");
        // The mid wave joins the slide through the SAME renormalization as the lift, and the
        // warp it rides is the warp the two long trains ride: one direction expression, one
        // envelope, one scale. A level that added the train to the lift only would leave a
        // crest that rises without steepening, which is the flat-curve artefact this exists for.
        Require(vertex.find("float2 warpedDirC=kDirC+6.5*dot(kDirC,float2(.6,.8))*warpGradient;")
                    !=std::string::npos&&
                vertex.find("float meshScale=(kWeightA+kWeightB)/(kWeightA+kWeightB+midGate*kWeightC);")
                    !=std::string::npos&&
                vertex.find("float longSwell=(kWeightA*sinA+kWeightB*sinB+midGate*kWeightC*sinC)*meshScale;")
                    !=std::string::npos,
                "the mid wave the mesh displaces is warped, enveloped and renormalized like the trains it joins");
        Require(pixel.find("swellSlope=(swellSlope+dot(swellSlope,float2(.6,.8))*warpGradient*6.5)*foreverStyle.x;")
                    !=std::string::npos,
                "pixel normals apply the full chain rule through the same crest warp");
        // The author's swell response is applied BEFORE the envelope: `foreverStyle.x` is
        // the WAVE/FOREGROUND slider and it belongs on the wave itself, while the envelope
        // belongs on the sea. Multiplying them in either order is the same product, but only
        // one of them survives a `swellSlope*gain+swell*dgain` product rule - differentiating
        // an envelope that already carries the slider would scale the slider by dgain too.
        Require(pixel.find("swellSlope=swellSlope*gain+swell*worldRel.zw;")!=std::string::npos&&
                pixel.find("swellSlope=(swellSlope+dot(swellSlope,float2(.6,.8))*warpGradient*6.5)*foreverStyle.x;")
                    <pixel.find("swellSlope=swellSlope*gain+swell*worldRel.zw;"),
                "the slope differentiates the envelope (product rule) after the slider, not before");
        Require(pixel.find("swell*=gain;")!=std::string::npos&&
                pixel.find("float2 slopes=((ripple.yz*.72*filter+rippleFine.yz*.30*filter*filter")
                    !=std::string::npos&&
                pixel.find("+rippleMid.yz*.40*filter)*gain+swellSlope)*waveFlow.w;")!=std::string::npos,
                "the swell height and the chop on top of it ride the same set");
        // The mid octave is a third chop band, not a fourth term in the shade: it rides the
        // swell between the base and the fine octave, and it fades with `filter` rather than
        // with its square because at half the base frequency it stays resolvable twice as far.
        // Both are pinned because a mid octave faded like the fine one disappears from the
        // middle distance, which is the only place it was added for.
        Require(pixel.find("rippleMid=noiseGradient(plane*.46+float2(-wavePhase.x,wavePhase.y)*.87+swell*float2(.40,.66));")
                    !=std::string::npos&&
                pixel.find("rippleMid.yz*.40*filter)")!=std::string::npos&&
                pixel.find("rippleMid.yz*.40*filter*filter")==std::string::npos,
                "the mid octave rides the swell and fades on the first power of the footprint filter");
        // The foam checkerboard, which took two attempts and is pinned in both directions because the
        // The player's latest capture still has the grid after both earlier changes. Level 7 is the
        // switch that adds rippleMid, and that per-pixel value was still feeding the lace. Keep the
        // mid octave remains for its wave slope; sample broad warp exactly per pixel as the second
        // foam scale, so neither a projected octave nor triangle interpolation prints a grid.
        // Also pin the old clipped thresholds out of both implementations.
        Require(pixel.find("float2 laceAbsolute=plane+wavePhase.xy;")!=std::string::npos&&
                pixel.find("float2 laceCell=float2(laceAbsolute.x*2+laceAbsolute.y,")!=std::string::npos&&
                pixel.find("-laceAbsolute.x+laceAbsolute.y*2)+swell*float2(.9,.47);")!=std::string::npos&&
                pixel.find("float laceFade=1-smoothstep(.18,.45,2.2360679*max(footprint.x,footprint.y));")!=std::string::npos&&
                pixel.find("float fine=lerp(.5,noiseGradient(laceCell).x,laceFade);")!=std::string::npos&&
                pixel.find("float2 foamWarpCoord=float2(plane.x*.8253356+plane.y*.5646425,")!=std::string::npos&&
                pixel.find("float3 foamWarp=noiseGradient(foamWarpCoord);")!=std::string::npos&&
                pixel.find("float broad=lerp(.5,foamWarp.x,filter);")!=std::string::npos&&
                pixel.find("float broad=lerp(.5,waveWarp.x,filter);")==std::string::npos&&
                pixel.find("lace=lerp(1,1-.70*(1-fine)*(1-broad),saturate(filter*2-.2));")!=std::string::npos&&
                pixel.find("rippleFine.x",pixel.find("float lace=1;"))==std::string::npos&&
                pixel.find("rippleMid.x",pixel.find("float lace=1;"))==std::string::npos&&
                pixel.find("*1.7-.35)")==std::string::npos&&
                pixel.find("*1.6-.30)")==std::string::npos&&
                mirror.find("FoamLaceFineValue(field, plane.x, plane.y, swell)")!=std::string::npos&&
                mirror.find("Mix(.5f, wave.laceFineValue, wave.laceFilter)")!=std::string::npos&&
                mirror.find("1.f - .70f * (1.f - laceFine) * (1.f - laceBroad)")!=std::string::npos&&
                mirror.find("laceMid")==std::string::npos,
                "the lace fleck is the anchored integer-matrix sample, not the counter-phased chop or mid octave");
        // The black shape inside the waves at surface height. `controls.z` is normalStrength times the
        // sea's agitation and reaches 2.5, so the body multiplier goes NEGATIVE on a face turning away
        // from the light, and nothing saturates rgb between there and the output - the render target
        // clamped it to black. Water is alpha-blended and writes no depth, so the far face of a wave
        // shows through the near one and a far face clamped to black read as a silhouette cut out of the
        // sea. Pinned as a floor on the multiplier in BOTH halves of the renderer, because a floor the
        // mirror does not carry is a floor the published frames were not tuned with.
        const auto vDotLFirst=pixel.find("float vDotL=dot(v,l);");
        Require(pixel.find("rgb*=max(1+controls.z*(1.9*(NoL-dot(n,l))+.75*(dot(wave,upWind.xyz)-dot(n,upWind.xyz))),")!=std::string::npos&&
                pixel.find("lerp(.22,.78,saturate(-vDotL*1.7-.45)));")!=std::string::npos&&
                vDotLFirst!=std::string::npos&&vDotLFirst==pixel.rfind("float vDotL=dot(v,l);"),
                "a wave face turning from the light is floored above black, and the dot that floors it is computed once");
        Require(mirror.find("Mix(.22f, .78f, Saturate(-Dot(toCamera, light) * 1.7f - .45f))")!=std::string::npos&&
                mirror.find("rgb = Mul(rgb, std::max(1.f + normalResponse")!=std::string::npos,
                "the CPU mirror floors its body shading identically");
        // Coverage is the column's own extinction in both halves. A mirror that kept the old
        // fraction would publish preview frames translucency the renderer no longer has, which
        // is exactly the kind of drift this pair of pins exists to catch: the double wave was
        // half a classification bug in the shader and half a coverage ceiling in both.
        Require(pixel.find("float depthAlpha=absorption;")!=std::string::npos&&
                mirror.find("const float depthAlpha = absorption;")!=std::string::npos&&
                mirror.find(".78f - .20f * fresnel")==std::string::npos,
                "the CPU mirror covers a face by what its own column extinguishes, as the shader does");
        // The preview renders from above the plane, where the shader's underwater branch of the
        // optical column is exactly zero, so the mirror carries the branch as a comment and the
        // captured-depth side of it as code. Pinned so the omission stays a stated identity
        // rather than becoming a silent drift.
        Require(mirror.find("column=min(lerp(effectiveDepth*reflectionControl.w,interfaceYards,underwater),400.0)")!=std::string::npos&&
                mirror.find("const float column = std::min(effectiveDepth * depthFogFactor, 400.f);")!=std::string::npos,
                "the CPU mirror states which side of the underwater column branch a preview camera takes");
        // Which side of the interface the CAMERA is on. This used to be asked of the triangle
        // (`1-step(-1e-4,dot(n,v))`), and a triangle cannot answer it: a facing test is true of
        // every face near edge-on, and at a low camera above water the whole sea out toward the
        // horizon is within a degree or two of edge-on. Half the sea therefore took the
        // underwater optical path while the camera stood in the air - the water-to-air mirror,
        // the camera-column absorption over the full distance to that face, and the coverage
        // floor with them - which is the dark sheet a player reads as seeing the front faces of
        // waves through the backs of them. The answer now comes from the draw's own matrices on
        // the CPU, in units of the wave's own lift, and is one number for the whole tile.
        // Pinned on both sides of the fix: the classification that replaced the facing test, the
        // absence of the facing test, and the three optical terms that are only correct because
        // of it (the critical-angle floor, the camera-to-surface column and the coverage that
        // matches what that column extinguishes).
        const auto classification=pixel.find("float underwater=smoothstep(-1.0,1.0,waveSwell2.w);");
        Require(classification!=std::string::npos&&
                pixel.find("1-step(-1e-4,cameraSide)")==std::string::npos&&
                pixel.find("float cameraSide=dot(n,v);")!=std::string::npos&&
                pixel.find("float flip=cameraSide<0?-1:1;")!=std::string::npos&&
                pixel.find("float totalInternal=1-smoothstep(.63,.69,NoV);")!=std::string::npos&&
                pixel.find("fresnel=max(fresnel,underwater*totalInternal*foreverStyle.z);")!=std::string::npos&&
                pixel.find("surfaceAlpha=max(surfaceAlpha,reflectionAmount);")!=std::string::npos,
                "the camera's side of the interface comes from the draw's own matrices, not from which way a triangle faces");
        // Under the surface, what is "behind" a fragment is AIR - the sky, a shore, a hull - so
        // the column measured from the surface down to the captured depth behind it is a length
        // of air mistaken for a length of water: a hundred yards of murk with the camera three
        // yards under, which collapsed every upward view onto the deep colour. The water this ray
        // really crosses is the camera-to-surface segment, which is the same length the
        // camera-column absorption has always used, so the two are now the same number and the
        // lerp between them is gated on the classification above.
        const auto cameraColumn=pixel.find("float cameraColumn=min(length(viewPos),250.0);");
        const auto column=pixel.find("float column=min(lerp(effectiveDepth*reflectionControl.w,interfaceYards,underwater),400.0);");
        Require(cameraColumn!=std::string::npos&&column!=std::string::npos&&cameraColumn<column&&
                pixel.find("float column=min(effectiveDepth,400.0);")==std::string::npos&&
                pixel.find("float totalInternal=1-smoothstep(.63,.69,NoV);")!=std::string::npos&&
                pixel.find("float interfaceYards=lerp(250.0,cameraColumn*(1.0+clamp(lightColor.w,0.0,15.0)),1.0-totalInternal);")
                    !=std::string::npos&&
                pixel.find("float4 lightColor:register(c202);")!=std::string::npos&&
                code.find("controls[11]=renderer::underwater::ExitWash(effectiveTurbidity,submersion,true)-1.f;")
                    !=std::string::npos&&
                pixel.find("float3 cameraTransmittance=exp(-bodyStyle.x*waterStyle.y*cameraColumn*absorbTint.rgb);")
                    !=std::string::npos&&
                pixel.find("rgb=lerp(rgb,underwaterBody,underwater);")!=std::string::npos&&
                pixel.rfind("float cameraColumn=min(length(viewPos),250.0);")==cameraColumn,
                "underwater the optical column is the camera-to-surface segment with the exit wash on it, and the far column outside Snell's window");
        // The underside is the medium's own colour, everywhere, not the sky's. Three
        // terms used to hand it the air's palette: the reflection MISS fell back to
        // the sky/fog colour (rayUp saturates at zero from below, so the horizon
        // term was the whole of it), an SSR HIT arrived in the client's dry in-air
        // shading because the full-screen pass leaves liquid pixels alone, and the
        // sheet fogged into fogColor at range - over a coverage that still leaked
        // most of the live scene through a shallow sheet outside Snell's window,
        // where nothing may be transmitted at all. A blue lake's underside on a
        // warm evening was orange by the first and third of those. Now the miss
        // reflects the body's asymptote, the hit is marched over the leg it
        // travelled, the camera segment is charged once to the finished pixel
        // (after foam, not before reflection), coverage is opaque at total
        // internal reflection, and the fog target is the medium.
        const auto foamGlow=pixel.find("*saturate(sunElev*2.+.2)*.55*deepTint.a;");
        const auto cameraCharge=pixel.find("rgb=lerp(rgb,underwaterBody,underwater);",foamGlow);
        Require(pixel.find("float3 underEnvironment=(deepTint.rgb+inScatter)*waterTint.rgb;")!=std::string::npos&&
                pixel.find("float3 environment=lerp(sky,underEnvironment,underwater);")!=std::string::npos&&
                pixel.find("if(hit<=0)hitDistance=traceDistance;")!=std::string::npos&&
                pixel.find("float3 reflectionTransmittance=exp(-bodyStyle.x*waterStyle.y*hitDistance*absorbTint.rgb);")
                    !=std::string::npos&&
                foamGlow!=std::string::npos&&cameraCharge!=std::string::npos&&foamGlow<cameraCharge&&
                pixel.find("surfaceAlpha=max(surfaceAlpha,underwater*max(totalInternal,1-transmittance.g));")
                    !=std::string::npos&&
                pixel.find("float3 fogTarget=lerp(lerp(fogColor.rgb,deepTint.rgb,seaShare),underEnvironment,underwater);")
                    !=std::string::npos&&
                pixel.find("result_.color=float4(fog*(rgb-fogTarget)+fogTarget,surfaceAlpha);")!=std::string::npos,
                "from below the sheet reflects and fogs into the medium it is in, covers where it mirrors, and charges the camera segment to the finished pixel");
        // A sea's horizon is not the sky. The air fog alone painted distant water the exact
        // colour of the sky it faded into, which reads as a hole in the world; the target
        // takes the body's deep colour in proportion to the column's own saturation, above
        // water only, so a shallow shore still fades exactly as before.
        {
            const auto farSea=pixel.find("float farSea=smoothstep(40.0,320.0,length(viewPos))*(1-underwater);");
            const auto share=pixel.find(
                "float seaShare=max(saturate(absorption)*(1-underwater)*.55,farSea*.9);");
            const auto target=pixel.find(
                "float3 fogTarget=lerp(lerp(fogColor.rgb,deepTint.rgb,seaShare),underEnvironment,underwater);");
            const auto fade=pixel.find("float3 fogTarget=lerp(lerp(");   // exactly one author
            Require(farSea!=std::string::npos&&share!=std::string::npos&&target!=std::string::npos&&
                    target==fade&&farSea<share&&share<target,
                    "the distant sheet fades into the sea's own colour above water, and into the medium below it");
            const std::string mirror=ReadAlongside("WaterPreview.cpp");
            Require(mirror.find("const float farSea = Smoothstep(40.f, 320.f, viewZ);")!=std::string::npos&&
                    mirror.find("std::max(Saturate(absorption) * .55f, farSea * .9f)")!=std::string::npos,
                    "the CPU preview fogs toward the same tinted target, deepening with distance");
        }
        // WATER OPACITY, second half. The alpha floor only decides how much of the scene the
        // sheet lets past; the body's COLOUR is built from the refracted seabed, so on a
        // clear shallow shelf the surface painted the bed back into the frame at full
        // coverage and the slider moved the alpha without moving the picture - the report
        // "opacity does nothing visually". The body now converges on the asymptote an
        // infinitely thick column converges on, gated by the water actually over the pixel.
        {
            const auto bed=pixel.find("float3 bottom=refracted*transmittance+inScatter+deepTint.rgb*absorption;");
            const auto bodyLine=pixel.find("float3 body=lerp(rgb,bottom,depthValid*saturate(waterStyle.x));",bed);
            const auto sea=pixel.find("float3 seaBody=deepTint.rgb+inScatter;",bodyLine);
            const auto gate=pixel.find(
                "float bedTrusted=depthValid*step(viewPos.z+.25,behindZ);",sea);
            const auto trust=pixel.find(
                "float bedFade=lerp(1.0,saturate(effectiveDepth*.5),bedTrusted);",gate);
            const auto fade=pixel.find(
                "body=lerp(body,seaBody,opacityControl.x*bedFade);",trust);
            Require(bed!=std::string::npos&&bodyLine!=std::string::npos&&sea!=std::string::npos&&
                    gate!=std::string::npos&&trust!=std::string::npos&&fade!=std::string::npos&&
                    bed<bodyLine&&bodyLine<sea&&sea<gate&&gate<trust&&trust<fade,
                    "water opacity fades the WHOLE body toward the sea, after the refraction mix");
            // An unknown or implausible bed reading must not switch the slider off.
            // `lerp(1.0, ...)` is the whole point: untrusted means full authority.
            Require(pixel.find("float bedFade=lerp(1.0,saturate(effectiveDepth*.5),bedTrusted);")
                        !=std::string::npos &&
                    pixel.find("opacityControl.x*saturate(effectiveDepth*.5)")==std::string::npos,
                    "a missing or implausible depth reading still lets opacity hide the bed");
            // The regression that mattered: the fade must come after `body` is composed from
            // the client's colour and the captured bottom, or it only scales the part
            // REFRACTION owns (14% at the shipped setting) and the slider does nothing.
            Require(pixel.find("float3 bottom=lerp(refracted*transmittance")==std::string::npos,
                    "opacity is not applied inside the refracted term");
            Require(pixel.find("waterBody=lerp(body,seaBody,saturate(absorption*(.85-.22*fresnel)));")
                        !=std::string::npos,
                    "the depth transition still converges on the same sea body");
            const std::string mirror=ReadAlongside("WaterPreview.cpp");
            Require(mirror.find("const float bedFade = validDepth > .5f ? Saturate(effectiveDepth * .5f) : 1.f;")
                        !=std::string::npos &&
                    mirror.find("body = Lerp(body, Add(deep, inScatter), .85f * bedFade);")
                        !=std::string::npos,
                    "the CPU preview fades the whole body at the shipped opacity too");
            // And the case with no scene copy at all keeps the BODY, not the client's own
            // liquid: over open ocean that fallthrough was a pale, nearly transparent sheet
            // the colour of the sky - the "completely transparent water" of the report.
            // (Comments are stripped from `pixel`, so only code can be pinned here.)
            Require(pixel.find("} else {")!=std::string::npos&&
                    pixel.find("waterBody=deepTint.rgb+inScatter;")!=std::string::npos&&
                    pixel.find("waterBody=deepTint.rgb+inScatter;")>pixel.find("} else {"),
                    "an uncaptured surface shows the sea's own colour, never the client's pale liquid");
        }
        // DEPTH FOG: the view DOWN into the water charges the sunlight's trip to the bed as
        // well as the trip back, so submerged terrain dissolves into the body with depth
        // instead of standing dark under clear-looking water. One register lane that was
        // uploaded unread, one multiply on the captured-depth side only (the submerged
        // branch is the medium's own column and is not scaled), exactly 1 with the toggle
        // off, and plumbed as a toggle plus a slider through the same [Water] tuning,
        // overlay and CPU mirror as every other water number.
        Require(code.find("controls[19]=depthFogEnabled?depthFogStrength:1.f;")!=std::string::npos&&
                code.find("depthFogEnabled=ReadTuning(L\"DepthFogEnabled\",1)!=0;")!=std::string::npos&&
                code.find("depthFogStrength=std::clamp(ReadTuning(L\"DepthFogPercent\",200),100,400)*.01f;")!=std::string::npos&&
                mirror.find("const float depthFogFactor = c.depthFogOn ? std::clamp(c.depthFog, 100.f, 400.f) * .01f : 1.f;")!=std::string::npos,
                "depth fog charges both legs of the bed's path, is exactly the old column when off, and reaches the mirror");
        // Coverage. Water writes no depth and blends, so whatever is behind a face shows through
        // by exactly (1-alpha) - and behind the face of a wave is very often another face of the
        // same wave. The old `absorption*(.78-.20*fresnel)` leaked 22% of the sea behind into
        // the sea in front at every depth past the first e-fold, and 42% at the grazing angles
        // where wave faces overlap most: the second half of the double wave. 1-T is the fraction
        // of what is behind that this column really extinguishes, and it is the same number the
        // colour converged to the deep tint by.
        Require(pixel.find("float depthAlpha=absorption;")!=std::string::npos&&
                pixel.find("absorption*(.78-.20*fresnel)")==std::string::npos&&
                pixel.find("float surfaceAlpha=saturate(baseAlpha+(1-baseAlpha)*depthAlpha);")!=std::string::npos,
                "a water face covers by what its own column extinguishes, so the sea behind it cannot show through the sea in front");
        // WATER OPACITY. Coverage is not the same question as clarity, and the shader had no
        // lane for it at all: baseAlpha came from the client's painted liquid and depthAlpha
        // from the body's own extinction, so a clear, shallow sea multiplied to almost nothing
        // and the bed read straight up through the surface - the picture the report is about.
        // The knob is a floor under the coverage, applied AFTER the optical terms (so it can
        // only add cover where the optics already agreed) and BEFORE the wrong-side fade (so a
        // far sheet still dissolves at its silhouette instead of being pinned opaque by it),
        // read from its own ini key so it is not a second spelling of WATER CLARITY, and
        // uploaded on a lane inside the block the draw already writes.
        {
            const auto floorRule=pixel.find(
                "surfaceAlpha=max(surfaceAlpha,lerp(opacityControl.x,1.0,openOceanCoverage));");
            const auto optics=pixel.find("surfaceAlpha=max(surfaceAlpha,underwater*max(totalInternal,1-transmittance.g));");
            const auto fade=pixel.find("surfaceAlpha*=1-smoothstep(0.0,.25,wrongSide);");
            // THE BLEND. Coverage is only an image if the fixed-function blend lets
            // the world through by (1-a). The native liquid material owns that state,
            // and an additive (or ONE-sourced) ocean makes every alpha we write a
            // no-op: the seabed stays at full strength and only the body colour
            // changes, which is the report "opacity hides the old texture but not
            // what I can see through". The pass now forces the one blend its output
            // is authored for and lets the captured state block put the original back.
            Require(code.find("d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);")!=std::string::npos&&
                    code.find("d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);")!=std::string::npos&&
                    code.find("d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);")!=std::string::npos&&
                    code.find("d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);")!=std::string::npos&&
                    code.find("d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);")!=std::string::npos&&
                    code.find("device->SetRenderState(D3DRS_ALPHABLENDENABLE,oldBlend[0]);")!=std::string::npos&&
                    code.find("liquid draw composited with blend enable=")!=std::string::npos,
                    "the replaced draw composites with an alpha blend, and the native state is restored");
            Require(pixel.find("float4 opacityControl:register(c199);")!=std::string::npos&&
                    floorRule!=std::string::npos&&optics!=std::string::npos&&fade!=std::string::npos&&
                    optics<floorRule&&floorRule<fade,
                    "opacity is a floor on coverage: after the optical terms, before the wrong-side fade");
            // A floor cannot cover what is not there. Over open water the capture holds no
            // geometry behind the sheet - only the clear colour it was never asked to
            // replace - so a partial alpha reveals emptiness itself, which is the second
            // half of the report ("still transparent where the seabed has run out"). The
            // sea is then the whole column and coverage is complete; under the surface the
            // same `behindZ` is the sky through Snell's window, so the term is above-water only.
            {
                const auto openTerm=pixel.find(
                    "float openOceanCoverage=max(openOcean*(1-underwater),1-captureValid);");
                const auto openRule=pixel.find(
                    "surfaceAlpha=max(surfaceAlpha,lerp(opacityControl.x,1.0,openOceanCoverage));");
                Require(openTerm!=std::string::npos&&openRule!=std::string::npos&&
                        optics<openTerm&&openTerm<openRule&&openRule<fade,
                        "open ocean is fully covered, above water only, and still before the wrong-side fade");
                // A frame with no scene copy has nothing behind the surface either: the
                // "behind" a partial alpha would blend is whatever the client left in the
                // target, which over water is the clear colour. Nothing captured is not
                // something to see through.
                Require(pixel.find("1-captureValid)")!=std::string::npos,
                        "a missing scene capture covers the sheet instead of blending the clear colour through it");
                const std::string mirror=ReadAlongside("WaterPreview.cpp");
                Require(mirror.find("alpha = std::max(alpha, Mix(.85f, 1.f, openOcean));")!=std::string::npos,
                        "the CPU preview carries the same open-ocean coverage rule");
            }
            Require(pixel.find("float surfaceAlpha=saturate(baseAlpha+(1-baseAlpha)*depthAlpha);")
                        !=std::string::npos&&
                    pixel.find("depthAlpha=absorption*opacityControl")==std::string::npos&&
                    code.find("opacityPercent=float(std::clamp(ReadTuning(L\"OpacityPercent\",85),0,100));")
                        !=std::string::npos&&
                    code.find("absorptionStrength=std::clamp(ReadTuning(L\"DepthAbsorptionPercent\",38),0,100)*.01f;")
                        !=std::string::npos,
                    "opacity has its own key and does not fold into depth absorption, so clarity keeps its meaning");
            // The lane is c199, inside the saved/restored c180..c218 block, and the per-frame
            // upload writes it in the same place the rest of the controls are built - not after
            // the cached-light memcpy, which stops at c196.
            const auto lane=code.find("pixelConstants[19][0]=std::clamp(opacityPercent*.01f,0.f,1.f);");
            const auto cached=code.find("std::memcpy(pixelConstants,cache.values,sizeof(cache.values));");
            Require(lane!=std::string::npos&&cached!=std::string::npos&&lane<cached&&
                    code.find("SetPixelShaderConstantF(180,pixelConstants[0],39)")!=std::string::npos,
                    "opacity rides c199 inside the one c180..c218 upload, ahead of the cached-light splice that only overwrites c180..c196");
            const std::string ini=ReadAlongside("../GraphicsEffects.ini");
            const std::string overlay=ReadAlongside("../src/Environment/OverlayItems.h");
            Require(ini.find("OpacityPercent=85")!=std::string::npos&&
                    overlay.find("L\"Water\",L\"OpacityPercent\",\"WATER OPACITY\",0,100,5,85}")!=std::string::npos&&
                    overlay.find("L\"Water\",L\"ClarityPercent\",\"WATER CLARITY\",5,200,5,100}")!=std::string::npos,
                    "opacity ships at 85 in the ini and as its own 0..100 F7 slider beside clarity");
        }
        // The CPU half: the plane over the camera, read from the draw's own modelView and one
        // of its own vertices - and nothing else. It used to be read against the live VIEW
        // transform and refused outright when the view's basis determinant was negative (the
        // client's reflection pass); on a real client that read nothing at all and the medium
        // never engaged, because a shader-driven client leaves its fixed-function VIEW state
        // holding whatever a fixed-function draw last put there, and the determinant's sign is
        // a handedness convention this tree never established. Reflection draws are now kept
        // out by their render target, a scaled tile still reports the same plane (a scale's
        // fourth row is (0,0,0,1), so it cancels out of row 3), the reading is held across
        // draws that cannot answer, and it is divided by the wave's own lift so a swimmer
        // crossing a rough surface fades instead of snapping.
        Require(code.find("renderer::underwater::PlaneOverCamera(modelView,")!=std::string::npos&&
                code.find("const bool haveVertices=mainTarget&&ReadLiquidVertices(d,vertexSamples);")!=std::string::npos&&
                code.find("mainTarget=mainTargetSurface&&currentTarget.Get()==mainTargetSurface;")!=std::string::npos&&
                code.find("submersionTracker.Observe(planeReading);uwPlaneValid++;")!=std::string::npos&&
                code.find("const float planeBand=std::max(renderer::underwater::kShaderBandYards,")!=std::string::npos&&
                code.find("geometry.amplitude*renderer::water::kGroupMax);")!=std::string::npos&&
                code.find("controls[75]=std::clamp(submersion/planeBand,-2.f,2.f);")!=std::string::npos&&
                code.find("const float submersion=renderer::underwater::ClassifiedYards(")
                    !=std::string::npos&&
                code.find("(haveVertices&&vertexSamples.haveCoverage)?&liquidCoverage:nullptr")
                    !=std::string::npos&&
                code.find("const float submersion=(planeReading.valid&&planeReading.local)")
                    ==std::string::npos,
                "the camera's side of the plane is read from the draw itself, refused when the draw is off-screen, and held when it cannot answer");
        // The read's own sweep (round 18): the coverage extremes are measured
        // once as the draw is copied in and handed to the reading, so a far
        // sheet is refused from its bounding box instead of a per-frame
        // triangle scan; the element count is computed in 64-bit under a
        // ceiling, so an absurd primitive count cannot wrap small and have
        // the first few emitted vertices swear they were the whole draw;
        // and the vertex declaration's position is parsed once per
        // declaration instead of on every liquid draw.
        Require(code.find("MeasureLiquidCoverageBounds(liquidCoverageScratch,consumedElements,")!=std::string::npos&&
                code.find("vertexSamples.haveCoverageBounds};")!=std::string::npos&&
                code.find("out.haveCoverageBounds=entry.haveCoverageBounds;")!=std::string::npos&&
                code.find("elements<=(UINT64(1)<<24)?static_cast<UINT>(elements):0;")!=std::string::npos&&
                code.find("liquidDeclarationCache.Get()==declaration.Get()")!=std::string::npos&&
                code.find("liquidDeclarationCache.Reset();")!=std::string::npos,
                "the liquid read measures its coverage bounds once, counts elements without wrapping, and parses each declaration once");
        // Foam placement must not be evaluated in view-depth units: that makes a fixed seabed
        // look shallower/steeper as the camera pitches. Reconstruct the bed point on its screen
        // ray, use vertical world separation for shore bands, and derive slope from a normal
        // measured over reconstructed points a fixed world span apart: a one-pixel derivative
        // stencil hands the slope to the captured depth's storage step, which printed through
        // shoal/beach foam as quad-scale ticks on shallow beds (modeled numerically above).
        // Every depth-gated foam source carries foamDepthValid, backwash included: a pixel
        // with sky behind it clamps its foam depth to zero and would read as ankle-deep swash.
        Require(pixel.find("float3 bottomView=float3((2*screenUV.x-1)*behindZ/max(reflectionProjection.z,1e-6),")!=std::string::npos&&
                pixel.find("float foamDepthSigned=dot(viewPos-bottomView,upWind.xyz);")!=std::string::npos&&
                pixel.find("static const float kBedSlopeSpanYards=1.5;")!=std::string::npos&&
                pixel.find("float bedTap=clamp(kBedSlopeSpanYards/max(bedPixelYards,1e-4),1.0,16.0);")!=std::string::npos&&
                pixel.find("float3 bedNormal=cross(bedE-bedW,bedS-bedN);")!=std::string::npos&&
                pixel.find("cross(ddx(bottomView),ddy(bottomView))")==std::string::npos&&
                pixel.find("float bedSlope=length(bedNormal-upWind.xyz*bedNormalUp)/max(abs(bedNormalUp),1e-6);")!=std::string::npos&&
                pixel.find("float swashDepth=max(foamDepth-sin01.x*runUp,0);")!=std::string::npos&&
                pixel.find("float shore=foamDepthValid*shallowEdge;")!=std::string::npos&&
                pixel.find("float shoal=foamDepthValid*saturate(bedSlope*bodyStyle.w*2.0-.3)*saturate(1-foamDepth*.05);")!=std::string::npos&&
                pixel.find("backwash=foamDepthValid*saturate(.55+.45*sin(swashDepth*2.4-sin01.x*2.1))")!=std::string::npos&&
                pixel.find("ddx(effectiveDepth)")==std::string::npos&&
                pixel.find("float contact=lerp(1,saturate(effectiveDepth*.9),step(9.5,qualityLevel));")!=std::string::npos&&
                mirror.find("const float e = 1.5f;")!=std::string::npos&&
                mirror.find("validDepth * Saturate(.55f")!=std::string::npos,
                "shore, shoal and backwash foam use camera-invariant world depth and a yard-scale bed slope while optical depth remains unchanged");
        Require(vertex.find("waveWarpPhase:register(c203)")!=std::string::npos&&
                vertex.find("waveGroupPhase:register(c204)")!=std::string::npos&&
                vertex.find("float4 waveWarp:TEXCOORD7")!=std::string::npos&&
                vertex.find("o.waveWarp=float4(waveWarp,gain);")!=std::string::npos&&
                vertex.find("o.worldRel=float4(rel,groupGradient);")!=std::string::npos&&
                pixel.find("float4 waveWarp:TEXCOORD7")!=std::string::npos&&
                pixel.find("float4 worldRel:TEXCOORD4")!=std::string::npos&&
                pixel.find("dot(worldRel.xy,waveLattice.xy)")!=std::string::npos&&
                pixel.find("noiseGradient(warpCoord)")==std::string::npos&&
                pixel.find("noiseGradient(groupCoord)")==std::string::npos&&
                code.find("SetPixelShaderConstantF(180,pixelConstants[0],39)")!=std::string::npos&&
                code.find("SetVertexShaderConstantF(200,vertexControls,5)")!=std::string::npos,
                "the vertex stage anchors the warp AND the set envelope and interpolates both to pixels without resampling");
        // The envelope has to ride the lanes the two stages already had. Interpolator counts
        // are a hardware rule, not a style preference: with no register declared psize0,
        // vs_3_0 gives the vertex stage eleven o# registers and ps_3_0 accepts ten v# ones,
        // and fxc answers a twelfth with X5622/X5300 - which is exactly what happened when
        // this round first added one. Counting them here says it in the source language, in
        // the second it is broken, instead of an hour later in a Windows job log.
        const auto between=[](const std::string& text,const char* open,const char* close) {
            const size_t a=text.find(open); if (a==std::string::npos) return std::string();
            const size_t b=text.find(close,a); return b==std::string::npos?std::string():text.substr(a,b-a);
        };
        const auto semantics=[](const std::string& text) {
            size_t n=0;
            for (const char* needle : {":POSITION",":COLOR0",":FOG",":TEXCOORD"})
                for (size_t at=text.find(needle); at!=std::string::npos; at=text.find(needle,at+1)) ++n;
            return n;
        };
        Require(semantics(between(vertex,"struct Output","}"))==11,
                "the vertex stage uses exactly vs_3_0's eleven output registers, no more");
        const std::string psInputs=between(pixel,"WaterOutput main(",")");
        Require(!psInputs.empty()&&1+std::count(psInputs.begin(),psInputs.end(),',')==10,
                "the pixel stage takes exactly ps_3_0's ten input registers, no more");
        Require(vertex.find("TEXCOORD8")==std::string::npos&&pixel.find("TEXCOORD8")==std::string::npos&&
                pixel.find("waveGroup")==std::string::npos&&vertex.find("o.waveGroup=")==std::string::npos,
                "the envelope is packed into TEXCOORD4/TEXCOORD7 rather than appended as a twelfth");
        // Save, upload and restore one contiguous c180..c218 pixel block. Besides retaining
        // every touched register, this keeps the three untouched gap registers byte-for-byte
        // while reducing six D3D9 constant calls per tile to three. The native vertex inputs
        // are likewise fetched through one enclosing c5..c35 span rather than three calls.
        for (const char* spelling : {"GetPixelShaderConstantF(180,oldPixelConstants,39)",
                                     "GetVertexShaderConstantF(200,oldVertexControls,5)",
                                     "GetVertexShaderConstantF(5,nativeVertexConstants[0],31)",
                                     "SetPixelShaderConstantF(180,pixelConstants[0],39)",
                                     "SetPixelShaderConstantF(180,oldPixelConstants,39)",
                                     "SetVertexShaderConstantF(200,oldVertexControls,5)",
                                     "float oldPixelConstants[156]{},oldVertexControls[20]{}",
                                     "float pixelConstants[39][4]","float vertexControls[20]="})
            Require(code.find(spelling)!=std::string::npos,
                    "water constants use the complete batched save/upload/restore windows");
        Require(code.find("GetPixelShaderConstantF(200,")==std::string::npos&&
                code.find("GetPixelShaderConstantF(180,oldLocalLights,17)")==std::string::npos&&
                code.find("SetPixelShaderConstantF(200,controls,19)")==std::string::npos,
                "no split pixel-constant driver calls remain in the per-tile path");
        const auto occurrences=[](const std::string& text,const std::string& needle) {
            size_t count=0;
            for(size_t at=text.find(needle);at!=std::string::npos;at=text.find(needle,at+needle.size()))++count;
            return count;
        };
        Require(occurrences(code,"HasScopedValue(L\"Water\",")==3&&
                code.find("scopedWaterColour=renderer::locationtuning::HasScopedValue")!=std::string::npos&&
                code.find("if(!scopedWaterColour)")!=std::string::npos&&
                code.find("if(!scopedDeepColour)")!=std::string::npos&&
                code.find("if(!scopedAbsorptionColour)")!=std::string::npos,
                "location override lookups are cached on reload rather than repeated per water tile");
        Require(code.find("cache.frameIndex!=frame.frameIndex")!=std::string::npos&&
                code.find("cache.selectionSignature!=signature")!=std::string::npos&&
                code.find("cache.values[i*4+axis]=light.position.x")!=std::string::npos&&
                code.find("memcpy(pixelConstants,cache.values,sizeof(cache.values))")!=std::string::npos&&
                occurrences(code,"localLightConstantCache.valid=false")==2,
                "selected local lights are transformed once per immutable frame instead of once per tile");
        Require(pixel.find("[branch] if(confidence>0)")!=std::string::npos&&
                pixel.find("stepIndex<12 && stepIndex<ssrSteps && hit<1.0")!=std::string::npos&&
                pixel.find("lightStep<12 && lightStep<lightSteps && lightVisibility>0")!=std::string::npos&&
                pixel.find("[branch] if(distanceSquared>=radius*radius) continue;")!=std::string::npos,
                "zero-contribution SSR, celestial and local-light work exits before expensive samples");
        // WATER QUALITY hands both marches a trip count instead of a compile-time one, so a
        // level can buy or give back ray steps without a second compiled shader. Pinned in all
        // three parts: the count arrives from a constant register, the loop is WRITTEN against
        // the ceiling so any count up to it is legal, and 0 skips the loop through the same
        // `steps>0` test that decides whether the work is worth doing at all - which is also
        // what stops a low level from paying for the scene copy it would not read.
        Require(pixel.find("int ssrSteps=(int)reflectionControl.y;")!=std::string::npos&&
                pixel.find("int lightSteps=(int)reflectionControl.z;")!=std::string::npos&&
                pixel.find("reflectionControl.x>.001 && ssrSteps>0 && reflectionBudget>.004")!=std::string::npos&&
                pixel.find("celestialPath>.00005 && lightSteps>0")!=std::string::npos,
                "the quality ladder gives both ray marches a trip count, and 0 skips them outright");
        Require(pixel.find("float q=float(stepIndex+1)*invSteps;")!=std::string::npos&&
                pixel.find("float q=float(lightStep+1)*invLightSteps;")!=std::string::npos&&
                pixel.find("/8.0")==std::string::npos,
                "both marches divide by the steps they were actually given, not by a baked-in eight");
        const std::string volumeCode=stripComments(ReadAlongside("../VolumeIntegration.h"));
        Require(volumeCode.find("const bool volumeConsumesWaterBoundary = enabled && active && HasActiveEffects();")!=std::string::npos&&
                volumeCode.find("if (volumeConsumesWaterBoundary && target && SUCCEEDED(target->GetDesc(&targetDesc)))")!=std::string::npos&&
                volumeCode.find("else if (!volumeConsumesWaterBoundary && resources.waterMaskOwner)")!=std::string::npos,
                "water-only scene capture skips and releases atmosphere-only full-resolution MRTs");
        Require(pixel.find("float2 tilt=-slopes*(controls.z/jacobian)*flip;")!=std::string::npos,
                "a crest leans toward the light it catches: the normal is the negative gradient");
        Require(pixel.find("float jacobian=max(1.0-pinch,.3);")!=std::string::npos&&
                pixel.find("float pinch=controls.x*gain*(dot(kWgt01*kRad01,sin01)+dot(kWgt4*kRad4,sin4)+dot(kWgt2*kRad2,sin2));")
                    !=std::string::npos,
                "the shading sharpens by the complete reconstructed Gerstner Jacobian and never divides by zero");
        // The two longest trains are evaluated exactly once by the vertex stage, where they
        // are already needed for displacement. Its four otherwise-unused UV lanes carry A/B's
        // sin/cos pairs. Renormalizing each interpolated pair keeps it on the unit circle and
        // reconstructs height, x/y slope and pinch without another projection or transcendental.
        // The start belongs only where the angle is evaluated; weights/directions/radians remain
        // in the pixel stage because they cheaply assemble the original analytic terms.
        // The displaced set is three trains now, not two: A and B always, and the twenty yard
        // mid wave C at WATER QUALITY 8 and above. C's coefficients are in BOTH stages for a
        // different reason than A's and B's are - the vertex stage needs its angle to lift and
        // slide the mesh, and the pixel stage still evaluates it as one of the six shading
        // trains it assembles its own eight-train field from - but the requirement is the same:
        // the two stages cannot disagree about where a crest is.
        for (const char* spelling : {"0.39269908","0.54636394","0.8290376","-0.5591929",
                                     "0.78539816","0.9335804","0.3583679","5.68"})
            Require(vertex.find(spelling)!=std::string::npos&&pixel.find(spelling)!=std::string::npos,
                    "both stages assemble the displaced long trains from the same coefficients");
        Require(vertex.find("4.255")!=std::string::npos&&pixel.find("4.255")==std::string::npos&&
                vertex.find("float longSwell=(kWeightA*sinA+kWeightB*sinB+midGate*kWeightC*sinC)*meshScale;")
                    !=std::string::npos&&
                vertex.find("cosA,cosB")!=std::string::npos&&vertex.find("sinA,sinB")!=std::string::npos&&
                pixel.find("float2 trigInv=rsqrt(max(uv0.zw*uv0.zw+uv1.zw*uv1.zw,1e-4));")!=std::string::npos&&
                pixel.find("float2 sin01=uv0.zw*trigInv,cos01=uv1.zw*trigInv;")!=std::string::npos&&
                pixel.find("float swell=dot(kWgt01,sin01)+dot(kWgt4,sin4)+dot(kWgt2,sin2);")!=std::string::npos&&
                pixel.find("sincos(t01")==std::string::npos&&pixel.find("float2 t01=")==std::string::npos,
                "spare UV lanes reconstruct the long waves without pixel-stage sin/cos work");
        // The five shading-only trains, and their starts: present in the pixel stage, absent
        // from the vertex stage. That split is the whole geometry/shading budget, so it is
        // pinned in both directions rather than left to a reader to notice. Train C used to be
        // in this list; it moved to the displaced set above when the mid waves arrived, and a
        // level below 8 still leaves the vertex stage doing the work of two trains rather than
        // three, because the gate is the level and not a recompiled shader.
        for (const char* spelling : {"1.08330781","1.42799666","1.84799568",
                                     "0.9925462","0.7313537",
                                     "2.46399424","3.22214631","0.4226183","-0.6156615",
                                     "0.075","2.822","0.7395","1.2285"})
            Require(pixel.find(spelling)!=std::string::npos&&vertex.find(spelling)==std::string::npos,
                    "the five short trains stay out of the displaced three");
        Require(pixel.find("float4 waveSwell2:register(c218);")!=std::string::npos&&
                pixel.find("float swell=dot(kWgt01,sin01)+dot(kWgt4,sin4)+dot(kWgt2,sin2);")!=std::string::npos&&
                code.find("controls[72]=wave.swellPhase[6];controls[73]=wave.swellPhase[7];")!=std::string::npos,
                "the two shortest trains remain a pixel batch on their own register");
        Require(pixel.find("rippleFine.yz*.30*filter*filter")!=std::string::npos,
                "a finer counter-phased chop octave breaks the corduroy in the pixel stage");
        Require(header.find("kTrainCells[kTrainCount] = {16.f, 11.5f, 8.f, 5.8f, 4.4f, 3.4f, 2.55f, 1.95f}")
                    !=std::string::npos&&
                header.find("kTrainDirX[kTrainCount] = {1.f, .8290376f, .9335804f, .6819984f, .1218693f, -.3420201f, .4226183f, -.6156615f}")
                    !=std::string::npos&&
                header.find("kTrainDirY[kTrainCount] = {0.f, -.5591929f, .3583679f, .7313537f, .9925462f, .9396926f, .9063078f, .7880108f}")
                    !=std::string::npos&&
                header.find("kTrainWeight[kTrainCount] = {1.f, .62f, .34f, .19f, .12f, .075f, .052f, .036f}")
                    !=std::string::npos&&
                header.find("kTwoPi / 16.f, kTwoPi / 11.5f, kTwoPi / 8.f, kTwoPi / 5.8f, kTwoPi / 4.4f, kTwoPi / 3.4f, kTwoPi / 2.55f, kTwoPi / 1.95f")
                    !=std::string::npos&&
                header.find("{0.f, .37f * 11.5f, .71f * 8.f, .19f * 5.8f, .55f * 4.4f, .83f * 3.4f, .29f * 2.55f, .63f * 1.95f}")
                    !=std::string::npos,
                "the CPU header carries the same eight wavelengths, directions, weights and starts");
        // The shader's hand-written starts are the header's fractions of each train's own
        // wavelength, spelled as literals because HLSL has no table to read. If one of them
        // stops being the other, a crest the vertex stage lifted is no longer the crest the
        // pixel stage lights, and the offset is invisible in a still frame.
        const float starts[kTrainCount]={0.f,4.255f,5.68f,1.102f,2.42f,2.822f,0.7395f,1.2285f};
        const float weightSum=kTrainWeight[0]+kTrainWeight[1]+kTrainWeight[2]
            +kTrainWeight[3]+kTrainWeight[4]+kTrainWeight[5]
            +kTrainWeight[6]+kTrainWeight[7];
        for (int train=0;train<kTrainCount;++train) {
            Require(std::fabs(kTrainPhaseCells[train]-starts[train])<1e-3f,
                "each train's start is the same fraction of its own wavelength in every copy");
        }
        Require(std::fabs(kSwellToUnit-1.f/(2.f*weightSum))<1e-5f&&
                std::fabs(1.f/(2.f*weightSum)-.20550760f)<1e-6f,
                "the crest normalization both stages carry is the table's own weight sum");
        // The grouping envelope's three numbers, in both stages and in the header: a gain
        // whose constants disagree between stages is a set that dims the shading but not the
        // geometry, which is exactly the artefact class this round was cut for.
        Require(vertex.find("float gain=kGroupBase+kGroupDepth*waveGroup.x;")!=std::string::npos&&
                pixel.find("float gain=kGroupBase+kGroupDepth*waveWarp.w;")!=std::string::npos&&
                vertex.find("static const float kGroupBase=0.52;")!=std::string::npos&&
                pixel.find("static const float kGroupDepth=0.62;")!=std::string::npos&&
                vertex.find("static const float kGroupScale=0.01347826;")!=std::string::npos&&
                // The pixel stage must NOT own that scale: the gradient it uses was already
                // rotated, scaled by the frequency and by the depth in the vertex stage, which
                // is the only place the envelope's noise is sampled. A second scaling here
                // would differentiate the set twice and lean every crest the wrong way.
                pixel.find("kGroupScale")==std::string::npos,
                "both stages build the same gain, and only the sampler scales its gradient");
        Require(std::fabs(kGroupFrequency*kGroupDepth-.01347826f)<1e-7f&&
                std::fabs(kGroupBase+kGroupDepth-kGroupMax)<1e-6f&&kGroupMax<=1.15f,
                "the envelope's shader literals are the header's frequency, depth and ceiling");
        // One warp, one frequency, one amplitude, three copies: the bend a crest gets in the
        // geometry has to be the bend its normal is chain-ruled through.
        for (const char* spelling : {"(waveWarp.x-.5)*6.5","warp*float2(.6,.8)","+6.5*dot(kDirA,float2(.6,.8))"})
            Require(code.find(spelling)!=std::string::npos,
                "the domain warp's scale is spelled the same way in both stages");
        Require(header.find("inline constexpr float kWaveWarpAmplitude = 6.5f;")!=std::string::npos&&
                header.find("inline constexpr float kWaveWarpFrequency = .05f;")!=std::string::npos,
                "the header agrees with both stages about how far and how often the sea bends");
        // Foam at the edges of waves, and only when the water is active: the pinch says a
        // crest is breaking, the authored wind says whether this body can foam at all, and
        // the foam then takes the mirror and the sun road with it - a whitecap scatters.
        Require(pixel.find("float foamWind=saturate(wind);")!=std::string::npos&&
                pixel.find("float breaking=saturate(pinch*2.4);")!=std::string::npos&&
                pixel.find("float foamPattern=saturate(.45*foamWarp.x+.55*detail.x);")!=std::string::npos&&
                pixel.find("float foamPattern=saturate(.45*waveWarp.x+.55*detail.x);")==std::string::npos&&
                pixel.find("float whitecap=smoothstep(foamThreshold,foamThreshold+.14,capBand);")!=std::string::npos,
                "breaking crests form irregular foam streaks with bounded wind-driven coverage");
        Require(pixel.find("float foamAmount=saturate((shore*waterStyle.z")!=std::string::npos,
                "foam coverage is physically bounded before it masks reflections and adds foam light");
        const size_t foamAt=pixel.find("float foamAmount="), dampAt=pixel.find("float reflectionAmount=");
        Require(foamAt!=std::string::npos&&dampAt!=std::string::npos&&foamAt<dampAt,
                "the foam is known before the reflection is mixed, so it can damp it");
        Require(pixel.find("(1-saturate(foamAmount*.85))")!=std::string::npos&&
                pixel.find("(1-saturate(foamAmount))")!=std::string::npos,
                "foam takes the mirror and the sun road away with it");
        // Per-zone and per-weather authoring has to reach the geometry, not only the
        // shading: the sea state multiplies the lift, and waves off is flat water. So does the
        // quality ladder, through the same argument rather than a second one: a level below the
        // mesh's own train count hands BuildGeometryWaves `false` and the surface stays a flat
        // sheet no matter what the zone authored, which is what makes level 0 cheap in the
        // vertex stage too and not only in the pixel one.
        Require(code.find("BuildGeometryWaves(geometryWavePercent,\n"
                            "                waveMotion.ScalePercent(),seaAgitation,\n"
                            "                geometryWavesEnabled&&qualityPlan.geometryTrains>=renderer::water::kGeometryTrainCount,\n"
                            "                wavesEnabled)")
                    !=std::string::npos,
                "the geometry waves are lifted by the same sea state the shading is");
        Require(mirror.find("TargetSeaState(tuning, weather)") != std::string::npos,
                "the game and the CPU preview ease the same sea state through one function");
        // The look was tuned against this file, so a gain that exists in one copy and not the
        // other is a frame nobody has looked at. Pin the mirror to the same neutral multiply.
        Require(mirror.find("Mul(waterBody, tint)") != std::string::npos
                && mirror.find("Mul(tint, 2.f)") == std::string::npos,
                "the CPU mirror multiplies the same tint with no gain of its own");
        // The waves too: the preview has no vertex stage, so it displaces the shaded point
        // itself, but the field, the trains, the pinch and the sign of the tilt are the same
        // expressions - otherwise the published frames are of water the client does not draw.
        Require(mirror.find("WarpWavePlane(field, plane.x, plane.y)") != std::string::npos
                && mirror.find("EvaluateSwell(warp.plane.x, warp.plane.y, field.swellPhase)") != std::string::npos
                && mirror.find("BuildGeometryWaves(c.geometryWaves") != std::string::npos,
                "the CPU preview applies the same anchored warp before evaluating the swell");
        // The set, mirrored on the CPU the same way: sampled once per displaced vertex,
        // multiplied into height/pinch, and differentiated into the slope. A preview that
        // skipped it would be a picture of water the client stopped drawing.
        Require(mirror.find("GroupEnvelope(field, plane.x, plane.y)") != std::string::npos
                && mirror.find("const float gain = ") != std::string::npos
                && mirror.find("geometry.sigma * gain * trains.pinch") != std::string::npos,
                "the CPU preview samples the same grouping envelope and carries it into pinch")
        ;
        Require(mirror.find("kWaveWarpAmplitude * warp.gradientX") != std::string::npos
                && mirror.find("slideAlongWarp*kWaveWarpAmplitude*warp.gradientY") != std::string::npos,
                "the CPU preview uses the shader's warp gradient for normals and vertex slides");
        Require(mirror.find("-(axisX.x * w.slopes.x * tilt") != std::string::npos
                && mirror.find("swell * kSwellToUnit") != std::string::npos,
                "the CPU preview tilts the same way and normalizes the swell by the same divisor");
        Require(mirror.find("rippleFine.dx * .30f * filter * filter") != std::string::npos
                && mirror.find("plane.x * 2.17f + field.ripplePhase[1] * 1.31f") != std::string::npos,
                "the CPU preview mirrors the fine chop octave at the same frequency and phase");
        Require(mirror.find("Smoothstep(foamThreshold, foamThreshold + .14f, capBand)") != std::string::npos
                && mirror.find(".45f * warp.noiseValue + .55f * w.detail") != std::string::npos
                && mirror.find("const float foamWind = Saturate(wind)") != std::string::npos
                && mirror.find("Saturate(wave.pinch * 2.4f)") != std::string::npos,
                "the CPU preview mirrors the broken, bounded wind-driven crest foam");
        const std::string profiles = ReadAlongside("../EnvironmentProfiles.ini");
        {
            // The key's own length, and a section lookup that cannot be fooled by the file's header
            // comment, which mentions `[Zone.33]` while explaining what the ids mean.
            constexpr size_t keyLength = 25;   // strlen("WaterAgitationMultiplier=")
            const auto valueFor=[&](const char* section)->std::string
            {
                const size_t at = profiles.find('\n' + std::string(section));
                if (at == std::string::npos) return {};
                const size_t start = at + 1 + std::string(section).size();   // past the header line
                const size_t end = profiles.find("\n[", start);
                const std::string body = profiles.substr(start, end == std::string::npos ? end : end - start);
                const size_t key = body.find("WaterAgitationMultiplier=");
                if (key == std::string::npos) return {};
                std::string value = body.substr(key + keyLength);
                const size_t stop = value.find_first_of("\r\n");
                return value.substr(0, stop == std::string::npos ? value.size() : stop);
            };
            Require(valueFor("[Zone.33]") == "1.45", "Stranglethorn Vale is authored as an active coast");
            Require(valueFor("[Zone.12]") == ".45", "Elwynn Forest is authored as sheltered water");
            Require(valueFor("[Default]") == "1.0", "an untuned zone inherits a neutral sea state");
            Require(valueFor("[Zone.1497]") == ".3", "Undercity is nearly a mirror, being underground");
            int tuned = 0;
            for (size_t at = profiles.find("WaterAgitationMultiplier="); at != std::string::npos;
                 at = profiles.find("WaterAgitationMultiplier=", at + 1))
            {
                const float v = std::stof(profiles.substr(at + keyLength));
                tuned += v != 1.f ? 1 : 0;
                Require(v >= 0.f && v <= 4.f, "shipped per-zone sea state stays in the accepted range");
            }
            Require(tuned >= 60, "the verified zones all carry a sea state, not a handful of examples");
        }
        Require(mirror.find("* (sharpness + 2.f) * fresnel") != std::string::npos
                && mirror.find(".17f / (phaseDen * std::sqrt(phaseDen))") != std::string::npos,
                "the CPU preview carries both normalizations, so it stays a mirror");

        // ---- WATER QUALITY: one level, one ladder, two shader stages that read it ----
        // Every other water number in this tree is a LOOK. This one says how much of the look
        // is evaluated, which makes it the only water control whose value is a cost, so the
        // cost is pinned as numbers rather than described in a comment: a level is monotone
        // (a player hunting for frames only ever moves down, and never has to know what a step
        // buys), level 0 is the flat sheet, and level 6 is the sea that shipped before the
        // slider existed. The header is read as text too, because the ladder has to be written
        // down in exactly one place for both stages to mean the same thing by a level.
        const std::string ladder=stripComments(ReadAlongside("../src/Water/WaterQuality.h"));
        Require(ladder.size()>2048,"the quality ladder header is readable");
        Require(kQualityMin==0&&kQualityMax==10&&kQualityDefault==7,
                "water quality is eleven levels, 0 to 10, and ships at 7");
        for (int level=kQualityMin;level<=kQualityMax;++level) {
            const QualityPlan plan=QualityPlanFor(level);
            Require(plan.level==level,"a level in range resolves to itself");
            Require(plan.ssrSteps>=0&&plan.ssrSteps<=12&&plan.lightSteps>=0&&plan.lightSteps<=12,
                    "no level asks a ray march for more steps than the loop is written against");
            Require(plan.localLights>=0&&plan.localLights<=8,
                    "no level reflects more emitters than the local-light constant cache holds");
            Require(plan.chopOctaves>=0&&plan.chopOctaves<=3,
                    "no level shades more chop octaves than the wave field has");
            Require(plan.geometryTrains>=0&&plan.geometryTrains<=kGeometryTrainCountMax,
                    "no level displaces more trains than the swell table has");
            Require(plan.waves==(level>=kQualityWaves)&&plan.shore==(level>=kQualityShore)
                    &&plan.midWaves==(level>=kQualityMidWaves)&&plan.midGeometry==(level>=kQualityMidGeometry)
                    &&plan.foamGlow==(level>=kQualityFoamGlow),
                    "every resolved gate is its own level, so the CPU and the shader cannot disagree");
            if (level>kQualityMin) {
                const QualityPlan below=QualityPlanFor(level-1);
                Require(plan.ssrSteps>=below.ssrSteps&&plan.lightSteps>=below.lightSteps
                        &&plan.localLights>=below.localLights&&plan.chopOctaves>=below.chopOctaves
                        &&plan.geometryTrains>=below.geometryTrains,
                        "the ladder is monotone in cost: raising the level only ever adds work");
                Require(plan.waves>=below.waves&&plan.shore>=below.shore&&plan.midWaves>=below.midWaves
                        &&plan.midGeometry>=below.midGeometry&&plan.foamGlow>=below.foamGlow,
                        "the ladder is monotone in features: raising the level only ever adds detail");
            }
        }
        Require(QualityPlanFor(-9).level==kQualityMin&&QualityPlanFor(99).level==kQualityMax,
                "an absurd authored level clamps into the ladder instead of indexing past it");
        Require(std::string(QualityName(0))=="FLAT"&&std::string(QualityName(6))=="HIGH"
                &&std::string(QualityName(kQualityDefault))=="ENHANCED"&&std::string(QualityName(10))=="MAX"
                &&std::string(QualityName(99))=="MAX",
                "every level has a name a log line can print, including the clamped ones");
        {
            const QualityPlan flat=QualityPlanFor(0), shipped=QualityPlanFor(6), enhanced=QualityPlanFor(7);
            Require(IsFlat(flat)&&!flat.waves&&flat.ssrSteps==0&&flat.lightSteps==0&&flat.localLights==0
                    &&flat.chopOctaves==0&&flat.geometryTrains==0,
                    "level 0 is the flat sheet: no waves, no octaves, no marches, no emitter reflections");
            Require(!IsFlat(shipped)&&shipped.ssrSteps==8&&shipped.lightSteps==8&&shipped.localLights==8
                    &&shipped.chopOctaves==2&&shipped.geometryTrains==kGeometryTrainCount&&!shipped.midWaves
                    &&!shipped.midGeometry&&!shipped.foamGlow,
                    "level 6 is the sea this renderer shipped before the slider existed");
            Require(enhanced.chopOctaves==3&&enhanced.midWaves&&enhanced.geometryTrains==kGeometryTrainCount
                    &&!enhanced.midGeometry&&enhanced.ssrSteps==8&&enhanced.lightSteps==8,
                    "level 7 adds the mid-wave octave and the shore work at the shipped march budget");
            Require(QualityPlanFor(kQualityMax).midGeometry&&QualityPlanFor(kQualityMax).foamGlow
                    &&QualityPlanFor(kQualityMax).geometryTrains==kGeometryTrainCountMax
                    &&QualityPlanFor(kQualityMax).ssrSteps==12&&QualityPlanFor(kQualityMax).lightSteps==12,
                    "level 10 is everything: displaced mid waves, twelve-step marches, the backlit veil");
        }
        // The shore back-flow's run-up inherits WAVE HEIGHT instead of authoring a second
        // scale, which is what makes it correct in a pond and in a gale without a new number.
        Require(SwashYards(QualityPlanFor(6),3.f)==0.f,
                "no run-up below the level that has one, whatever the waves are authored to do");
        Require(SwashYards(QualityPlanFor(7),0.f)==0.f,
                "water authored flat has no crest arriving, so nothing runs up it");
        Require(std::fabs(SwashYards(QualityPlanFor(7),1.f)-1.35f)<1e-6f,
                "the run-up is the arriving crest's own height, scaled once");
        Require(SwashYards(QualityPlanFor(7),1e9f)<=2.5f+1e-6f&&SwashYards(QualityPlanFor(7),-4.f)==0.f,
                "an absurd wave height cannot drain a harbour, and a negative one cannot either");
        Require(SwashYards(QualityPlanFor(7),std::nanf("x"))==0.f,
                "a NaN wave height cannot reach the shore masks as a NaN run-up");
        // The gates reach the pixel stage as ONE number rather than as a pile of resolved
        // booleans, and the stage compares it against the midpoints this header spells -
        // `level - .5` - so a level can never sit on a boundary and a float rounding on the way
        // through a driver cannot flip a feature. The literals are derived from GateFor rather
        // than typed again here, which is the only way the two halves stay one ladder.
        const auto gateSpelling=[](int level) {
            char buffer[16];
            std::snprintf(buffer,sizeof(buffer),"%g",double(GateFor(level)));
            return std::string(buffer);
        };
        Require(gateSpelling(kQualityChop)=="1.5"&&gateSpelling(kQualityChopFine)=="3.5"
                &&gateSpelling(kQualityShore)=="3.5"&&gateSpelling(kQualityMidWaves)=="6.5"
                &&gateSpelling(kQualityFoamGlow)=="9.5",
                "the ladder's gates are spelled as midpoints, so no level sits on a boundary");
        for (int gate : {kQualityChop,kQualityChopFine,kQualityMidWaves,kQualityFoamGlow})
            Require(pixel.find("qualityLevel>="+gateSpelling(gate))!=std::string::npos,
                    "the pixel stage gates its octaves, lace and veil on the ladder's own midpoint");
        Require(pixel.find("step("+gateSpelling(kQualityShore)+",qualityLevel)")!=std::string::npos&&
                pixel.find("step("+gateSpelling(kQualityFoamGlow)+",qualityLevel)")!=std::string::npos,
                "the shore band and the contact line ask the same midpoint as a 0/1, not as a branch");
        Require(pixel.find("float qualityLevel=waveFlow.z;")!=std::string::npos&&
                code.find("controls[46]=float(qualityPlan.level)")!=std::string::npos,
                "the level rides one constant lane to the pixel stage instead of a register of booleans");
        Require(code.find("controls[17]=float(qualityPlan.ssrSteps)")!=std::string::npos&&
                code.find("controls[18]=float(qualityPlan.lightSteps)")!=std::string::npos&&
                code.find("controls[47]=wavesEnabled&&qualityPlan.waves?1.f:0.f")!=std::string::npos&&
                code.find("controls[74]=renderer::water::SwashYards(qualityPlan,geometry.amplitude)")
                    !=std::string::npos,
                "the ladder reaches the pixel stage as constants: both marches, the wave gate, the run-up");
        Require(vertex.find("float midGate=waveGroupPhase.z;")!=std::string::npos&&
                code.find("qualityPlan.midGeometry?1.f:0.f")!=std::string::npos&&
                code.find("wave.swellPhase[0],wave.swellPhase[1],wave.swellPhase[2],0.f")!=std::string::npos,
                "the mesh displaces the mid wave on a gate and gets its phase, so a low level pays for two trains");
        // A level that removes a march has to remove the REASON for the scene copy as well, or
        // it pays the full-resolution capture to feed a loop that no longer runs. That is what
        // makes level 0 cheap rather than merely simpler.
        Require(code.find("reflectionsEnabled&&qualityPlan.ssrSteps>0")!=std::string::npos&&
                code.find("sunGlintEnabled&&qualityPlan.lightSteps>0")!=std::string::npos,
                "a level with no marches stops asking for the scene copy it would not read");
        Require(code.find("unsigned count=std::min<unsigned>(unsigned(std::max(qualityPlan.localLights,0))")
                    !=std::string::npos&&
                occurrences(code,"cache.localLights")!=std::string::npos,
                "a level clamps the emitter reflections on the CPU, where the lights are transformed");
        // The slider and the ini are the two places a player and a supporter set the level, and
        // both have to agree with the header's own bounds, or the overlay offers a level the
        // renderer does not resolve.
        {
            const std::string ini=ReadAlongside("../GraphicsEffects.ini");
            Require(ini.find("Quality=7")!=std::string::npos,
                    "the shipped ini asks for the default level, so an untouched install gets level 7");
            const std::string overlay=ReadAlongside("../src/Environment/OverlayItems.h");
            Require(overlay.find("L\"Water\",L\"Quality\"")!=std::string::npos&&
                    overlay.find("WATER QUALITY (0 FLAT 6 HIGH 10 MAX)\",0,10,1,7}")!=std::string::npos,
                    "water quality is an eleven-position slider in F7, bounded and defaulted by the ladder");
            Require(ini.find("DepthFogEnabled=1")!=std::string::npos&&
                    ini.find("DepthFogPercent=200")!=std::string::npos&&
                    overlay.find("L\"Water\",L\"DepthFogEnabled\",\"DEPTH FOG\",0,1,1,1}")!=std::string::npos&&
                    overlay.find("L\"Water\",L\"DepthFogPercent\",\"DEPTH FOG AMOUNT\",100,400,5,200}")!=std::string::npos,
                    "depth fog ships on at both-legs strength in the ini and as a toggle plus slider in F7");
        }
        // And the CPU mirror resolves the SAME ladder rather than inventing its own gates, so a
        // published frame at a level is a picture of what the renderer draws at that level.
        Require(mirror.find("const QualityPlan plan = QualityPlanFor(c.quality);") != std::string::npos
                && mirror.find("const float swashYards = SwashYards(plan, geometry.amplitude);") != std::string::npos
                && mirror.find("const NoiseSample rippleMid = plan.chopOctaves >= 3") != std::string::npos
                && mirror.find("plane.x * .46f - field.ripplePhase[0] * .87f") != std::string::npos,
                "the CPU preview resolves the same ladder and mirrors the mid octave it adds");
        Require(mirror.find("const float lace = plan.midWaves") != std::string::npos
                && mirror.find("const float backwash = plan.midWaves") != std::string::npos
                && mirror.find("const float beach = 1.f - Saturate(bottomSlope * 1.6f - .32f)") != std::string::npos,
                "the CPU preview mirrors the foam lace, the backwash and the shore shape read off the bed");
    }
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr,"FAIL %s\n",e.what());
    return 1;
}
