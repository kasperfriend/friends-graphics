#pragma once
// Wind-driven sway for the client's OWN vegetation draws.
//
// The client's skinned M2 vertex shader cannot be replaced: it carries the
// client's lighting, fog and texture stages. So the sway is applied where the
// client already keeps its animation - in the bone matrices it uploads for the
// draw - and the client's own matrices are put back the moment the draw is
// over. src/Environment/VegetationWind.h owns the policy (how far a plant of a
// given size leans right now, and how a bone block is rotated about the point
// where the model meets the ground); this file owns the device half: decide
// which draws are vegetation, read the palette, lean it, restore it.
//
// A draw is touched only when EVERY one of these holds. A failure at any step
// leaves the draw exactly as the client intended it, and is COUNTED, because a
// gate that refuses silently is a gate nobody can diagnose:
//   1. the shared wind is on, the vegetation link is on and the effect is on;
//   2. it is transparent (alpha tested or blended). A solid draw cannot be
//      foliage and never reveals an instance, though it is leaned once its own
//      model is known - that is what keeps a tree's opaque trunk welded to the
//      canopy above it;
//   3. its vertex shader is one of the rigid skinned M2 variants this project
//      verified live (src/Lighting/TorchSignatures.h), or it binds the client's
//      M2 model layout in c2..c5 - the same proof the material cache and the
//      camera's sky fallback run on live draws - and it is not one of the passes
//      that must never be leaned (the native shadow casters above all, whose
//      palettes are in the LIGHT's space, plus UI, precipitation and water). A
//      newly proven variant is learned for the session and written to the
//      renderer's log so it can be captured like the rest;
//   4. it uses the client's skinned model layout (POSITION float3 and
//      BLENDINDICES in stream 0) and is drawn from a vertex buffer;
//   5. the frame has an accepted camera pose (cameraValid + viewRawValid) and
//      the draw renders into that camera's viewport, which is what makes the
//      lean geometry be in the frame camera's own space - the client's portrait,
//      minimap and reflection views are cameras of their own;
//   6. the draw's own vertices are readable, which is what BOUNDS the palette:
//      c31 is followed by whatever the previous draw left there, so counting
//      "blocks that look like matrices" counts another model's skeleton. The
//      bound is the highest bone index this draw's vertices reference, and
//      nothing past it is ever read, analysed or written;
//   7. the palette at c31 looks like object -> view matrices (finite,
//      orthogonal up to scale, plausible translation) up to that bound;
//   8. the model is plant-shaped: at most kVegetationMaxBones bones, and a size
//      measured from its own vertices - not from its bone pivots, which span no
//      height at all for a one-bone tuft;
//   9. it is inside DistanceYards of the camera, with the sway fading to
//      nothing before the limit so a tree never pops between leaning and still;
//  10. the client is NOT moving it: the pose read this frame has to equal the
//      pose read last frame AND the model's own vertices have to be in the same
//      world positions. A static doodad repeats both exactly; an animated
//      neighbour, a character, a creature, an effect or a particle batch (which
//      the client re-uploads every frame even when nothing about it moves)
//      never does.
//
// Batches of one model stay coherent because the decision belongs to the
// INSTANCE - its placement point, its root orientation and the vertex buffer it
// is drawn from, all three camera-independent - and not to the batch: the opaque
// trunk of a tree follows the alpha-tested canopy that revealed it, even when
// the client draws the trunk first.
//
// The sway adds no shader constant of its own: it reads the client's palette,
// writes the leaned blocks into the same registers, and restores them. That is
// why the disabled path can be asserted as "no Get/SetVertexShaderConstantF".
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include "../Core/FrameContext.h"
#include "../Core/M2DrawGeometry.h"
#include "../Core/M2ShaderProbe.h"
#include "../Core/MathTypes.h"
#include "../D3D9/CameraContinuity.h"
#include "../D3D9/TrackedRenderState.h"
#include "../Environment/VegetationWind.h"
#include "../Environment/WindDebug.h"

