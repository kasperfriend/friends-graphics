#pragma once
// The grass a bone palette cannot bend: the client's own detail-doodad vertex
// shader, rewritten once and swapped in.
//
// See src/Environment/GrassShaderWind.h for what is injected and why. This is
// the device half: which shaders the client draws its grass with, what the swap
// costs, and where the constants come from.
//
//   * A shader is BENT when the client draws unskinned foliage with it, its own
//     disassembly can take the motion block, and the reassembled shader compiles
//     on the device. Every draw after that is wind, for the rest of the session -
//     one injection per shader, not per draw.
//   * The swap happens inside the SetVertexShader hook this project already
//     owns: when the client binds a shader this module has bent, the device is
//     given the bent one instead. Nothing else about the draw changes.
//   * The constants are written from the draw hook, directly before the client's
//     own draw call, so they are the last word for that draw. In practice that is
//     once per frame, and again whenever the client writes over the six registers
//     that shader's block lives in - which is what the SetVertexShaderConstantF
//     hook is there to notice.
//   * Those six registers are chosen for the shader, not for the module: c14..c19
//     where the shader never touches them (the engine's own per-chunk upload fills
//     c0..c13 and leaves the rest alone), and the first free run above them where
//     it does. A real session's foliage shaders read c17 and c18 as their own
//     lighting, and writing the wind over those would have had the grass read the
//     wind as its light.
//
// Nothing here is fatal. A missing d3dcompiler, a shader that never lands its
// vertex in r0, a declaration whose texture coordinate is not where the shader
// reads it, a scratch register the shader still needs across the insertion point,
// an assembler that refuses the block: each one refuses THAT shader, says so once
// in the renderer log in its own words, and leaves the client drawing exactly
// what it drew before.
#include <d3d9.h>
#include <d3dcommon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl/client.h>
#include "../Core/FrameContext.h"
#include "../Core/M2DrawGeometry.h"
#include "../Core/TextureCutout.h"
#include "../Environment/GrassShaderWind.h"
#include "../Environment/WindDebug.h"

namespace grassblade
{
// [Vegetation] GrassBlades and the F11 graphics switch, like every other
// effect's pair of flags.
inline bool enabled = true;
inline bool active = true;
inline renderer::GrassBladeSettings settings;
inline void (*logLine)(const char*) = nullptr;
inline void Log(const std::string& text) { if (logLine) logLine(text.c_str()); }

// The client draws its whole world with a handful of vertex shaders, and only a
// few of them can be grass. The cap is where this module stops learning - it
// never evicts, because an entry holds the only reference that keeps a live
// shader's identity (and therefore the map's key) stable.
inline constexpr unsigned kMaxBladeShaders = 64;
inline constexpr unsigned kMaxBlobBytes = 64u * 1024u;
inline constexpr unsigned kMaxVertexReadBytes = 64u * 1024u;
// How many vertices of a grass draw are sampled to measure which way its blade's
// texture runs. A blade card is two to four of them, so this is several cards.
inline constexpr unsigned kBladeVertexSamples = 24;
// How many times a shader whose material did not look like foliage is looked at
// again before the answer is taken as final. A draw's material can be read wrong
// once - a state the client set through a cached state block, a texture still
// uploading - while a shader's own structure cannot change.
inline constexpr unsigned kMaterialRetries = 3;
// How many draws of one shader are measured before a card that is not a blade is
// taken as the shader's answer. A shader is bent once for ALL of its draws, so
// the measurement has to be about the shader rather than about the one draw that
// happened to arrive first - and a chunk of grass is drawn as several cards, some
// of them flat decals that carry the same material. Until a card measures as a
// blade nothing is bent, which is the safe side of the same question.
//
// The count is generous on purpose. A shader's draws are a family, not one thing:
// a client that draws its blades with the same shader that draws a wide flat quad
// of the same material (it does) reaches this branch with a big batch first, and a
// verdict taken from six of those is a field that never sways. What bounds the
// cost is that a measurement happens only while a shader is undecided, and a
// shader is decided once.
inline constexpr unsigned kBladeCardTries = 16;

// One shader the client draws its world with, and what this build decided about
// it. `original` is a live reference held on purpose: the entry is keyed by its
// address, and a released shader's address could be handed to a different one.
struct BladeShader
{
    Microsoft::WRL::ComPtr<IDirect3DVertexShader9> original;
    Microsoft::WRL::ComPtr<IDirect3DVertexShader9> injected; // null until one is built
    uint64_t hash = 0;
    unsigned materialTries = 0; // draws whose material looked like foliage
    bool structureRefused = false; // no anchor, temps in the way, no texture coordinate
    int bladeVSign = 0;        // measured from the vertices; 0 = the reference convention
    float bladeExtentYards = 0.f; // how tall the draw's own vertices were
    unsigned bladePairs = 0;   // and how many of them looked like a base and its tip
    // Which ramp this block reads: the texture's own v (false), or - for the family
    // whose declaration carries no texture coordinate at all - the vertex's own
    // height (true). The block was built for one of the two, so the frame's rows
    // have to be packed for the same one: `rampScale`/`rampOffset` are the card's
    // own floor and height, which is what turns the stored z into a 0..1 weight.
    bool rampByHeight = false;
    float rampScale = 1.f;
    float rampOffset = 0.f;
    bool cardReported = false; // the one-shot line for a card whose vertices could not be read
    bool bigReported = false;  // the one-shot line for a batch too big to be a blade card
    unsigned cardTries = 0;    // cards of this shader measured and found not to be a blade
    bool worldShaped = false;  // its constants bind the world's projection: recorded, not refused
    bool worldReported = false;// the one-shot line for that shape
    bool worldSpace = false;   // the block it got reads r0 as the WORLD position (see the block's own note)
    float greenPercent = -1.f; // how much of the card's texture is green, when that was readable
    // Where this shader's block lives. c14..c19 is the reference module's free
    // block and what most shaders take, but a shader that reads those registers
    // itself gets the first run of six above them that it never mentions - the
    // blocks of a real session landed at c14 and at c34, because the lighting
    // rows c17/c18 read by that session's foliage cannot be written over.
    unsigned firstRegister = renderer::kGrassBladeFirstRegister;
    // The block this shader is drawn with: the frame's own rows, with the last row
    // taken from this shader's measured blade direction.
    float rows[renderer::kGrassBladeRows][4]{};
    bool rowsReady = false;
};

inline std::unordered_map<const void*, BladeShader> g_blades;
inline renderer::GrassBladeStatus g_status;
inline renderer::GrassBladeConstants g_constants;
inline bool g_constantsReady = false;
// The upload bookkeeping: one write per frame per shader is enough, and a client
// write over the block re-arms it.
inline bool g_dirty = true;
inline bool g_uploading = false;
inline uint64_t g_uploadedFrame = ~0ull;
inline const void* g_uploadedShader = nullptr;
inline float g_phasePrimary = 0.f, g_phaseCross = 0.f;
inline bool g_reportedCompiler = false;
// The family that carries no texture coordinate: reported once per shader, up to
// a few, because a report's first question is what those vertices DO carry - the
// declaration and the texture verdict answer it without the game being run again.
inline unsigned g_noCoordinateReports = 0;
inline constexpr unsigned kNoCoordinateReports = 3;
inline std::unordered_set<const void*> g_noCoordinateReported;
inline bool g_reportedInjection = false;
inline bool g_reportedRefusal = false;

inline renderer::GrassBladeStatus& Status() noexcept { return g_status; }

// The disassembler and the assembler, out of d3dcompiler. Both are resolved by
// name: D3DAssemble has no declaration in the public headers, and the module the
// reference implementation loads is the one already on a modern client's disk.
// A machine without it loses the blade wind and keeps everything else.
using GrassBladeDisassembleFn = HRESULT(WINAPI*)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);
using GrassBladeAssembleFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, UINT,
    ID3DBlob**, ID3DBlob**);

