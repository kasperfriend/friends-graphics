#pragma once
// The client's own model vertices: what a draw is about to render, and which
// bones its vertices reference.
//
// The sway needs two things from a draw that the constant file cannot give it:
// how big the model is, and how many of the bone blocks at c31 are THIS model's.
// The client uploads a model's bones starting at c31 and leaves every other
// register of the constant file holding whatever the previous draw put there, so
// a reader that walks c31 upwards until a block stops looking like a matrix
// reads the PREVIOUS model's bones as if they were this one's: a six-bone bush
// counted as a seventy-five-bone skeleton. The vertex data is the only place
// that says which bones a model has, and the client's own vertices are also the
// only honest measure of how big the model is - which is what a one-bone tuft
// needs, because a single bone pivot spans no height at all.
//
// This is the read the flame capture already performs on a flame draw
// (src/Lighting/LocalLightManager.cpp, verified live): lock the draw's stream,
// follow its indices, take each sampled vertex's position and its bone index.
// The same draw is read here with the model's own bound and size taken from the
// same vertices, so a reader cannot disagree with the capture about the layout.
// Only vertex-buffer draws are read: an up-front (UP) draw is a particle or UI
// batch with no stable model identity, and the sway never touches one.
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <wrl/client.h>
#include "../Environment/VegetationWind.h"

namespace renderer
{
// One draw's vertex layout, parsed from the declaration the client bound for it.
// Stream 0 only, exactly like the flame capture: the client's model path keeps
// POSITION and its bone indices in the same stream.
struct M2VertexLayout
{
    // `position` means the vertex data can be addressed at all; `skinned` is the
    // stronger and much more important statement that the vertices carry a BONE
    // INDEX. Every M2 model the client draws - one bone or seventy - is declared
    // with the model vertex format, blend indices included, so a draw that has
    // them is on the client's skinned model path, and a draw that does not is
    // terrain, a WMO, a particle, a ribbon or the interface. `valid` is the two
    // together: a declaration the sway is willing to read at all.
    bool valid = false;
    bool position = false;
    bool skinned = false; // POSITION + BLENDINDICES: the client's palette path
    bool weighted = false;
    unsigned positionOffset = 0;
    unsigned boneOffset = 0;
    // The texture coordinate, for the one reader that needs it without needing a
    // skeleton: the blade-shader path bends a grass card by the HEIGHT ALONG THE
    // BLADE, which is that coordinate's v - and the register the shader reads it
    // from is the element's own place in the declaration, because that is how
    // D3D9 assigns input registers. A draw whose vertices carry no such element
    // can never be a blade card, so this is filled whatever `valid` says.
    bool texcoord = false;
    bool texcoordIsFloat2 = false; // the block reads .y, so two floats are what it needs
    unsigned texcoordRegister = 0;
    unsigned texcoordOffset = 0;
    // Where the position itself lands. A card bent along its texture's v does not
    // need it, but a card whose vertices carry NO texture coordinate is bent by
    // its own height - which is this register's z - and the shader's own
    // `dcl_position` has to name the same register for the block to read the right
    // input (see MeasureGrassBladeCardByHeight).
    unsigned positionRegister = 0;
    // The declaration as one line of text, filled by whoever read the elements
    // (DescribeVertexDeclaration). The blade path logs it for the family that
    // carries no texture coordinate: what a report has to say is what the vertices
    // DO carry.
    char description[192]{};
};

inline bool ParseM2VertexLayout(const D3DVERTEXELEMENT9* elements, unsigned count, M2VertexLayout& out) noexcept
{
    out = {};
    if (!elements) return false;
    unsigned stream0Register = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const D3DVERTEXELEMENT9& element = elements[i];
        // Every element in the declaration takes an input register in the order
        // it is declared, stream by stream, so the register is counted before
        // anything is skipped.
        const unsigned inputRegister = stream0Register;
        if (element.Stream != 0) continue;
        ++stream0Register;
        // FLOAT4 positions are accepted as well as the client's own FLOAT3: the
        // fourth component of a valid model position is 1 and is ignored by the
        // 3x4 bone matrices either way.
        if (element.Usage == D3DDECLUSAGE_POSITION &&
            (element.Type == D3DDECLTYPE_FLOAT3 || element.Type == D3DDECLTYPE_FLOAT4) && !out.position)
        {
            out.position = true;
            out.positionOffset = element.Offset;
            out.positionRegister = inputRegister;
        }
        // UBYTE4N as well as UBYTE4: a normalised byte pair is the same four
        // bytes to a shader that reads them as an index; only the declaration's
        // own idea of the range differs, and the reader wants the raw value.
        if (element.Usage == D3DDECLUSAGE_BLENDINDICES &&
            (element.Type == D3DDECLTYPE_UBYTE4 || element.Type == D3DDECLTYPE_UBYTE4N ||
             element.Type == D3DDECLTYPE_D3DCOLOR) && !out.skinned)
        {
            out.skinned = true;
            out.boneOffset = element.Offset;
        }
        if (element.Usage == D3DDECLUSAGE_BLENDWEIGHT) out.weighted = true;
        if (element.Usage == D3DDECLUSAGE_TEXCOORD && element.UsageIndex == 0 && !out.texcoord)
        {
            out.texcoord = true;
            out.texcoordIsFloat2 = element.Type == D3DDECLTYPE_FLOAT2;
            out.texcoordRegister = inputRegister;
            out.texcoordOffset = element.Offset;
        }
    }
    out.valid = out.position && out.skinned;
    return out.valid;
}