namespace vegetation
{
// [Vegetation] Enabled and the F11 graphics switch, like every other effect's
// pair of flags. Both are checked before any device call.
inline bool enabled = true;
inline bool active = true;
inline renderer::VegetationSettings settings;
inline renderer::VegetationWind wind;
inline renderer::M2ShaderProbe probe;

// The renderer's own log, set once at startup. A shader this project has never
// captured is reported here by hash the first time it proves itself, so the
// capture list can be extended from a player's log instead of from a guess - and
// a draw that cannot be leaned for a reason the player cannot see says so once.
inline void (*logLine)(const char*) = nullptr;
inline void Log(const std::string& text) { if (logLine) logLine(text.c_str()); }

// Device-side bookkeeping only: the model-shape gates themselves are policy and
// live in VegetationWind.h (renderer::kVegetationMaxBones, renderer::kVegetationMinModelHeight,
// renderer::kVegetationMaxModelHeight, renderer::kVegetationMinFootprint,
// renderer::kVegetationFootprintToHeight, renderer::kVegetationPoseTolerance), where the portable
// suite can pin them.
inline constexpr unsigned kMaxStableFrames = 8;
inline constexpr uint64_t kStaleFrames = 30;
inline constexpr unsigned kMaxMeshProfiles = 512;
inline constexpr unsigned kMaxShaderProfiles = 512;
inline constexpr unsigned kMaxGeometryProfiles = 1024;
inline constexpr unsigned kMaxInstances = 1024;
// How many of a batch's sampled vertices the mesh-move test watches. Eight is
// enough to see a re-uploaded particle batch and costs eight matrix multiplies.
inline constexpr unsigned kGeometryPoseSamples = 8;

// One vertex-declaration layout, cached by identity like every other live D3D9
// object in this project. The client keeps a handful of declarations, so this
// only ever holds a few entries.
struct MeshProfile
{
    bool skinned = false;
    renderer::M2VertexLayout layout;
    uint64_t lastSeenFrame = 0;
};

// What one vertex shader turned out to be. Verified variants are accepted on
// sight; anything else has to bind the client's M2 model layout, which is read
// once per shader and remembered.
struct ShaderProfile
{
    bool model = false;
    bool reported = false;      // the log already carries this shader's verdict
    const char* reason = "";    // why it was refused, for that one log line
    uint64_t lastSeenFrame = 0;
};

// One batch's own vertices, read once and reused every frame: the model's own
// bone bound and a spread of its model-space vertices. The world positions of
// the first few samples are kept too - a mesh the client re-uploads with
// different data every frame (a particle system, an effect) is moving even when
// every matrix it binds is still, and that is exactly what the sway must never
// touch.
struct GeometryProfile
{
    renderer::M2DrawGeometry geometry;
    float poseWorld[kGeometryPoseSamples][3]{};
    uint64_t poseFrame = 0;
    unsigned poseSamples = 0;
    uint64_t lastSeenFrame = 0;
};

// One placed model. `key` is its identity (placement point, root orientation,
// vertex buffer) and doubles as the per-instance phase seed, so two tufts a yard
// apart never shimmer in lockstep.
struct Instance
{
    renderer::VegetationBones pose;
    float pivot[3]{};
    float height = 0.f;
    float footprint = 0.f;
    uint64_t key = 0;
    uint64_t rootKey = 0;      // the c31 block of this instance this frame
    uint64_t rootKeyFrame = ~0ull;
    uint64_t poseFrame = 0;
    uint64_t lastSeenFrame = 0;
    uint64_t leanedFrame = ~0ull;
    // The model's own bone blocks, learned from the vertices it draws. Every
    // batch of the model raises it and no batch ever reaches past it, so the
    // palette is read exactly as far as the model goes - never into the previous
    // draw's leftovers.
    unsigned boneBound = 0;
    unsigned poseBound = 0;   // the bound the stored pose was taken with
    unsigned stable = 0;
    bool swaying = false;
};

// The last palette read, reused by every further batch of the same instance in
// the same frame. Keyed by the raw root block, so a different model - or the
// same model seen through another camera in this frame - always re-reads.
struct PaletteCache
{
    float registers[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4]{};
    uint64_t rootKey = 0;
    uint64_t frame = ~0ull;
    unsigned registersRead = 0;
    bool valid = false;
};

inline std::unordered_map<const void*, MeshProfile> g_meshes;
inline std::unordered_map<uint64_t, ShaderProfile> g_shaders;
inline std::unordered_map<uint64_t, GeometryProfile> g_geometry;
inline std::unordered_map<uint64_t, Instance> g_instances;
inline PaletteCache g_palette;
inline bool g_anySwaying = false;
inline bool g_reportedUnreadableGeometry = false;

inline renderer::VegetationStatus& Status() noexcept { return wind.Status(); }

inline void Configure(const renderer::VegetationSettings& next) noexcept
{
    settings = next;
    enabled = next.enabled;
    wind.Configure(next);
}

// One call per frame boundary, with the same snapshot the clouds and waves read.
inline void Update(const renderer::WindState& state) noexcept { wind.Update(state); }

// A new frame starts here: the per-frame counters and the "something already
// leans" shortcut are reset, and identities the client has not drawn for a while
// are dropped. Motion phases live in the field and the wind, never here, so a
// reset or an Alt+Tab cannot move anything.
inline void BeginFrame() noexcept
{
    wind.BeginFrame();
    g_anySwaying = false;
    const uint64_t frame = renderer::FrameContext::Current().frameIndex;
    const auto stale = [frame](uint64_t lastSeen) { return frame > lastSeen + kStaleFrames; };
    for (auto it = g_instances.begin(); it != g_instances.end();)
        it = stale(it->second.lastSeenFrame) ? g_instances.erase(it) : std::next(it);
    for (auto it = g_geometry.begin(); it != g_geometry.end();)
        it = stale(it->second.lastSeenFrame) ? g_geometry.erase(it) : std::next(it);
    for (auto it = g_meshes.begin(); it != g_meshes.end();)
        it = stale(it->second.lastSeenFrame) ? g_meshes.erase(it) : std::next(it);
    for (auto it = g_shaders.begin(); it != g_shaders.end();)
        it = stale(it->second.lastSeenFrame) ? g_shaders.erase(it) : std::next(it);
}

// A device reset drops every cached identity: the pointers, the palette and the
// pose history are all device resources or device observations.
inline void Reset(IDirect3DDevice9*) noexcept
{
    g_meshes.clear();
    g_shaders.clear();
    g_geometry.clear();
    g_instances.clear();
    g_palette = {};
    g_anySwaying = false;
    g_reportedUnreadableGeometry = false;
    probe.Clear();
}

// The optional HUD, drawn by the wind debug text only while a debug view asks
// for it: the state row and the gate row, both short enough to be readable in
// the HUD's own box. Empty otherwise, so a vegetation-off session shows exactly
// the HUD it showed before the feature existed.
inline std::vector<std::string> DebugLines()
{
    const renderer::FrameContext& frame = renderer::FrameContext::Current();
    if (!settings.debugView && !frame.wind.debugView) return {};
    return renderer::VegetationDebugLines(wind.Status(), settings, frame.wind);
}

namespace detail
{
    // Evict the least recently seen entry, so a cache can never grow with the
    // session. Every cache here is keyed by a live D3D9 identity, which is also
    // why each one is cleared on device reset.
    template<class Map>
    inline void Evict(Map& map, unsigned limit)
    {
        if (map.size() < limit) return;
        auto oldest = map.begin();
        for (auto it = map.begin(); it != map.end(); ++it)
            if (it->second.lastSeenFrame < oldest->second.lastSeenFrame) oldest = it;
        map.erase(oldest);
    }