inline GrassBladeDisassembleFn g_disassemble = nullptr;
inline GrassBladeAssembleFn g_assemble = nullptr;
inline bool g_compilerChecked = false;

inline bool Compiler() noexcept
{
    if (g_compilerChecked) return g_disassemble != nullptr && g_assemble != nullptr;
    g_compilerChecked = true;
    static const char* kModules[] = { "d3dcompiler_47.dll", "d3dcompiler_43.dll" };
    for (const char* name : kModules)
    {
        HMODULE module = GetModuleHandleA(name);
        if (!module) module = LoadLibraryA(name);
        if (!module) continue;
        auto disassemble = reinterpret_cast<GrassBladeDisassembleFn>(GetProcAddress(module, "D3DDisassemble"));
        auto assemble = reinterpret_cast<GrassBladeAssembleFn>(GetProcAddress(module, "D3DAssemble"));
        if (!disassemble || !assemble) continue;
        g_disassemble = disassemble;
        g_assemble = assemble;
        break;
    }
    if (!g_disassemble || !g_assemble)
    {
        Log("Grass blade sway: d3dcompiler (D3DDisassemble/D3DAssemble) is not available on this machine, so the "
            "client's blade shaders cannot be rewritten and the grass the client bakes into its own meshes stays "
            "still. Every model-based plant still sways.");
        return false;
    }
    return true;
}

// The shader's own assembly text, or empty. One call per shader, ever.
inline std::string DisassembleShader(IDirect3DVertexShader9* shader, uint32_t& bytes) noexcept
{
    bytes = 0;
    if (!shader || !Compiler()) return {};
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || !size || size > kMaxBlobBytes) return {};
    std::vector<uint8_t> code(size);
    if (FAILED(shader->GetFunction(code.data(), &size))) return {};
    bytes = size;
    ID3DBlob* blob = nullptr;
    if (FAILED(g_disassemble(code.data(), size, 0, nullptr, &blob)) || !blob) return {};
    std::string text(static_cast<const char*>(blob->GetBufferPointer()),
        static_cast<const char*>(blob->GetBufferPointer()) + blob->GetBufferSize());
    blob->Release();
    return text;
}

