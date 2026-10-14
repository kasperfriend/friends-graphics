// Portable policy regressions. Run from any directory; no Windows SDK needed.
#include "../src/Water/WaterPolicy.h"
#include "../src/Water/WaveField.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <initializer_list>

using namespace renderer::water;
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
        return WrapLattice(relativeX*field.lattice[0]+relativeY*field.lattice[1]+field.phase[0]);
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
            Require(std::fabs(field.phase[0])<=kLatticeWrap&&std::fabs(field.phase[1])<=kLatticeWrap&&
                    std::fabs(field.phase[2])<=kLatticeWrap&&std::fabs(field.phase[3])<=kLatticeWrap,
                    "phases stay inside the lattice wrap at any world coordinate");
            Require(std::fabs(coordinate(field,relativeX,relativeY))<=kLatticeWrap,
                    "the lattice coordinate the pixel shader floors is always wrapped");
            Require(std::isfinite(field.lattice[0])&&std::isfinite(field.phase[0])&&
                    std::isfinite(field.uvDrift[0])&&std::isfinite(field.uvDrift[1]),
                    "wave constants stay finite after hours of animation");
        }
    }
    {
        const auto still=BuildWaveField(12.0,1.f,.32f,100.f,0.f,0.f,false,false);
        Require(still.uvDrift[0]==0.f&&still.uvDrift[1]==0.f,
                "switching waves off also stops the texture creep");
        const auto moving=BuildWaveField(12.0,1.f,.32f,100.f,0.f,0.f,false,true);
        Require(moving.phase[0]!=still.phase[0], "flow animation drives the wave field");
        const auto noCamera=BuildWaveField(12.0,1.f,.32f,100.f,0.f,0.f,false,true);
        Require(std::isfinite(noCamera.phase[0])&&std::isfinite(noCamera.phase[3]),
                "a frame without camera capture still yields a finite field");
        const float cell=kRippleCellYards*100.f*.01f;
        Require(std::fabs(std::hypot(moving.lattice[0],moving.lattice[1])-1.f/cell)<1e-5f,
                "the lattice frequency is one cell of the configured size");
        Require(std::fabs(moving.lattice[0]*moving.lattice[2]+
                          moving.lattice[1]*moving.lattice[3])<1e-5f,
                "the two lattice axes stay perpendicular");
    }
    // The vertex stage displaces the very waves the pixel shader lights, from one
    // shared number: the swell phase in lattice cells. That is only legal because
    // every train's period divides the wrap the shader applies to its lattice
    // indices - if it stopped doing so, a wrapped phase would slide the displaced
    // crest off the lit one every 64 cells, which is exactly the artefact class
    // this whole field exists to remove.
    static_assert(int(kLatticeWrap) % int(kSwellCells) == 0,
        "the long swell period must divide the lattice wrap");
    static_assert(int(kLatticeWrap) % int(kSwellShortCells) == 0,
        "the secondary swell period must divide the lattice wrap");
    Require(std::fabs(kSwellRadians*kSwellCells-6.2831853f)<1e-4f&&
            std::fabs(kSwellRadians*kSwellShortCells*2.0f-6.2831853f)<1e-4f,
            "one swell train is 2pi over its cell count, the other twice as frequent");
    // (1) Translation invariance, measured where a tile is actually visible: the
    // reference camera sits on the water it is testing, and each other camera sees
    // the same point from up to six hundred yards away.
    for (float offset : {-600.f, -133.f, 0.f, 271.f, 640.f})
    for (float counter : {0.f, .32f, 1.f})
    {
        const auto field=BuildWaveField(17.5,1.f,counter,100.f,4213.f+offset,-7781.f+offset*.5f,true,true);
        const auto reference=BuildWaveField(17.5,1.f,counter,100.f,4213.f,-7781.f,true,true);
        const float relativeX=4213.f-(4213.f+offset), relativeY=-7781.f-(-7781.f+offset*.5f);
        const auto argum=[](const WaveField& field,float relativeX,float relativeY) {
            return (relativeX*field.lattice[0]+relativeY*field.lattice[1]+field.phase[2])*kSwellRadians;
        };
        const float argX=argum(field,relativeX,relativeY);
        const float argRef=argum(reference,0.f,0.f);
        // sin() is what the shader feeds, and arguments that differ by a whole
        // number of turns are the same wave. Both sides are wrapped with floor (NOT
        // by taking an absolute value first: |a| and |b| are not congruent when the
        // signs differ, which is how an earlier version of this check invented a
        // failure), and compared circularly.
        const auto turn=[](float value){return value-6.2831853f*std::floor(value/6.2831853f);};
        const float waveA=turn(argX),waveB=turn(argRef);
        const float apart=std::fabs(waveA-waveB);
        Require(std::fmin(apart,6.2831853f-apart)<1e-2f,
                "the shared swell phase lands the same wave under every camera");
    }
    // (2) And the argument the vertex stage evaluates stays inside a range where
    // float32 still has fraction bits to spare even at the far corner of a
    // continent, because the camera's own coordinate is subtracted first.
    {
        const auto far=BuildWaveField(41.25,1.f,.32f,100.f,46321.f,-51000.f,true,true);
        const float arg=std::fabs((421.f*far.lattice[0]-307.f*far.lattice[1]+far.phase[2])*kSwellRadians);
        Require(arg<160.f,"the geometry wave argument stays small at any world coordinate");
        const float ripple=std::fabs(421.f*far.lattice[0]-307.f*far.lattice[1]+far.phase[0]);
        Require(ripple<=kLatticeWrap*2.f,"the shading argument never leaves the wrapped lattice twice over");
        Require(std::isfinite(arg)&&std::isfinite(far.phase[3]),
                "the swell phase is finite for hours of flow and any camera");
    }
    {
        const auto blind=BuildWaveField(9.0,1.f,.32f,100.f,12000.f,9000.f,false,true);
        const auto origin=BuildWaveField(9.0,1.f,.32f,100.f,0.f,0.f,true,true);
        // A frame without a captured camera cannot be nailed to the terrain, so the
        // field degrades to exactly what a camera at the origin would see: still
        // animated, still bounded, no reliance on a position it does not have.
        Require(blind.phase[0]==origin.phase[0]&&blind.phase[3]==origin.phase[3],
                "a missing camera falls back to the origin-anchored field, not to garbage");
        Require(std::isfinite(blind.lattice[0])&&std::isfinite(blind.phase[2]),
                "the anchor fallback stays usable by both shader stages");
    }
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr,"FAIL %s\n",e.what());
    return 1;
}