    // The skinned M2 vertex layout, cached per declaration: one GetDeclaration
    // per declaration the client uses, and none afterwards.
    inline const renderer::M2VertexLayout* Layout(IDirect3DVertexDeclaration9* declaration, uint64_t frame)
    {
        if (!declaration) return nullptr;
        auto found = g_meshes.find(static_cast<const void*>(declaration));
        if (found != g_meshes.end())
        {
            found->second.lastSeenFrame = frame;
            return found->second.skinned ? &found->second.layout : nullptr;
        }
        if (g_meshes.size() >= kMaxMeshProfiles) Evict(g_meshes, kMaxMeshProfiles);
        D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
        UINT count = std::size(elements);
        MeshProfile profile;
        if (SUCCEEDED(declaration->GetDeclaration(elements, &count)))
            profile.skinned = renderer::ParseM2VertexLayout(elements, count, profile.layout);
        profile.lastSeenFrame = frame;
        auto inserted = g_meshes.emplace(static_cast<const void*>(declaration), profile).first;
        return inserted->second.skinned ? &inserted->second.layout : nullptr;
    }

    // Which vertex shader is this draw using? A variant this project captured
    // live is accepted on sight; a variant it has not seen has to bind the
    // client's M2 model layout in c2..c5, which is the same test the material
    // cache and the camera's sky fallback run on live draws. Anything this
    // project has already identified as never-a-model - the native shadow
    // casters first of all, whose palettes are in the LIGHT's space - is refused
    // before that read.
    //
    // Both verdicts about a shader this build had not seen are written to the
    // renderer's log once, by hash: an accepted one is a variant to capture so
    // the acceptance is instant next time, and a refused one is the answer to
    // "my plants do not sway" that a counter alone cannot give - the HUD says
    // how many draws `vs` refused, the log says which shader they were.
    inline void ReportShader(uint64_t hash, bool model, const char* reason) noexcept
    {
        char text[320]{};
        if (model)
            std::snprintf(text, sizeof(text),
                "Vegetation sway: accepted an M2 model vertex shader this build had not captured, hash=0x%016llx. "
                "Add it to kRigidM2Shaders in src/Lighting/TorchSignatures.h to accept it on sight.",
                static_cast<unsigned long long>(hash));
        else
            std::snprintf(text, sizeof(text),
                "Vegetation sway: a foliage vertex shader was refused (%s), hash=0x%016llx. If plants do not sway, "
                "this hash is the one to add to src/Lighting/TorchSignatures.h once it is verified.",
                reason, static_cast<unsigned long long>(hash));
        Log(text);
    }