// What the draw's own vertices say about the card it carries. They are read
// through the draw's own index buffer (or straight out of the stream for a
// non-indexed draw), their texture coordinate and their height recorded, and
// then measured the way the portable half does it (see MeasureGrassBladeCard in
// GrassShaderWind.h): which way up the texture runs, how tall the card is, and
// how many pairs of vertices looked like one blade's base and its tip. The sign
// is what the injected block is told; the other two are what refuses a shader
// being rewritten for a card that is not a blade - a decal or a shadow blob on
// the ground has no height for a bend weight to run along, and a block whose
// weight is the same everywhere translates the card instead of bending it.
inline renderer::GrassBladeCard MeasureBladeCard(IDirect3DDevice9* device, const renderer::M2VertexLayout& layout,
    const renderer::M2DrawCall& draw, bool rampByHeight) noexcept
{
    using Microsoft::WRL::ComPtr;
    renderer::GrassBladeCard card;
    // A card bent along its texture's v needs that coordinate to be two floats; a
    // card bent along its own height needs the height, which is the position it
    // already has. One of the two has to be there, and which one is the caller's
    // own decision - the same one the shader's block was built from, because a
    // block that reads a lane the draw never fed bends nothing.
    if (!device || !layout.position) return card;
    if (!rampByHeight && (!layout.texcoord || !layout.texcoordIsFloat2)) return card;
    // The one place the portable numbers meet D3D9's own: a mismatch is a compile
    // error rather than a silent mis-sized batch. (Cast, because a /W4 build reads
    // a bare enum/int comparison as a signed/unsigned mismatch.)
    static_assert(renderer::kGrassBladeTriangleListPrimitive == static_cast<unsigned>(D3DPT_TRIANGLELIST) &&
        renderer::kGrassBladeTriangleStripPrimitive == static_cast<unsigned>(D3DPT_TRIANGLESTRIP),
        "the portable primitive numbers are D3D9's own");
    const UINT count = renderer::GrassBladeDrawVertices(draw.primitives, draw.type);
    if (count < 4) return card;
    ComPtr<IDirect3DVertexBuffer9> stream;
    UINT offset = 0, stride = 0;
    if (FAILED(device->GetStreamSource(0, stream.GetAddressOf(), &offset, &stride)) || !stream) return card;
    // The elements have to fit inside one vertex: a declaration whose texture
    // coordinate ends exactly on the stride's last byte is a card the client
    // packs tight, and a `<=` here is what refused every one of them (measured,
    // in a real session: the blade path examined five foliage shaders and read
    // 0.0000 yd of card out of all of them).
    if (stride < layout.positionOffset + 12u) return card;
    if (!rampByHeight && stride < layout.texcoordOffset + 8u) return card;
    D3DVERTEXBUFFER_DESC streamDesc{};
    if (FAILED(stream->GetDesc(&streamDesc))) return card;

    // The vertices the draw consumes, and the whole range they span: a lock has
    // to be one span, so a draw that reads the far ends of a streaming buffer is
    // read in one piece or not at all.
    ComPtr<IDirect3DIndexBuffer9> indicesBuffer;
    void* lockedIndices = nullptr;
    D3DFORMAT indexFormat = draw.indexFormat;
    UINT low = ~0u, high = 0;
    if (draw.indexed && !draw.indicesUP)
    {
        if (FAILED(device->GetIndices(indicesBuffer.GetAddressOf())) || !indicesBuffer) return card;
        D3DINDEXBUFFER_DESC indexDesc{};
        if (FAILED(indicesBuffer->GetDesc(&indexDesc))) return card;
        indexFormat = indexDesc.Format;
        const unsigned indexSize = indexFormat == D3DFMT_INDEX16 ? 2u : 4u;
        const uint64_t range = uint64_t(count) * indexSize;
        if (uint64_t(draw.start) * indexSize + range > indexDesc.Size || range > kMaxVertexReadBytes) return card;
        if (FAILED(indicesBuffer->Lock(uint64_t(draw.start) * indexSize, range, &lockedIndices, D3DLOCK_READONLY)) ||
            !lockedIndices)
            return card;
        const void* elements = lockedIndices;
        for (UINT i = 0; i < count; ++i)
        {
            const int64_t element = indexFormat == D3DFMT_INDEX16 ? int64_t(static_cast<const WORD*>(elements)[i])
                : int64_t(static_cast<const DWORD*>(elements)[i]);
            const int64_t vertex = int64_t(draw.base) + element;
            if (vertex < 0 || uint64_t(offset) + uint64_t(vertex + 1) * stride > streamDesc.Size) break;
            low = low == ~0u ? UINT(vertex) : (std::min)(low, UINT(vertex));
            high = (std::max)(high, UINT(vertex));
        }
    }
    else if (!draw.indexed)
    {
        low = draw.start;
        high = draw.start + count - 1;
    }
    else
    {
        return card; // indices from a UP pointer: nothing here needs that case
    }
    if (low == ~0u || high < low || (uint64_t(high - low + 1) * stride) > kMaxVertexReadBytes ||
        uint64_t(offset) + uint64_t(high + 1) * stride > streamDesc.Size)
    {
        if (lockedIndices) indicesBuffer->Unlock();
        return card;
    }

    void* locked = nullptr;
    if (FAILED(stream->Lock(uint64_t(offset) + uint64_t(low) * stride, uint64_t(high - low + 1) * stride, &locked,
            D3DLOCK_READONLY)) || !locked)
    {
        if (lockedIndices) indicesBuffer->Unlock();
        return card;
    }
    const unsigned char* bytes = static_cast<const unsigned char*>(locked);

    float samples[kBladeVertexSamples][3]{};
    float heights[kBladeVertexSamples]{};
    unsigned taken = 0;
    float lowestX = 0.f, highestX = 0.f, lowestY = 0.f, highestY = 0.f;
    bool haveGround = false;
    const UINT span = high - low + 1;
    const UINT wanted = (std::min)(kBladeVertexSamples, span);
    for (UINT s = 0; s < wanted; ++s)
    {
        UINT vertex = low;
        if (draw.indexed && lockedIndices)
        {
            const UINT element = wanted > 1 ? s * (count - 1) / (wanted - 1) : 0;
            const int64_t value = indexFormat == D3DFMT_INDEX16
                ? int64_t(static_cast<const WORD*>(lockedIndices)[element])
                : int64_t(static_cast<const DWORD*>(lockedIndices)[element]);
            const int64_t resolved = int64_t(draw.base) + value;
            if (resolved < int64_t(low) || resolved > int64_t(high)) continue;
            vertex = UINT(resolved);
        }
        else
        {
            vertex = wanted > 1 ? low + s * (span - 1) / (wanted - 1) : low;
        }
        const unsigned char* at = bytes + uint64_t(vertex - low) * stride;
        float position[3]{};
        std::memcpy(position, at + layout.positionOffset, sizeof(position));
        if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
            continue;
        // The patch the batch stands on. This is the one measurement that tells a
        // blade card apart from a piece of the world drawn through the same path -
        // a terrain tile's material is every bit as much of a cut-out atlas as a
        // blade's - and it is read off the same vertices the blade is, so it costs
        // nothing. x and y are the ground plane here: WoW's world is Z-up.
        if (!haveGround)
        {
            haveGround = true;
            lowestX = highestX = position[0];
            lowestY = highestY = position[1];
        }
        lowestX = (std::min)(lowestX, position[0]);
        highestX = (std::max)(highestX, position[0]);
        lowestY = (std::min)(lowestY, position[1]);
        highestY = (std::max)(highestY, position[1]);
        if (rampByHeight)
        {
            // The card's own stored height, which is what its shader reads into the
            // register the block takes its weight from. No floor is subtracted here:
            // the block is given the floor instead, because it works on the raw value.
            heights[taken] = position[2]; // WoW's world is Z-up, and a blade grows up
            ++taken;
            continue;
        }
        float uv[2]{};
        std::memcpy(uv, at + layout.texcoordOffset, sizeof(uv));
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) continue;
        samples[taken][0] = uv[0];
        samples[taken][1] = uv[1];
        samples[taken][2] = position[2]; // WoW's world is Z-up, and a blade grows up
        ++taken;
    }
    stream->Unlock();
    if (lockedIndices) indicesBuffer->Unlock();
    if (taken < 4) return card;
    // The batch's own shape, which is what the admission test judges first: how
    // many vertices this draw carries and how far they span in the ground plane.
    // A blade card is a handful of vertices on a patch a couple of feet across; a
    // terrain tile is thousands of vertices spanning the world.
    card.vertices = span;
    if (haveGround)
        card.footprintYards = (std::max)(highestX - lowestX, highestY - lowestY);
    // A card with no texture coordinate is measured by its height - the same
    // vertices, the other ramp - and the card carries the floor the block needs.
    if (rampByHeight)
    {
        const renderer::GrassBladeCard shaped = renderer::MeasureGrassBladeCardByHeight(heights, taken);
        card.measured = shaped.measured;
        card.baseYards = shaped.baseYards;
        card.extentYards = shaped.extentYards;
        card.sign = shaped.sign;
        card.pairs = shaped.pairs;
        card.vertices = span;
        return card;
    }

    // Heights relative to the lowest sampled vertex, so the vote is about the
    // blades rather than about the slope the whole patch sits on.
    float lowest = samples[0][2];
    for (unsigned i = 1; i < taken; ++i) lowest = (std::min)(lowest, samples[i][2]);
    for (unsigned i = 0; i < taken; ++i) samples[i][2] -= lowest;
    const renderer::GrassBladeCard shaped = renderer::MeasureGrassBladeCard(samples, taken);
    card.measured = shaped.measured;
    card.sign = shaped.sign;
    card.extentYards = shaped.extentYards;
    card.pairs = shaped.pairs;
    card.vertices = span;
    return card;
}

