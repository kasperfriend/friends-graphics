// Portable checks for the production software-state transaction. The D3D9
// companion test renders the real F7 menu through setter hooks on Windows.
#include "../src/D3D9/TrackedRenderState.h"
#include <cstdio>
#include <stdexcept>
#include <type_traits>

using namespace renderer;
static_assert(!std::is_copy_constructible_v<ScopedTrackedRenderState>);
static_assert(!std::is_copy_assignable_v<ScopedTrackedRenderState>);

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
bool Same(const TrackedRenderState& a, const TrackedRenderState& b)
{
    return a.currentVS == b.currentVS && a.currentPS == b.currentPS &&
        a.currentVDecl == b.currentVDecl && a.currentFVF == b.currentFVF &&
        a.vsHash == b.vsHash && a.psHash == b.psHash && a.alphaBlend == b.alphaBlend &&
        a.alphaTest == b.alphaTest && a.zWrite == b.zWrite && a.zEnable == b.zEnable;
}
void MenuSetters()
{
    // F7 + font atlas call these setters through the proxy. StateBlock::Apply
    // restores the actual device without replaying those hooks.
    g_trackedState = {};
    g_trackedState.currentFVF = 0x144;
    g_trackedState.alphaBlend = true;
    g_trackedState.zWrite = g_trackedState.zEnable = false;
}
int main() try
{
    static char vs, ps, declaration;
    const TrackedRenderState world{
        reinterpret_cast<IDirect3DVertexShader9*>(&vs),
        reinterpret_cast<IDirect3DPixelShader9*>(&ps),
        reinterpret_cast<IDirect3DVertexDeclaration9*>(&declaration),
        0, 0x279ee550dad80fecull, 0x12345678ull, true, true, false, true
    };
    g_trackedState = world;
    // Ten seconds at 60 Hz: deliberately no redundant game Set* calls between
    // frames. Reasserting world flags each frame would conceal the original bug.
    for (unsigned frame = 0; frame < 600; ++frame)
    {
        {
            ScopedTrackedRenderState saved;
            MenuSetters();
            Require(!Same(g_trackedState, world), "test must actually poison the mirror");
        }
        Require(Same(g_trackedState, world), "F7 must preserve every tracked world field");
        Require(g_trackedState.zEnable && g_trackedState.alphaBlend &&
            g_trackedState.vsHash == world.vsHash, "torch observation gates stay eligible with menu open");
    }
    std::puts("PASS repeated menu drawing preserves world state without redundant setters");
    {
        ScopedTrackedRenderState outer;
        MenuSetters();
        const auto menu = g_trackedState;
        { ScopedTrackedRenderState inner; g_trackedState = world; }
        Require(Same(g_trackedState, menu), "nested pass restores enclosing state");
    }
    Require(Same(g_trackedState, world), "outer pass restores world state");
    try { ScopedTrackedRenderState saved; MenuSetters(); throw 1; }
    catch (int) {}
    Require(Same(g_trackedState, world), "exception restores world state");
    const auto earlyReturn = [] { ScopedTrackedRenderState saved; MenuSetters(); return; };
    earlyReturn();
    Require(Same(g_trackedState, world), "early return restores world state");
    {
        ScopedTrackedRenderState saved;
        MenuSetters(); saved.Restore();
        Require(Same(g_trackedState, world), "explicit restoration works");
        g_trackedState.psHash = 42; // a subsequent legitimate game update
        saved.Restore();
    }
    Require(g_trackedState.psHash == 42, "double restore/destructor must not undo later game state");
    std::puts("PASS nested passes, exceptions, early returns and idempotent restoration");
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}