    inline bool ShaderAccepted(IDirect3DDevice9* device, const renderer::FrameContext& frame, uint64_t hash,
        bool foliage)
    {
        if (renderer::IsRigidM2Shader(hash)) return true;
        auto found = g_shaders.find(hash);
        if (found != g_shaders.end())
        {
            ShaderProfile& profile = found->second;
            profile.lastSeenFrame = frame.frameIndex;
            if (!profile.model && !profile.reported && foliage)
            {
                profile.reported = true;
                ReportShader(hash, false, profile.reason);
            }
            return profile.model;
        }
        if (g_shaders.size() >= kMaxShaderProfiles) Evict(g_shaders, kMaxShaderProfiles);
        ShaderProfile profile;
        profile.lastSeenFrame = frame.frameIndex;
        if (hash == 0 || renderer::IsNonModelShader(hash))
        {
            profile.reason = hash == 0 ? "no vertex shader at all" : "a pass this project has identified as never a model";
        }
        else
        {
            float constants[6][4]{};
            profile.model = SUCCEEDED(device->GetVertexShaderConstantF(0, constants[0], 6)) &&
                renderer::IsM2ModelConstants(constants);
            profile.reason = "its constants do not bind the client's M2 model layout";
        }
        if (profile.model) ++Status().learnedShaders;
        else if (foliage)
        {
            profile.reported = true;
            ReportShader(hash, false, profile.reason);
        }
        g_shaders.emplace(hash, profile);
        if (profile.model) ReportShader(hash, true, nullptr);
        return profile.model;
    }

    // The draw has to render into the accepted camera's own viewport: that is
    // what says the palette and the projection the client bound belong to the
    // frame camera. The client's portrait, target, minimap and reflection views
    // are cameras of their own in viewports of their own, and a lean built in
    // their space would be a visibly broken model.
    inline bool MatchesFrameViewport(IDirect3DDevice9* device, const renderer::FrameContext& frame)
    {
        D3DVIEWPORT9 viewport{};
        if (FAILED(device->GetViewport(&viewport))) return false;
        return viewport.Width == frame.viewport.Width && viewport.Height == frame.viewport.Height;
    }

    // Identity of the raw c31 block. Two batches of one model in one frame
    // upload the same block, which is how the trunk finds the canopy's instance
    // without a second palette read.
    inline uint64_t RootKey(const float root[3][4]) noexcept
    {
        uint64_t key = 0xcbf29ce484222325ull;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
            {
                uint32_t bits = 0;
                std::memcpy(&bits, &root[row][column], sizeof(bits));
                key = renderer::VegetationHash64(key ^ bits);
            }
        return key;
    }

    // A batch's own geometry, read once. The key covers everything that decides
    // which vertices are read - the stream, the batch's place in it and the
    // layout - so a hit is the same vertices the client is drawing again, which
    // is what every frame of a standing model is.
    inline GeometryProfile* BatchGeometry(IDirect3DDevice9* device, const renderer::M2VertexLayout& layout,
        const renderer::M2DrawCall& draw, uint64_t frame)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IDirect3DVertexBuffer9> stream;
        UINT offset = 0, stride = 0;
        if (FAILED(device->GetStreamSource(0, stream.GetAddressOf(), &offset, &stride)) || !stream) return nullptr;
        uint64_t key = renderer::VegetationHash64(uint64_t(reinterpret_cast<uintptr_t>(stream.Get())) ^
            (uint64_t(stride) << 32) ^ uint64_t(offset));
        key = renderer::VegetationHash64(key ^ (uint64_t(draw.start) << 8) ^ (uint64_t(draw.primitives) << 32) ^
            (uint64_t(uint32_t(draw.base)) << 48) ^ (draw.indexed ? 0x9e3779b97f4a7c15ull : 0ull) ^
            (uint64_t(draw.minVertexIndex) << 20) ^ (uint64_t(draw.vertexCount) << 24) ^
            (uint64_t(layout.positionOffset) << 40) ^ (uint64_t(layout.boneOffset) << 44));
        auto found = g_geometry.find(key);
        if (found != g_geometry.end())
        {
            found->second.lastSeenFrame = frame;
            return found->second.geometry.valid ? &found->second : nullptr;
        }
        if (g_geometry.size() >= kMaxGeometryProfiles) Evict(g_geometry, kMaxGeometryProfiles);
        GeometryProfile profile;
        if (!renderer::ReadM2DrawGeometry(device, layout, draw, stream.Get(), offset, stride, profile.geometry))
            return nullptr;
        profile.lastSeenFrame = frame;
        return &g_geometry.emplace(key, profile).first->second;
    }

    // The batches of one model arrive in the client's own material order, so the
    // canopy that proved the model is swaying can be drawn after its opaque
    // trunk. An instance that leaned within the last couple of frames therefore
    // still owns those batches; one that stopped leaning (out of range, no
    // longer static, no longer drawn) owns nothing, because the range fade and
    // the drift guard are re-checked for every batch anyway.
    inline Instance* SwayingByRoot(uint64_t rootKey, uint64_t frame) noexcept
    {
        for (auto& entry : g_instances)
        {
            Instance& instance = entry.second;
            if (!instance.swaying || instance.rootKey != rootKey) continue;
            if (instance.rootKeyFrame == frame) return &instance;
            if (instance.leanedFrame != ~0ull && frame >= instance.leanedFrame &&
                frame - instance.leanedFrame <= 2)
                return &instance;
        }
        return nullptr;
    }