// Build the bent shader for `shader` and put it in the entry. Every refusal is
// recorded on the entry (and in the log, once) so the same shader is never
// examined twice.
inline void BuildInjectedShader(IDirect3DDevice9* device, BladeShader& entry) noexcept
{
    // The shader's own bytecode, once: its first dword is the model the text has
    // to be reassembled against, and the disassembly needs the rest of it.
    UINT size = 0;
    std::vector<uint8_t> code;
    if (FAILED(entry.original->GetFunction(nullptr, &size)) || !size || size > kMaxBlobBytes)
    {
        entry.structureRefused = true;
        ++g_status.refused;
        return;
    }
    code.resize(size);
    if (FAILED(entry.original->GetFunction(code.data(), &size)))
    {
        entry.structureRefused = true;
        ++g_status.refused;
        return;
    }
    const char* model = renderer::GrassBladeShaderModel(code.data(), size);
    if (!model[0])
    {
        entry.structureRefused = true;
        ++g_status.refused;
        Log("Grass blade sway: a shader the client draws unskinned foliage with is neither vs_2_0 nor vs_3_0, so this "
            "build has no business rewriting it.");
        return;
    }

    uint32_t bytes = 0;
    const std::string disassembly = DisassembleShader(entry.original.Get(), bytes);
    const renderer::GrassBladeInjection injection =
        renderer::BuildGrassBladeInjection(disassembly, model, entry.rampByHeight);
    if (!injection.ok)
    {
        entry.structureRefused = true;
        ++g_status.refused;
        // The whole disassembly goes to the log for this one: a shader that IS
        // the client's foliage and still cannot be bent is the report this
        // feature exists to answer, and the answer is in the text.
        Log(std::string("Grass blade sway: a shader the client draws unskinned foliage with (") + model + ", " +
            std::to_string(bytes) + " bytes) cannot take the motion block: " + injection.reason +
            ". Its own disassembly follows, once, so the next build can be written against it.");
        Log(disassembly);
        return;
    }

    ID3DBlob* assembled = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(g_assemble(injection.text.data(), injection.text.size(), nullptr, nullptr, nullptr, 0, &assembled,
            &errors)) || !assembled)
    {
        entry.structureRefused = true;
        ++g_status.refused;
        std::string detail = "the assembler refused the block";
        if (errors && errors->GetBufferPointer() && errors->GetBufferSize())
            detail += std::string(": ") + std::string(static_cast<const char*>(errors->GetBufferPointer()));
        if (errors) errors->Release();
        Log("Grass blade sway: a shader the client draws unskinned foliage with (" + std::string(model) +
            ") could not be reassembled - " + detail + ". The grass stays as the client drew it.");
        return;
    }
    Microsoft::WRL::ComPtr<IDirect3DVertexShader9> injected;
    const HRESULT created = device->CreateVertexShader(
        static_cast<const DWORD*>(assembled->GetBufferPointer()), injected.GetAddressOf());
    assembled->Release();
    if (FAILED(created) || !injected)
    {
        entry.structureRefused = true;
        ++g_status.refused;
        char text[256]{};
        std::snprintf(text, sizeof(text),
            "Grass blade sway: the device refused the reassembled blade shader (HRESULT=0x%08lx). The grass stays as "
            "the client drew it.", static_cast<unsigned long>(created));
        Log(text);
        return;
    }
    entry.injected = injected;
    entry.worldSpace = injection.worldSpace;
    entry.firstRegister = injection.firstRegister;
    ++g_status.injected;
    if (!g_reportedInjection)
    {
        g_reportedInjection = true;
        char text[560]{};
        std::snprintf(text, sizeof(text),
            "Grass blade sway: the client draws its grass with a vertex shader this build has now bent - %s, %u "
            "bytes, texture coordinate v%u, motion constants c%u..c%u, temps r%u..r%u, blade upright %s (%.2f yd "
            "of it, %u pair(s) voted), card %.0f%% green. Every draw of it now moves with the wind; the HUD counts "
            "them as `blades`.",
            model, bytes, injection.texcoordRegister, injection.firstRegister,
            injection.firstRegister + renderer::kGrassBladeRows - 1,
            injection.firstScratch, injection.firstScratch + renderer::kGrassBladeScratchCount - 1,
            entry.bladeVSign > 0 ? "the other way up" : "as the reference client has it",
            double(entry.bladeExtentYards), entry.bladePairs,
            double(entry.greenPercent < 0.f ? 0.f : entry.greenPercent * 100.f));
        Log(text);
    }
}

