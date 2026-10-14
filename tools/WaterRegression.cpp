// Portable policy regressions. Run from any directory; no Windows SDK needed.
#include "../src/Water/WaterPolicy.h"
#include <cstdio>
#include <stdexcept>

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