    inline Instance* InstanceFor(uint64_t key, uint64_t frame)
    {
        auto found = g_instances.find(key);
        if (found != g_instances.end())
        {
            found->second.lastSeenFrame = frame;
            return &found->second;
        }
        if (g_instances.size() >= kMaxInstances) Evict(g_instances, kMaxInstances);
        Instance fresh;
        fresh.key = key;
        fresh.lastSeenFrame = frame;
        return &g_instances.emplace(key, fresh).first->second;
    }

    // The client's own matrices, bounded to the model's own bones: a later batch
    // of the same instance sees the values the client uploaded, not the lean this
    // renderer already restored.
    inline bool ReadPalette(IDirect3DDevice9* device, uint64_t rootKey, uint64_t frame, unsigned bound,
        float (&out)[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4])
    {
        const unsigned registers = std::min(bound * renderer::kM2RegistersPerBone,
            renderer::kM2MaxBones * renderer::kM2RegistersPerBone);
        if (!registers) return false;
        if (g_palette.valid && g_palette.frame == frame && g_palette.rootKey == rootKey &&
            g_palette.registersRead >= registers)
        {
            std::memcpy(out, g_palette.registers, sizeof(float[4]) * registers);
            return true;
        }
        if (FAILED(device->GetVertexShaderConstantF(renderer::kM2BoneBaseRegister, out[0], registers)))
            return false;
        std::memcpy(g_palette.registers, out, sizeof(float[4]) * registers);
        g_palette.rootKey = rootKey;
        g_palette.frame = frame;
        g_palette.registersRead = registers;
        g_palette.valid = true;
        return true;
    }
}

// ---------------------------------------------------------------------------
// The batch as its hook received it
// ---------------------------------------------------------------------------
// One tiny constructor per device hook, so each of the four draw entry points
// stays a single line and no argument can be passed in the wrong order. The
// arguments are the hook's own, in the hook's own order.
inline renderer::M2DrawCall PlainDraw(D3DPRIMITIVETYPE type, UINT start, UINT primitives) noexcept
{
    renderer::M2DrawCall draw;
    draw.type = type;
    draw.start = start;
    draw.primitives = primitives;
    return draw;
}

inline renderer::M2DrawCall IndexedDraw(D3DPRIMITIVETYPE type, INT base, UINT minVertexIndex, UINT vertexCount,
    UINT start, UINT primitives) noexcept
{
    renderer::M2DrawCall draw;
    draw.type = type;
    draw.indexed = true;
    draw.base = base;
    draw.minVertexIndex = minVertexIndex;
    draw.vertexCount = vertexCount;
    draw.start = start;
    draw.primitives = primitives;
    return draw;
}

inline renderer::M2DrawCall PlainUpDraw(D3DPRIMITIVETYPE type, UINT primitives, const void* vertices,
    UINT stride) noexcept
{
    renderer::M2DrawCall draw;
    draw.type = type;
    draw.primitives = primitives;
    draw.verticesUP = vertices;
    draw.strideUP = stride;
    return draw;
}

inline renderer::M2DrawCall IndexedUpDraw(D3DPRIMITIVETYPE type, UINT minVertexIndex, UINT vertexCount,
    UINT primitives, const void* indices, D3DFORMAT format, const void* vertices, UINT stride) noexcept
{
    renderer::M2DrawCall draw = PlainUpDraw(type, primitives, vertices, stride);
    draw.indexed = true;
    draw.minVertexIndex = minVertexIndex;
    draw.vertexCount = vertexCount;
    draw.indicesUP = indices;
    draw.indexFormat = format;
    return draw;
}