// The entry for a shader the device is drawing with - the client's own copy of a
// shader this build has bent, or the bent one itself. Both have to find the same
// entry: the SetVertexShader hook swaps the pointer on the way in, so every
// later read of "what is bound" returns the BENT object, and without this a
// bent shader would look like a new discovery on the next draw - it would be
// examined again, refused a second time for a structure this build has already
// answered, and logged as a refusal that means nothing.
inline BladeShader* FindBladeShader(IDirect3DVertexShader9* shader) noexcept
{
    if (!shader) return nullptr;
    auto found = g_blades.find(static_cast<const void*>(shader));
    if (found != g_blades.end()) return &found->second;
    for (auto& entry : g_blades)
        if (entry.second.injected.Get() == shader) return &entry.second;
    return nullptr;
}

// The client is binding `shader`: give it the bent one instead, when there is
// one. Called from the SetVertexShader hook before this project's own state
// tracking, so the mirror and the device agree on what is bound.
inline void Substitute(IDirect3DVertexShader9** shader) noexcept
{
    if (!shader || !*shader) return;
    if (!enabled || !active || !settings.enabled) return;
    auto found = g_blades.find(static_cast<const void*>(*shader));
    if (found == g_blades.end() || !found->second.injected) return;
    *shader = found->second.injected.Get();
}

// The client wrote vertex shader constants. Anything that touches the block this
// module owns re-arms the upload, so the block is rewritten before the next draw.
inline void NoteConstantWrite(UINT start, UINT count) noexcept
{
    if (g_uploading) return;
    // Every injected shader's own range, not one fixed block: a client write over
    // ANY of them re-arms the upload, which is what keeps a shader from running
    // with rows the client has since overwritten.
    const UINT end = start + count;
    for (const auto& shader : g_blades)
    {
        const BladeShader& entry = shader.second;
        if (!entry.injected) continue;
        if (start < entry.firstRegister + renderer::kGrassBladeRows && end > entry.firstRegister)
        {
            g_dirty = true;
            return;
        }
    }
}

// One block per frame per shader: a frame's constants do not change between two
// draws of the same shader, and a client write over them re-arms the write.
inline void Upload(IDirect3DDevice9* device, uint64_t frameIndex, const BladeShader& entry) noexcept
{
    if (!entry.rowsReady || !entry.injected) return;
    if (!g_dirty && g_uploadedFrame == frameIndex && g_uploadedShader == entry.injected.Get()) return;
    g_uploading = true;
    device->SetVertexShaderConstantF(entry.firstRegister, entry.rows[0], renderer::kGrassBladeRows);
    g_uploading = false;
    g_dirty = false;
    g_uploadedFrame = frameIndex;
    g_uploadedShader = entry.injected.Get();
    ++g_status.uploads;
}

inline void Configure(const renderer::GrassBladeSettings& next) noexcept { settings = next; }
inline void BeginFrame() noexcept
{
    g_status.bladeDraws = 0;
    g_status.uploads = 0;
    g_status.foliageDraws = 0;
    g_status.materialRefused = 0;
    g_status.neverModel = 0;
    g_status.cardRefused = 0;
    g_status.cardUnreadable = 0;
}
inline void Reset() noexcept
{
    g_blades.clear();
    g_constantsReady = false;
    g_dirty = true;
    g_uploadedFrame = ~0ull;
    g_uploadedShader = nullptr;
    g_phasePrimary = g_phaseCross = 0.f;
    g_status = {};
    g_reportedInjection = false;
    g_reportedRefusal = false;
    g_noCoordinateReports = 0;
    g_noCoordinateReported.clear();
}