// The declaration as one line of text, for the renderer log. A draw that a
// player reports as "standing still" is diagnosed from this: the reader needs a
// POSITION it can address, and the sway additionally needs to know whether the
// vertices carry a bone index. The names are D3D's own, spelled the way the
// debug output of a capture tool spells them.
inline const char* VertexUsageName(BYTE usage) noexcept
{
    switch (usage)
    {
    case D3DDECLUSAGE_POSITION: return "POSITION";
    case D3DDECLUSAGE_BLENDWEIGHT: return "BLENDWEIGHT";
    case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
    case D3DDECLUSAGE_NORMAL: return "NORMAL";
    case D3DDECLUSAGE_PSIZE: return "PSIZE";
    case D3DDECLUSAGE_TEXCOORD: return "TEXCOORD";
    case D3DDECLUSAGE_TANGENT: return "TANGENT";
    case D3DDECLUSAGE_BINORMAL: return "BINORMAL";
    case D3DDECLUSAGE_COLOR: return "COLOR";
    case D3DDECLUSAGE_POSITIONT: return "POSITIONT";
    default: return "OTHER";
    }
}

inline const char* VertexTypeName(BYTE type) noexcept
{
    switch (type)
    {
    case D3DDECLTYPE_FLOAT1: return "FLOAT1";
    case D3DDECLTYPE_FLOAT2: return "FLOAT2";
    case D3DDECLTYPE_FLOAT3: return "FLOAT3";
    case D3DDECLTYPE_FLOAT4: return "FLOAT4";
    case D3DDECLTYPE_D3DCOLOR: return "D3DCOLOR";
    case D3DDECLTYPE_UBYTE4: return "UBYTE4";
    case D3DDECLTYPE_SHORT2: return "SHORT2";
    case D3DDECLTYPE_SHORT4: return "SHORT4";
    case D3DDECLTYPE_UBYTE4N: return "UBYTE4N";
    case D3DDECLTYPE_SHORT2N: return "SHORT2N";
    case D3DDECLTYPE_SHORT4N: return "SHORT4N";
    case D3DDECLTYPE_USHORT2N: return "USHORT2N";
    case D3DDECLTYPE_USHORT4N: return "USHORT4N";
    case D3DDECLTYPE_UDEC3: return "UDEC3";
    case D3DDECLTYPE_DEC3N: return "DEC3N";
    case D3DDECLTYPE_FLOAT16_2: return "FLOAT16_2";
    case D3DDECLTYPE_FLOAT16_4: return "FLOAT16_4";
    default: return "OTHER";
    }
}

inline void DescribeVertexDeclaration(const D3DVERTEXELEMENT9* elements, unsigned count, char* out,
    size_t size) noexcept
{
    if (!out || !size) return;
    out[0] = 0;
    if (!elements) return;
    size_t used = 0;
    unsigned written = 0;
    for (unsigned i = 0; i < count && written < 8; ++i)
    {
        const D3DVERTEXELEMENT9& element = elements[i];
        if (element.Stream == 0xFF) break; // D3DDECL_END
        if (element.Stream != 0) continue;
        char piece[64]{};
        std::snprintf(piece, sizeof(piece), "%s%s/%s@%u", written ? " " : "",
            VertexUsageName(element.Usage), VertexTypeName(element.Type), unsigned(element.Offset));
        const size_t length = std::strlen(piece);
        if (used + length + 1 >= size) break;
        std::memcpy(out + used, piece, length);
        used += length;
        out[used] = 0;
        ++written;
    }
}