// ---------------------------------------------------------------------------
// One draw: read, decide, lean, restore
// ---------------------------------------------------------------------------
struct Scope
{
    // `skip` follows the other effects' scopes: overlay and internal passes pass
    // true and are never touched. `draw` is the batch exactly as the client
    // issued it, so the sway can read the vertices that are about to be drawn.
    Scope(IDirect3DDevice9* device, bool skip, const renderer::M2DrawCall& draw)
    {
        m_device = device;
        if (skip || !device || !enabled || !active || !settings.enabled) return;
        const renderer::FrameContext& frame = renderer::FrameContext::Current();
        // The link and the effect switch come before anything is read at all: a
        // frame with the wind unlinked touches no device state, which is what
        // makes "off" indistinguishable from the renderer without the feature.
        if (!frame.wind.DrivesVegetation()) return;

        // A model drawn with a transparent material is a cutout: the leaves, the
        // grass, the cloth. The state mirror the device hooks maintain is the
        // only source for it, and it is read before any device call at all.
        const bool foliageMaterial =
            renderer::g_trackedState.alphaTest || renderer::g_trackedState.alphaBlend;

        // Which shader, and is it the client's model path? Which declaration, and
        // is it the client's skinned model layout? These two gates run for every
        // model draw, but the counters are incremented only for the draws that
        // could be foliage: a count polluted by the terrain, the UI and the WMOs
        // would be as unreadable as no count at all.
        //
        // An up-front batch ends here: it has no vertex buffer and therefore no
        // model identity, no palette bound and no placement point that outlives
        // the call. It is a particle, a ribbon or an interface quad - never a
        // model - so it is not counted as one.
        if (draw.verticesUP) return;
        if (!renderer::g_trackedState.currentVS) // fixed function is not the M2 path
        {
            if (foliageMaterial) ++Status().skippedShader;
            return;
        }
        if (!detail::ShaderAccepted(device, frame, renderer::g_trackedState.vsHash, foliageMaterial))
        {
            if (foliageMaterial) ++Status().skippedShader;
            return;
        }
        const renderer::M2VertexLayout* layout =
            detail::Layout(renderer::g_trackedState.currentVDecl, frame.frameIndex);
        if (!layout)
        {
            if (foliageMaterial) ++Status().skippedLayout;
            return;
        }

        // A model drawn with a transparent material is a cutout: the leaves, the
        // grass, the cloth. WoW draws its foliage either alpha tested or alpha
        // blended, so BOTH count, and this is where an INSTANCE comes from - a
        // solid draw cannot be a plant and never reveals one. Solid draws are
        // still carried by a model that is already leaning (a tree's own opaque
        // trunk, drawn before or after its canopy), which is why they continue
        // past this point while that shortcut holds.
        if (!foliageMaterial)
        {
            ++Status().skippedMaterial;
            if (!g_anySwaying) return;
        }
        if (!frame.cameraValid || !frame.viewRawValid)
        {
            if (foliageMaterial) ++Status().skippedCamera;
            return;
        }
        // The frame camera's own viewport, so the palette this draw binds is in
        // the space the lean is built in. A draw into another viewport (the F7
        // menu's own passes are scoped out already, the client's portrait and
        // reflection views are not) is left alone.
        if (!detail::MatchesFrameViewport(device, frame))
        {
            if (foliageMaterial) ++Status().skippedViewport;
            return;
        }

        // The root block is every model's first bone, and it is also the palette
        // cache key and the placement point: read once and reused.
        float root[3][4]{};
        if (FAILED(device->GetVertexShaderConstantF(renderer::kM2BoneBaseRegister, root[0],
                renderer::kM2RegistersPerBone)))
            return;
        if (!renderer::M2BoneBlockValid(root[0], root[1], root[2])) return;
        const uint64_t rootKey = detail::RootKey(root);

        if (!foliageMaterial)
        {
            Instance* known = detail::SwayingByRoot(rootKey, frame.frameIndex);
            if (!known) return;
            float registers[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4]{};
            const unsigned bound = std::max(known->boneBound, 1u);
            if (!detail::ReadPalette(device, rootKey, frame.frameIndex, bound, registers))
            {
                ++Status().unbounded;
                return;
            }
            const auto bones = renderer::AnalyzeVegetationBones(registers,
                bound * renderer::kM2RegistersPerBone, &frame.viewRaw.m[0][0], &frame.cameraPosition.x);
            if (!bones.valid)
            {
                ++Status().skippedPalette;
                probe.Observe(false, 0, frame.frameIndex);
                return;
            }
            Lean(frame, *known, bones, registers, bones.bones);
            return;
        }

        // The draw's own vertices: the model's bone bound, its model-space shape
        // and the mesh-move witness. Without them the palette cannot be bounded
        // and the model cannot be measured, so a draw whose vertices cannot be
        // read is left exactly as the client drew it.
        GeometryProfile* geometry = detail::BatchGeometry(device, *layout, draw, frame.frameIndex);
        if (!geometry)
        {
            ++Status().skippedGeometry;
            if (!g_reportedUnreadableGeometry)
            {
                g_reportedUnreadableGeometry = true;
                Log("Vegetation sway: a model draw's vertices could not be read, so its palette cannot be bounded and "
                    "its size cannot be measured. Such draws are left alone - the client's vertex buffer is not "
                    "lockable, or the batch is larger than 1 MB, or the draw is an up-front batch.");
            }
            return;
        }
        ++Status().candidates;

        // The model's own bones: what this batch's vertices reference, raised by
        // every other batch of the same model. The palette is read, analysed and
        // written only inside this bound - the registers above it hold the
        // previous draw's matrices and are never treated as bones.
        const renderer::M2DrawGeometry& model = geometry->geometry;
        float origin[3]{};
        if (!renderer::VegetationModelOrigin(root, &frame.viewRaw.m[0][0], &frame.cameraPosition.x, origin))
            return;
        float rootBasis[3][3]{};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
            {
                float value = 0.f;
                for (int k = 0; k < 3; ++k) value += frame.viewRaw.m[row][k] * root[k][column];
                rootBasis[row][column] = value;
            }
        Instance* instance = detail::InstanceFor(
            renderer::VegetationInstanceKey(origin, rootBasis, model.model), frame.frameIndex);
        if (!instance) return;
        const unsigned bound = std::max(model.bones, std::max(instance->boneBound, 1u));
        instance->boneBound = bound;

        float registers[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4]{};
        if (!detail::ReadPalette(device, rootKey, frame.frameIndex, bound, registers))
        {
            ++Status().unbounded;
            return;
        }
        const auto bones = renderer::AnalyzeVegetationBones(registers,
            bound * renderer::kM2RegistersPerBone, &frame.viewRaw.m[0][0], &frame.cameraPosition.x);
        if (!bones.valid)
        {
            ++Status().skippedPalette;
            probe.Observe(false, 0, frame.frameIndex);
            return;
        }
        probe.Observe(true, bones.bones, frame.frameIndex);
        if (bones.bones > renderer::kVegetationMaxBones)
        {
            // A full skeleton: character, creature, mount, weapon.
            ++Status().skippedShape;
            return;
        }

        // How big is it? The bones give a lower bound (a pivot sits INSIDE the
        // model), the draw's own vertices give the real answer - which is the only
        // answer a one-bone tuft has, because a single pivot spans no height at
        // all. Both are accumulated per instance, so the batches of one model
        // agree on one size instead of each measuring only its own share.
        const auto measured = renderer::MeasureVegetationGeometry(model.sample, model.samples, registers, bound,
            &frame.viewRaw.m[0][0], &frame.cameraPosition.x);
        const float height = std::max({ bones.height, measured.valid ? measured.SpanZ() : 0.f, instance->height });
        const float footprint = std::max({ bones.footprint, measured.valid ? measured.Footprint() : 0.f,
            instance->footprint });
        if (height < renderer::kVegetationMinModelHeight || height > renderer::kVegetationMaxModelHeight)
        {
            Status().lastMeasured = height;
            ++Status().skippedSize;
            return;
        }
        if (footprint >
            std::max(renderer::kVegetationFootprintToHeight * height, renderer::kVegetationMinFootprint))
        {
            Status().lastMeasured = height;
            ++Status().skippedShape;
            return;
        }
        Status().lastMeasured = height;
        instance->height = height;
        instance->footprint = footprint;
        instance->rootKey = rootKey;
        instance->rootKeyFrame = frame.frameIndex;
        for (int i = 0; i < 3; ++i) instance->pivot[i] = origin[i];

        // The animation test, in two halves. The mesh has to be where it was - a
        // particle or effect batch is re-uploaded with new vertices every frame
        // even when every matrix it binds is still - and the bones have to be
        // where they were, which is what keeps the client's animated models, its
        // characters, creatures, mounts and weapons out of the wind. Both are
        // published after the comparison, so the next frame reads what this one
        // measured instead of re-reading the client's buffer.
        const unsigned meshSamples = std::min(model.samples, kGeometryPoseSamples);
        const bool meshComparable = meshSamples > 0 && geometry->poseSamples == meshSamples &&
            geometry->poseFrame != 0 && frame.frameIndex > geometry->poseFrame &&
            frame.frameIndex - geometry->poseFrame <= 5;
        bool meshMoved = false, meshMeasured = false;
        for (unsigned i = 0; i < kGeometryPoseSamples; ++i)
        {
            float world[3]{};
            const bool finite = i < meshSamples &&
                renderer::VegetationVertexToWorld(model.sample[i], registers, bound, &frame.viewRaw.m[0][0],
                    &frame.cameraPosition.x, world);
            if (finite && meshComparable)
            {
                meshMeasured = true;
                const float* kept = geometry->poseWorld[i];
                meshMoved = meshMoved || std::fabs(world[0] - kept[0]) > renderer::kVegetationPoseTolerance ||
                    std::fabs(world[1] - kept[1]) > renderer::kVegetationPoseTolerance ||
                    std::fabs(world[2] - kept[2]) > renderer::kVegetationPoseTolerance;
            }
            for (int axis = 0; axis < 3; ++axis)
                geometry->poseWorld[i][axis] = finite ? world[axis] : 0.f;
        }
        geometry->poseSamples = meshSamples;
        geometry->poseFrame = frame.frameIndex;
        const bool meshStable = !meshMoved && !(meshComparable && !meshMeasured);

        // The palette's own history is only comparable between readings taken
        // with the same bound: a batch that revealed more of the skeleton has a
        // different pose signature, and comparing across it would read the
        // model's own size as motion. A new bound is a first reading again.
        const bool comparable = instance->pose.valid && instance->poseFrame != 0 && instance->poseBound == bound &&
            frame.frameIndex > instance->poseFrame && frame.frameIndex - instance->poseFrame <= 5;
        const bool sameFrame = instance->poseFrame == frame.frameIndex;
        const bool samePose = comparable &&
            renderer::VegetationPoseDelta(instance->pose, bones) <= renderer::kVegetationPoseTolerance;
        if (meshStable)
        {
            if (samePose) instance->stable = std::min(instance->stable + 1, kMaxStableFrames);
            else if (comparable)
            {
                // The client is moving it after all. It stops leaning immediately.
                ++Status().skippedAnimated;
                StopLeaning(*instance);
            }
            else if (!sameFrame || instance->poseBound != bound) instance->stable = 0;
        }
        else
        {
            // The client is re-uploading this mesh: an effect, not a plant.
            ++Status().skippedAnimated;
            StopLeaning(*instance);
        }
        if (!sameFrame || instance->poseBound != bound)
        {
            instance->pose = bones;
            instance->poseFrame = frame.frameIndex;
            instance->poseBound = bound;
        }
        if (instance->stable < 1 && !settings.animatedModels) return;

        instance->swaying = true;
        g_anySwaying = true;
        Lean(frame, *instance, bones, registers, bones.bones);
    }