// The frame's own constants: the wind's heading and strength, the camera, the
// two phases and the shape of the block. Packed by the portable half; the only
// thing this side adds is the clock, which walks the phases forward and wraps
// them so a shader's sine never sees a large argument.
inline void Update(const renderer::FrameContext& frame, float gain) noexcept
{
    g_constantsReady = false;
    if (!enabled || !active || !settings.enabled) return;
    const renderer::WindState& wind = frame.wind;
    if (!wind.DrivesVegetation() || !frame.cameraValid) return;

    renderer::GrassBladeFrame blades;
    blades.directionX = wind.direction.x;
    blades.directionY = wind.direction.y;
    const float length = std::sqrt(blades.directionX * blades.directionX + blades.directionY * blades.directionY);
    if (length > 1e-4f) { blades.directionX /= length; blades.directionY /= length; }
    blades.strength = std::clamp(wind.strength * gain, 0.f, 3.f);
    blades.cameraX = frame.cameraPosition.x;
    blades.cameraY = frame.cameraPosition.y;
    blades.settings = settings;

    const float speed = settings.waveSpeedYardsPerSecond * std::clamp(wind.strength, 0.f, 3.f);
    g_phasePrimary = renderer::GrassBladePhase(g_phasePrimary, speed, settings.wavelengthYards, wind.deltaSeconds);
    g_phaseCross = renderer::GrassBladePhase(g_phaseCross, speed, settings.crossWavelengthYards, wind.deltaSeconds);
    blades.phasePrimary = g_phasePrimary;
    blades.phaseCross = g_phaseCross;

    g_constants = renderer::PackGrassBladeConstants(blades, 0);
    g_constantsReady = true;
    g_status.amplitudeYards = std::max(0.f, settings.tipAmplitudeYards) * blades.strength;
    // And each bent shader's own block: identical except for the one row that
    // says which way its blades' texture runs, which its own vertices measured.
    for (auto& shader : g_blades)
    {
        BladeShader& entry = shader.second;
        if (!entry.injected) continue;
        std::memcpy(entry.rows, g_constants.rows, sizeof(entry.rows));
        if (entry.rampByHeight)
        {
            // A card bent by its height: the ramp is the card's own floor and
            // height, so the weight is 0 at the base and 1 at the tip with no
            // up-or-down question to answer.
            entry.rows[renderer::kGrassBladeRows - 1][0] = entry.rampScale;
            entry.rows[renderer::kGrassBladeRows - 1][1] = entry.rampOffset;
        }
        else
        {
            entry.rows[renderer::kGrassBladeRows - 1][0] = entry.bladeVSign > 0 ? 1.f : -1.f;
            entry.rows[renderer::kGrassBladeRows - 1][1] = entry.bladeVSign > 0 ? 0.f : 1.f;
        }
        if (entry.worldSpace)
        {
            // A shader whose vertex is already in the world when the block runs:
            // there is no view position to correct with the camera (c14.x/y) and
            // no view depth for the distance fade (c17.w is the fade's slope on
            // r0.z, which for a world position is a height, not a distance). Both
            // are packed away here - the fade collapses to the constant 1 in
            // c14.z - and the block itself was built for that frame.
            entry.rows[0][0] = 0.f;
            entry.rows[0][1] = 0.f;
            entry.rows[3][3] = 0.f;
        }
        entry.rowsReady = true;
    }
}

// The HUD, in the wind debug box: what is bending, and - when something was
// refused - which of the blade shaders this build could not take.
inline std::vector<std::string> DebugLines()
{
    return renderer::GrassBladeDebugLines(g_status, settings);
}

// One unskinned, foliage-material draw, offered to this module. `material` is a
// callable returning the texture probe for the draw: it is only called for a
// shader this build has not decided about yet, because the read behind it (a
// level 0 lock) is the most expensive thing on this path and a shader is decided
// once.
// A draw whose shader this project has already identified as a pass that is
// never a model. Counted on the blade row and never offered to the injection
// below: an interface icon is a cut-out card with a texture coordinate - exactly
// the shape a blade has - and a bending interface would be a far worse bug than
// a still field. The client's native shadow casters, rain, snow and water are in
// the same list, and a depth-only pass is refused by its colour mask before it
// ever gets here.
//
// The WORLD verdict is deliberately NOT a refusal on this path (it was, and a
// real session paid for it: 376 unskinned foliage draws were turned away as
// "world-shaped" while the field stayed perfectly still). The world's own
// projection in c4..c7 is a suspicion on the model path because a model is
// supposed to carry its bones at c31 - and an unskinned card has no bones at
// all, so binding an object -> world matrix is simply how it is drawn. What is
// left to prove a blade on this path is the draw's own geometry and material,
// and that is what the injection below proves.
inline void RefuseShader() noexcept
{
    ++g_status.neverModel;
}

