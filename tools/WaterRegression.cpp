// Portable policy regressions. Run from any directory; no Windows SDK needed.
#include "../src/Water/WaterPolicy.h"
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
    for (uint64_t vertex : {0x206d861fd0a721ddull,
                           0xfdd9528ed3ac30eaull, 0x48a82796bd612aebull})
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
    Require(!IsMaterialLayout(8,64,256,256,0,pixel), "known PS alone cannot identify water");
    Require(!IsMaterialLayout(8,64,256,256,0x17f042a7906ca126ull,pixel), "second water PS cannot be used as VS identity");
    Require(!IsMaterialLayout(4,32,256,256), "unknown scaled materials fail closed");
    Require(!IsMaterialLayout(8,64,64,64,0xd9e7756460af6296ull,pixel), "UI shader with ripple texture rejected");
    for (unsigned bits=0; bits<32; ++bits)
        Require(NeedsScene(bits&1,bits&2,bits&4,bits&8,bits&16)==(bits!=0),
                "every depth-dependent feature requests capture independently");
    Require(TargetsMatch(1920,1080,1920,1080), "same-size mask/depth pair can be reused");
    Require(!TargetsMatch(1280,720,1920,1080), "window resize invalidates BOTH targets");
    Require(!TargetsMatch(1920,720,1920,1080), "height-only resize invalidates targets");
    Require(!TargetsMatch(1280,1080,1920,1080), "width-only resize invalidates targets");
    Require(!TargetsMatch(0,0,0,0), "uninitialized targets are never valid");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr,"FAIL %s\n",e.what());
    return 1;
}