    ~Scope()
    {
        // The client's own matrices go back immediately, in the same call shape
        // the lean used, so the next draw and any state the game applies
        // afterwards see exactly what it uploaded.
        if (m_wrote && m_device)
            m_device->SetVertexShaderConstantF(renderer::kM2BoneBaseRegister, m_saved[0], m_count);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    // A model that starts moving must stop leaning at once, and the shortcut that
    // lets its opaque batches lean has to be re-evaluated rather than left set
    // for the rest of the frame.
    void StopLeaning(Instance& instance) noexcept
    {
        instance.stable = 0;
        if (!instance.swaying) return;
        instance.swaying = false;
        g_anySwaying = false;
        for (const auto& entry : g_instances) g_anySwaying = g_anySwaying || entry.second.swaying;
    }

    // Lean every validated bone of the instance about its placement point. One
    // rotation for the whole model, exactly as VegetationWind.h documents: every
    // bone receives the same transform, so no two batches can be pulled apart,
    // the distance from the pivot is preserved (a bend, not a stretch) and the
    // tip travel is what the size class and the wind strength asked for.
    void Lean(const renderer::FrameContext& frame, Instance& instance, const renderer::VegetationBones& bones,
        const float (&registers)[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4], unsigned readBones)
    {
        // The palette's translations live in view space, so the pivot has to as
        // well: the same reconstruct-and-verify ladder the torch path uses, run
        // backwards. A view transform that does not round-trip (a scaled or
        // otherwise unexpected capture) ends the draw untouched.
        const renderer::Vec3 offset = { bones.pivot[0] - frame.cameraPosition.x,
            bones.pivot[1] - frame.cameraPosition.y, bones.pivot[2] - frame.cameraPosition.z };
        const renderer::Vec3 pivot = renderer::TransformDirectionRowVector(offset, frame.viewRaw.m);
        float recovered[3]{};
        renderer::ViewPositionToWorld(&frame.viewRaw.m[0][0], &frame.cameraPosition.x, &pivot.x, recovered);
        const float drift = std::sqrt((recovered[0] - bones.pivot[0]) * (recovered[0] - bones.pivot[0]) +
            (recovered[1] - bones.pivot[1]) * (recovered[1] - bones.pivot[1]) +
            (recovered[2] - bones.pivot[2]) * (recovered[2] - bones.pivot[2]));
        if (!std::isfinite(drift) || drift > 1.f) return;

        const float distance = renderer::Length(pivot);
        const auto sample = wind.Sample(bones.pivot[0], bones.pivot[1], instance.height,
            instance.key, distance);
        if (!sample.Leaning())
        {
            ++Status().skippedRange;
            return;
        }
        const renderer::Vec3 up = renderer::TransformDirectionRowVector({ 0.f, 0.f, 1.f }, frame.viewRaw.m);
        const renderer::Vec3 direction = renderer::TransformDirectionRowVector(
            { sample.directionX, sample.directionY, 0.f }, frame.viewRaw.m);
        const auto lean = renderer::BuildVegetationLean(&pivot.x, &up.x, &direction.x, sample.angleDegrees);
        if (lean.Identity()) return;

        const unsigned blocks = std::min(readBones, renderer::kM2MaxBones);
        if (!blocks) return;
        m_count = blocks * renderer::kM2RegistersPerBone;
        float leaned[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4]{};
        for (unsigned bone = 0; bone < blocks; ++bone)
        {
            // Bone i is the three registers starting at 3i: three ROWS of a
            // model -> view matrix, which is the shape the lean is applied to.
            const float (*block)[4] = &registers[bone * renderer::kM2RegistersPerBone];
            std::memcpy(m_saved + bone * renderer::kM2RegistersPerBone, block,
                renderer::kM2RegistersPerBone * sizeof(float[4]));
            float out[3][4]{};
            renderer::ApplyVegetationLean(lean, block, out);
            std::memcpy(leaned + bone * renderer::kM2RegistersPerBone, out,
                renderer::kM2RegistersPerBone * sizeof(float[4]));
        }
        if (FAILED(m_device->SetVertexShaderConstantF(renderer::kM2BoneBaseRegister, leaned[0], m_count))) return;
        m_wrote = true;
        Status().maxAngle = std::max(Status().maxAngle, sample.angleDegrees);
        Status().lastHeight = instance.height;
        if (instance.leanedFrame != frame.frameIndex)
        {
            instance.leanedFrame = frame.frameIndex;
            ++Status().swaying;
        }
        ++Status().batches;
    }

    IDirect3DDevice9* m_device = nullptr;
    unsigned m_count = 0;
    float m_saved[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4]{};
    bool m_wrote = false;
};
}