template<class MaterialProbe>
inline void ConsiderDraw(IDirect3DDevice9* device, const renderer::FrameContext& frame, uint64_t hash,
    IDirect3DVertexShader9* shader, const renderer::M2VertexLayout& layout, const renderer::M2DrawCall& draw,
    bool worldShaped, MaterialProbe&& material) noexcept
{
    if (!device || !shader) return;
    if (!enabled || !active || !settings.enabled) return;
    if (!frame.wind.DrivesVegetation()) return;
    // Which ramp this card can be bent by. The texture's v is the reference
    // client's own and stays first: it is the blade's height in texture space, and
    // the block reads it whenever the draw carries that coordinate as two floats.
    //
    // The family that carries no coordinate at all is the one this path used to
    // refuse, and refusing it is what left the field still while the log said a
    // shader was bent: 1.2 million of those draws in one session against 49 that
    // reached the material gate and 4 to 17 that went out bent. Those cards are
    // bent by their own height instead - the block reads the vertex's z, and the
    // ramp is the card's own measured floor and height - which is the same 0-at-
    // the-base, 1-at-the-tip weight the coordinate gave, taken from the one thing
    // those vertices do carry. Nothing else about the funnel changes: the material
    // has to measure as a cut-out, the draw's own vertices have to measure as a
    // blade of a blade's size, and the shader still has to be one the client's
    // shader library says is not the interface, the precipitation or the liquid.
    const bool rampByHeight = !(layout.texcoord && layout.texcoordIsFloat2);
    if (rampByHeight)
    {
        if (!layout.position)
        {
            // Neither a texture coordinate nor a position: there is no height on
            // this draw to read a bend weight from, and no register to read it
            // out of. This is the one uv-less card that stays refused - and a
            // declaration with no position at all is not a card, so the row
            // naming it is where a report should end up if it appears.
            ++g_status.noTexcoord;
            return;
        }
        if (g_noCoordinateReports < kNoCoordinateReports && shader && device &&
            g_noCoordinateReported.find(shader) == g_noCoordinateReported.end())
        {
            uint32_t bytes = 0;
            const std::string disassembly = DisassembleShader(shader, bytes);
            if (!disassembly.empty())
            {
                ++g_noCoordinateReports;
                g_noCoordinateReported.insert(shader);
                const renderer::TextureAlphaProbe texture = material();
                char text[512]{};
                std::snprintf(text, sizeof(text),
                    "Grass blade sway: a foliage draw carries no texture coordinate; its declaration is %s, and its "
                    "blade is bent by the vertex's own height instead. Stage 0 measures %s [%u texel(s) examined]. Its "
                    "shader (%u bytes) follows once:", layout.description, renderer::TextureCutoutText(texture),
                    texture.texels, bytes);
                Log(text);
                Log(disassembly.c_str());
            }
        }
    }

    BladeShader* entry = FindBladeShader(shader);
    if (!entry)
    {
        if (g_blades.size() >= kMaxBladeShaders) return;
        BladeShader fresh;
        fresh.original = shader;
        fresh.hash = hash;
        fresh.worldShaped = worldShaped;
        entry = &g_blades.emplace(static_cast<const void*>(shader), std::move(fresh)).first->second;
    }
    else
    {
        if (entry->injected)
        {
            // And a batch too big to be a blade is not bent by that shader, however
            // good the card this shader was admitted on. One injected shader
            // replaces EVERY draw made with it, so a shader that draws both the
            // field and a piece of the world (a merged tile, a WMO group, a wide
            // decal under the same material) would otherwise bend the world with
            // the grass. The batch's own vertex count is free - it is in the draw
            // call - and it is the one measurement that separates the two without
            // touching a vertex buffer: a blade card is a couple of dozen vertices,
            // a terrain tile is thousands.
            const unsigned batch = renderer::GrassBladeDrawVertices(draw.primitives, draw.type);
            if (batch > renderer::kGrassBladeMaxCardVertices)
            {
                ++g_status.bigDraws;
                // Once per shader, with the number in it: this bound is the one
                // thing on the path that is a guess about how the client batches
                // its grass, and a guess that is too tight costs the player the
                // field. If the field really is drawn in batches this size, the
                // line says exactly what to raise the bound to, instead of the
                // next report being another "nothing sways, the log says big N".
                if (!entry->bigReported)
                {
                    entry->bigReported = true;
                    char text[320]{};
                    std::snprintf(text, sizeof(text),
                        "Grass blade sway: a batch of %u vertices was left as the client drew it, because no blade "
                        "card is more than %u vertices. If the grass itself is drawn in batches this size, this "
                        "bound is the one to raise: the number above is the size the field arrives in.",
                        batch, renderer::kGrassBladeMaxCardVertices);
                    Log(text);
                }
                return;
            }
            // The common case by far: a shader this build has already bent. The
            // only work left is the frame's constants, written before the draw -
            // and, for a client that binds its shaders through state blocks
            // (which never reach this project's Set hook), the bind itself: if
            // the device is about to draw with the client's own copy of a shader
            // this build has bent, the bent one goes in here, where this draw has
            // already been proven to be a blade card.
            ++g_status.bladeDraws;
            if (entry->rampByHeight) ++g_status.heightDraws;
            Upload(device, frame.frameIndex, *entry);
            if (shader == entry->original.Get()) device->SetVertexShader(entry->injected.Get());
            return;
        }
        // A shader whose structure cannot take the block never can, and one whose
        // material did not look like foliage is looked at a few times before that
        // answer is taken as final: the material of one draw can be read wrong,
        // the shape of a shader cannot.
        if (entry->structureRefused || entry->materialTries >= kMaterialRetries) return;
    }

    // The material: a card cut out of a transparent image, which is what a blade
    // is. Whether its texels are GREEN is deliberately not asked here, and the
    // difference from the model path's gate one branch above is the point: the
    // grass a player stands in has to be bent on any zone and any season, and a
    // dry, autumn or snow-covered field is as much of a blade card as a green
    // one. Nothing that could be mistaken for a player's gear can reach this
    // path at all - a character, a weapon, a mount, a cape and every other
    // attached model carries bone indices in its declaration and is judged by
    // the model path above, which is where the green test earns its keep - so
    // asking about colour here would refuse the very field this exists for.
    ++entry->materialTries;
    ++g_status.examined;
    ++g_status.foliageDraws;
    const renderer::TextureAlphaProbe texture = material();
    if (texture.verdict != renderer::TextureCutout::Cutout)
    {
        ++g_status.materialRefused;
        return;
    }
    if (texture.readable()) entry->greenPercent = texture.greenFraction();

    // The declaration's own texture coordinate has to be the register the shader
    // reads. A shader compiled for one layout drawn through another is a mistake
    // this module must not turn into a wave, and the check is cheap: the layout's
    // register comes from the declaration's element order, which is how D3D9
    // assigns them, and the shader's from its own declaration line.
    uint32_t bytes = 0;
    const std::string disassembly = DisassembleShader(entry->original.Get(), bytes);
    if (!disassembly.empty())
    {
        unsigned declared = 0;
        const bool declaredHere = rampByHeight
            ? renderer::GrassBladePositionRegister(disassembly, declared)
            : renderer::GrassBladeTexcoordRegister(disassembly, declared);
        const unsigned carried = rampByHeight ? layout.positionRegister : layout.texcoordRegister;
        if (declaredHere && declared != carried)
        {
            entry->structureRefused = true;
            ++g_status.refused;
            ++g_status.registerMismatch;
            Log(std::string("Grass blade sway: a foliage draw declares its ") +
                (rampByHeight ? "vertex position" : "texture coordinate") + " in v" + std::to_string(carried) +
                " while its shader reads v" + std::to_string(declared) +
                ", so this build will not bend it - the bend weight would come from the wrong channel.");
            return;
        }
    }

    // Which way up a blade's texture runs, from the draw's own vertices: the
    // reference client has v = 1 at the base, and a shader whose blades are the
    // other way up is bent by the same block with one constant's sign flipped.
    const renderer::GrassBladeCard card = MeasureBladeCard(device, layout, draw, rampByHeight);
    // A shader is bent only when the draw's OWN vertices say it is a blade: a
    // measurable card, of a blade's size, with at least one base-and-tip pair in
    // it. Nothing else is admitted - not a card that could not be read, not a card
    // that measured zero, and not a card of a tree's size. That is the whole
    // answer to "parts of the buildings sway": a real session bent a 22.91 yd card
    // with ten pairs in it, which is a canopy or a wall carrying a foliage texture,
    // and at that size the bend is invisible while the card's silhouette moves.
    // The verdict takes a few draws, because one shader is bent for all of its
    // draws and the first one to arrive is not always a blade (a chunk of grass is
    // drawn as several cards, some of them flat: they share this material).
    // One predicate, in the portable header, so the admission test is a thing the
    // suite can exercise rather than five clauses repeated in a device function.
    const bool bladeShaped = renderer::GrassBladeCardIsGrass(card);
    if (!bladeShaped)
    {
        ++entry->cardTries;
        if (card.measured) ++g_status.cardRefused;
        else ++g_status.cardUnreadable;
        // A draw that is not a blade card is NOT a verdict about its shader. One
        // shader draws a whole family - some of its batches are blade cards, some
        // are the wide flat ones that share the material, and a wall or a terrain
        // tile drawn through the same shader looks like one of these too - so the
        // answer is looked for in more draws rather than taken from the first few.
        // A `structureRefused` here would end the shader for the session on the
        // sixth unlucky batch, which is exactly how a field that should sway goes
        // still while the log says a shader was bent.
        if (entry->cardTries < kBladeCardTries) return;
        entry->structureRefused = true;
        ++g_status.refused;
        char text[560]{};
        if (card.measured)
            std::snprintf(text, sizeof(text),
                "Grass blade sway: none of the %u draws offered for a foliage shader measured as a blade card (the "
                "last was %.4f yd tall with %u blade-shaped pair(s), %u vertices standing on %.2f yd of ground; a "
                "blade is %.2f to %.1f yd with at least %u pair, at most %u vertices and at most %.0f yd across), so "
                "no shader was bent for it. A canopy, a wall carrying a foliage texture and a decal all look like "
                "this, and bending one of them moves a piece of the scenery instead of a blade.",
                entry->cardTries, double(card.extentYards), card.pairs, card.vertices, double(card.footprintYards),
                double(renderer::kGrassBladeMinExtentYards), double(renderer::kGrassBladeMaxExtentYards),
                renderer::kGrassBladeMinPairs, renderer::kGrassBladeMaxCardVertices,
                double(renderer::kGrassBladeMaxCardFootprintYards));
        else
            std::snprintf(text, sizeof(text),
                "Grass blade sway: none of the %u draws offered for a foliage shader could be measured at all (the "
                "stream is not lockable, or the batch is not a triangle list), so no shader was bent for it - an "
                "unreadable draw is not evidence of a blade, and the grass of a real client measures in the first "
                "draw or two.", entry->cardTries);
        Log(text);
        return;
    }
    entry->bladeVSign = card.sign;
    entry->bladeExtentYards = card.extentYards;
    entry->bladePairs = card.pairs;
    entry->rampByHeight = rampByHeight;
    if (rampByHeight)
    {
        // The weight the block builds from the vertex's own height: the card's
        // measured floor and height, so the ramp runs 0 at the base to 1 at the
        // tip of the card that was measured - the same shape the texture's v gave.
        entry->rampScale = card.extentYards > 0.f ? 1.f / card.extentYards : 1.f;
        entry->rampOffset = -card.baseYards * entry->rampScale;
    }
    BuildInjectedShader(device, *entry);
    if (entry->injected && entry->worldShaped && !entry->worldReported)
    {
        // Recorded, not refused: the client draws its unskinned cards through
        // the world path, and saying so once per shader is what keeps this from
        // reading as "nothing sways" the next time a field is measured.
        entry->worldReported = true;
        ++g_status.worldShaped;
        char text[360]{};
        std::snprintf(text, sizeof(text),
            "Grass blade sway: hash=0x%016llx binds the world's own projection in c4..c7, which is how this client "
            "draws an unskinned card - it is bent, because its own vertices measured %.2f yd of upright blade in %u "
            "pair(s), which is what a blade card is and what a piece of the world is not.",
            static_cast<unsigned long long>(entry->hash), double(entry->bladeExtentYards), entry->bladePairs);
        Log(text);
    }
    // The draw that revealed the shader is not bent (there was nothing to bind
    // when it arrived); every draw of it after this one is.
}
}
