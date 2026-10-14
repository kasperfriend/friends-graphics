// Portable policy regressions. Run from any directory; no Windows SDK needed.
#include "../src/Water/WaterPolicy.h"
#include "../src/Water/WaveField.h"
#include "../src/Water/WaterNoise.h"
#include "../src/Water/WaterSeaState.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
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

int main() try
{
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
            Require(std::fabs(field.ripplePhase[0])<=kLatticeWrap&&
                    std::fabs(field.ripplePhase[1])<=kLatticeWrap&&
                    std::fabs(field.warpPhase[0])<=kLatticeWrap&&
                    std::fabs(field.warpPhase[1])<=kLatticeWrap,
                    "chop and domain-warp phases stay inside their own noise wraps");
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
    // coordinate the vertex stage computes from its own matrix, and the camera's world
    // position the CPU folds into the phase. Read one half from a frame-captured camera
    // and the sea slides by exactly however far the player moved since that capture - the
    // capture happens on the first draw of the frame that carries the perspective
    // constants, so water drawn before it is anchored to LAST frame's camera, and the
    // client's own reflection pass renders water under a mirrored camera the capture never
    // sees at all. Both halves therefore come from c5..c8 of the draw being shaded.
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
    static_assert(kTrainCount==3,"the swell is three trains: the two the mesh can hold, and one the shading only");
    static_assert(kGeometryTrainCount==2,"the vertex stage carries the two longest trains only");
    for (int train=0;train<kTrainCount;++train)
        Require(std::fabs(kTrainRadians[train]*kTrainCells[train]-kTwoPi)<1e-4f&&
                std::fabs(kTrainDirX[train]*kTrainDirX[train]+kTrainDirY[train]*kTrainDirY[train]-1.f)<1e-4f,
                "every swell train is 2pi over its own cell count, in a unit direction");
    Require(kMaxPinch>1.1f&&kMaxPinch<1.3f,"the train table's steepness budget is the one the clamp assumes");
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
        Require(waves.sigma<=kGerstnerSigmaMax&&waves.sigma*kMaxPinch<1.f,
                "the Gerstner steepness stays below the fold, whatever is authored");
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
        Require(kGerstnerSigma*kMaxPinch<.7f,"a fully pinched crest still leaves most of its Jacobian");
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
        Require(shader.find("const double elapsed=seaMotion_.Time();") != std::string::npos
                && shader.find("BuildWaveField(elapsed,1.f,") != std::string::npos,
                "the wave field runs on the integrated clock, and speed is not re-multiplied into it");

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
        // The whole file with its comments off, for the pins that assert an expression is
        // NOT there: both sources explain what the lattice orientation used to be by name.
        // The header is read too, because the wave model itself - the train table, the
        // dispersion, the orientation - lives there and is spelled into both stages.
        const std::string code=stripComments(shader);
        const std::string header=stripComments(ReadAlongside("../src/Water/WaveField.h"));
        Require(header.size()>4096,"the wave field header both stages mirror is readable");
        Require(vertex.size()>1024&&pixel.size()>8192,"both water stages are readable as text");
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
        Require(code.find("GetVertexShaderConstantF(5,&modelView[0][0],4)")!=std::string::npos&&
                code.find("AnchorFromModelView(modelView)")!=std::string::npos&&
                code.find("anchor.camera[0],anchor.camera[1],anchor.valid")!=std::string::npos,
                "the wave field is anchored to the matrix of the draw being shaded");
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
        Require(vertex.find("float2 warpedDirA=kDirA+3.2*dot(kDirA,float2(.6,.8))*warpGradient;")
                    !=std::string::npos&&
                vertex.find("float2 slide=waveControl.y*(kWeightA*cosA*warpedDirA+kWeightB*cosB*warpedDirB);")
                    !=std::string::npos,
                "the Gerstner slide follows the domain-warped crest with its chain-rule direction");
        Require(pixel.find("swellSlope=(swellSlope+dot(swellSlope,float2(.6,.8))*warpGradient*3.2)*foreverStyle.x;")
                    !=std::string::npos,
                "pixel normals apply the full chain rule through the same crest warp");
        Require(vertex.find("waveWarpPhase:register(c203)")!=std::string::npos&&
                pixel.find("waveWarpPhase:register(c217)")!=std::string::npos&&
                code.find("SetPixelShaderConstantF(200,controls,18)")!=std::string::npos&&
                code.find("SetVertexShaderConstantF(200,vertexControls,4)")!=std::string::npos,
                "both shader stages upload the independent camera-anchored warp phase");
        Require(pixel.find("float2 tilt=-slopes*(controls.z/jacobian)*flip;")!=std::string::npos,
                "a crest leans toward the light it catches: the normal is the negative gradient");
        Require(pixel.find("float jacobian=max(1.0-pinch,.3);")!=std::string::npos&&
                pixel.find("float pinch=controls.x*(kWgtA*kRadA*sinA+kWgtB*kRadB*sinB+kWgtC*kRadC*sinC);")
                    !=std::string::npos,
                "the shading sharpens by the Gerstner Jacobian, and never divides by zero");
        // One train table, three spellings: the vertex stage, the pixel stage, and the CPU
        // header that builds the phases. A wavelength that moves in one and not the others
        // slides a lit crest off a displaced one, so the spellings are held to each other.
        // The two longest trains are spelled into both stages; the ten yard one is the pixel
        // stage's alone, because the client's liquid grid is about four yards apart and
        // lifting a wave that short on it is what facets. That split is deliberate, so it is
        // pinned in both directions rather than left to a reader to notice.
        for (const char* spelling : {"0.39269908","0.78539816","-0.4695,0.8829"})
            Require(vertex.find(spelling)!=std::string::npos&&pixel.find(spelling)!=std::string::npos,
                    "both shader stages carry the same swell train table");
        for (const char* spelling : {"1.57079633","0.4384,-0.8988","0.28409091"})
            Require(pixel.find(spelling)!=std::string::npos&&vertex.find(spelling)==std::string::npos,
                    "the ten yard train and its crest normalization are the pixel stage's alone");
        Require(header.find("kTrainCells[kTrainCount] = {16.f, 8.f, 4.f}")!=std::string::npos&&
                header.find("kTrainDirX[kTrainCount] = {1.f, -.4695f, .4384f}")!=std::string::npos&&
                header.find("kTrainDirY[kTrainCount] = {0.f, .8829f, -.8988f}")!=std::string::npos&&
                header.find("kTrainRadians[kTrainCount] = {kTwoPi / 16.f, kTwoPi / 8.f, kTwoPi / 4.f}")
                    !=std::string::npos,
                "the CPU header carries the same wavelengths and directions");
        // Foam at the edges of waves, and only when the water is active: the pinch says a
        // crest is breaking, the authored wind says whether this body can foam at all, and
        // the foam then takes the mirror and the sun road with it - a whitecap scatters.
        Require(pixel.find("float foamWind=saturate(wind);")!=std::string::npos&&
                pixel.find("float breaking=saturate(pinch*2.4);")!=std::string::npos&&
                pixel.find("float foamPattern=saturate(.45*waveWarp.x+.55*detail.x);")!=std::string::npos&&
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
        // shading: the sea state multiplies the lift, and waves off is flat water.
        Require(code.find("BuildGeometryWaves(geometryWavePercent,\n"
                            "                waveScalePercent,seaAgitation,geometryWavesEnabled,wavesEnabled)")
                    !=std::string::npos,
                "the geometry waves are lifted by the same sea state the shading is");
        const std::string mirror = ReadAlongside("WaterPreview.cpp");
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
        Require(mirror.find("kWaveWarpAmplitude * warp.gradientX") != std::string::npos
                && mirror.find("slideAlongWarp*kWaveWarpAmplitude*warp.gradientY") != std::string::npos,
                "the CPU preview uses the shader's warp gradient for normals and vertex slides");
        Require(mirror.find("-(axisX.x * w.slopes.x * tilt") != std::string::npos
                && mirror.find("swell * kSwellToUnit") != std::string::npos,
                "the CPU preview tilts the same way and normalizes the swell by the same divisor");
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
    }
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr,"FAIL %s\n",e.what());
    return 1;
}