// One draw call, exactly as the four device hooks receive it.
struct M2DrawCall
{
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    bool indexed = false;
    INT base = 0;
    UINT start = 0;
    UINT primitives = 0;
    UINT minVertexIndex = 0;
    UINT vertexCount = 0;
    const void* verticesUP = nullptr;
    UINT strideUP = 0;
    const void* indicesUP = nullptr;
    D3DFORMAT indexFormat = D3DFMT_INDEX16;
};

// The largest byte range this reader will lock. A model's own batch is a few
// hundred kilobytes at most; anything larger is not a batch the sway should be
// locking on the render thread.
inline constexpr uint64_t kM2GeometryMaxBytes = 1ull << 20;

struct M2DrawGeometry
{
    bool valid = false;
    uint64_t model = 0; // vertex-buffer identity, so one model's batches share a measurement
    unsigned samples = 0;
    // The model's own bone blocks, taken from its own vertices: every bone index
    // this draw's vertices reference, plus one. This is the bound the palette is
    // read and analysed with - never the whole rest of the constant file.
    unsigned bones = 0;
    VegetationGeometrySample sample[kVegetationGeometrySamples];
};

// Read vertices spread over the draw's own range - their model-space positions
// and their bone indices - plus the highest bone index the range references.
// Returns false, and the draw is left alone, when the client's buffers are not
// readable, when the layout cannot address a position, or when the batch is
// larger than kM2GeometryMaxBytes.
//
// The stream, its offset and its stride are the caller's if it already has them
// (the sway keys an instance on its vertex buffer, so it does); use the
// one-line overload below otherwise.
inline bool ReadM2DrawGeometry(IDirect3DDevice9* device, const M2VertexLayout& layout,
    const M2DrawCall& draw, IDirect3DVertexBuffer9* vertices, UINT offset, UINT stride, M2DrawGeometry& out)
{
    out = {};
    if (!device || !layout.valid || !layout.skinned || draw.verticesUP || !vertices) return false;
    const UINT count = draw.type == D3DPT_TRIANGLELIST ? draw.primitives * 3 :
        draw.type == D3DPT_TRIANGLESTRIP ? draw.primitives + 2 : 0;
    if (count < 3) return false;

    using Microsoft::WRL::ComPtr;
    if (stride <= UINT(layout.positionOffset) + 12u) return false;
    D3DVERTEXBUFFER_DESC vertexDesc{};
    if (FAILED(vertices->GetDesc(&vertexDesc))) return false;

    // Which vertices the draw consumes, exactly as the flame capture follows it:
    // an indexed draw is read through its index buffer, a plain one is the run of
    // vertices the client asked for.
    const void* indices = nullptr;
    D3DFORMAT indexFormat = draw.indexFormat;
    ComPtr<IDirect3DIndexBuffer9> indicesBuffer;
    void* lockedIndices = nullptr;
    bool usable = true;
    UINT low = ~0u, high = 0;
    if (draw.indexed)
    {
        if (draw.indicesUP)
        {
            indices = draw.indicesUP;
        }
        else
        {
            D3DINDEXBUFFER_DESC indexDesc{};
            if (FAILED(device->GetIndices(indicesBuffer.GetAddressOf())) || !indicesBuffer) return false;
            if (FAILED(indicesBuffer->GetDesc(&indexDesc))) return false;
            indexFormat = indexDesc.Format;
            const unsigned size = indexFormat == D3DFMT_INDEX16 ? 2u : 4u;
            if (uint64_t(draw.start) * size + uint64_t(count) * size > indexDesc.Size) return false;
            if (uint64_t(count) * size > kM2GeometryMaxBytes) return false;
            if (FAILED(indicesBuffer->Lock(uint64_t(draw.start) * size, uint64_t(count) * size, &lockedIndices,
                    D3DLOCK_READONLY)) || !lockedIndices) return false;
            indices = lockedIndices;
        }
        if (!indices)
        {
            if (lockedIndices) indicesBuffer->Unlock();
            return false;
        }
        for (UINT i = 0; i < count; ++i)
        {
            const int64_t element = indexFormat == D3DFMT_INDEX16 ?
                int64_t(static_cast<const WORD*>(indices)[i]) : int64_t(static_cast<const DWORD*>(indices)[i]);
            const int64_t vertex = int64_t(draw.base) + element;
            if (vertex < 0 || uint64_t(offset) + uint64_t(vertex + 1) * stride > vertexDesc.Size)
            {
                usable = false;
                break;
            }
            low = std::min(low, UINT(vertex));
            high = std::max(high, UINT(vertex));
        }
        if (draw.vertexCount && uint64_t(low) < draw.minVertexIndex)
            usable = false;
        if (draw.vertexCount && uint64_t(high) >= uint64_t(draw.minVertexIndex) + draw.vertexCount)
            usable = false;
    }
    else
    {
        if (uint64_t(draw.start) + count > UINT(~0u) - 1u) usable = false;
        else
        {
            low = draw.start;
            high = draw.start + count - 1;
            if (uint64_t(offset) + uint64_t(high + 1) * stride > vertexDesc.Size) usable = false;
        }
    }
    if (!usable || high < low || uint64_t(high - low + 1) * stride > kM2GeometryMaxBytes)
    {
        if (lockedIndices) indicesBuffer->Unlock();
        return false;
    }

    void* locked = nullptr;
    if (FAILED(vertices->Lock(uint64_t(offset) + uint64_t(low) * stride, uint64_t(high - low + 1) * stride,
            &locked, D3DLOCK_READONLY)) || !locked)
    {
        if (lockedIndices) indicesBuffer->Unlock();
        return false;
    }
    const unsigned char* stream = static_cast<const unsigned char*>(locked);

    // Every vertex of the range contributes its bone index: a model's own bone
    // count is the highest index ANY of its vertices binds, and a bound that
    // missed a canopy bone would leave that bone un-leaned while the trunk moved.
    unsigned highestBone = 0;
    for (UINT vertex = low; vertex <= high; ++vertex)
        highestBone = std::max(highestBone, unsigned(stream[uint64_t(vertex - low) * stride + layout.boneOffset]));

    // The sampled vertices are what measures the model: spread over the batch, so
    // the top and the bottom of what this draw renders are both in the set.
    unsigned samples = 0;
    const unsigned wanted = kVegetationGeometrySamples;
    for (unsigned s = 0; s < wanted; ++s)
    {
        const UINT position = wanted > 1 ? s * (count - 1) / (wanted - 1) : 0;
        int64_t vertex = int64_t(draw.start) + position;
        if (draw.indexed)
        {
            const int64_t element = indexFormat == D3DFMT_INDEX16 ?
                int64_t(static_cast<const WORD*>(indices)[position]) :
                int64_t(static_cast<const DWORD*>(indices)[position]);
            vertex = int64_t(draw.base) + element;
        }
        if (vertex < int64_t(low) || vertex > int64_t(high)) continue;
        const unsigned char* bytes = stream + uint64_t(vertex - low) * stride;
        float positionValue[3]{};
        std::memcpy(positionValue, bytes + layout.positionOffset, sizeof(positionValue));
        if (!std::isfinite(positionValue[0]) || !std::isfinite(positionValue[1]) ||
            !std::isfinite(positionValue[2]))
            continue;
        VegetationGeometrySample& sample = out.sample[samples++];
        sample.position[0] = positionValue[0];
        sample.position[1] = positionValue[1];
        sample.position[2] = positionValue[2];
        sample.bone = unsigned(bytes[layout.boneOffset]);
    }

    vertices->Unlock();
    if (lockedIndices) indicesBuffer->Unlock();
    if (!samples) return false;

    out.samples = samples;
    out.bones = std::min(highestBone + 1u, kM2MaxBones);
    out.model = VegetationHash64(uint64_t(reinterpret_cast<uintptr_t>(vertices)) ^
        (uint64_t(stride) << 32) ^ uint64_t(offset));
    out.valid = true;
    return true;
}

// The convenience form: fetch the stream itself. The sway reads the stream first
// anyway (the vertex buffer is part of an instance's identity), so it calls the
// form above with the buffer it already holds and the draw costs one
// GetStreamSource either way.
inline bool ReadM2DrawGeometry(IDirect3DDevice9* device, const M2VertexLayout& layout,
    const M2DrawCall& draw, M2DrawGeometry& out)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDirect3DVertexBuffer9> vertices;
    UINT offset = 0, stride = 0;
    if (!device || FAILED(device->GetStreamSource(0, vertices.GetAddressOf(), &offset, &stride)) || !vertices)
        return false;
    return ReadM2DrawGeometry(device, layout, draw, vertices.Get(), offset, stride, out);
}
}
