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
    bool valid = false;
    bool position = false;
    bool skinned = false; // POSITION + BLENDINDICES: the client's palette path
    bool weighted = false;
    unsigned positionOffset = 0;
    unsigned boneOffset = 0;
};

inline bool ParseM2VertexLayout(const D3DVERTEXELEMENT9* elements, unsigned count, M2VertexLayout& out) noexcept
{
    out = {};
    if (!elements) return false;
    for (unsigned i = 0; i < count; ++i)
    {
        const D3DVERTEXELEMENT9& element = elements[i];
        if (element.Stream != 0) continue;
        if (element.Usage == D3DDECLUSAGE_POSITION && element.Type == D3DDECLTYPE_FLOAT3 && !out.position)
        {
            out.position = true;
            out.positionOffset = element.Offset;
        }
        if (element.Usage == D3DDECLUSAGE_BLENDINDICES &&
            (element.Type == D3DDECLTYPE_UBYTE4 || element.Type == D3DDECLTYPE_D3DCOLOR) && !out.skinned)
        {
            out.skinned = true;
            out.boneOffset = element.Offset;
        }
        if (element.Usage == D3DDECLUSAGE_BLENDWEIGHT) out.weighted = true;
    }
    out.valid = out.position && out.skinned;
    return out.valid;
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
    if (!device || !layout.valid || draw.verticesUP || !vertices) return false;
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
