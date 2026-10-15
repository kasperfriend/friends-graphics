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
// leaves the draw exactly as the client intended it:
//   1. the shared wind is on, the vegetation link is on, the effect is on, and
//      the frame has an accepted camera (cameraValid + viewRawValid);
//   2. the draw uses a vertex declaration with POSITION/FLOAT3, BLENDINDICES
//      and BLENDWEIGHT, and one of the rigid skinned M2 vertex shaders this
//      project verified live for torch draws (src/Lighting/TorchSignatures.h).
//      No declaration, an FVF draw, a fixed-function draw, an unskinned model
//      variant or an unproven shader is not the client's palette path;
//   3. the palette at c31 looks like object -> view matrices (finite,
//      orthogonal up to scale, plausible translation). Nothing past the first
//      block that does not is ever written, and a rejected palette ends the
//      draw untouched;
//   4. the model is plant-shaped: a skeleton of at most kMaxVegetationBones
//      bones (characters, creatures, mounts and weapons carry far more), tall
//      enough to be worth leaning and not longer than it is tall, which is what
//      separates a bush from a wall or a fence;
//   5. it is inside DistanceYards of the camera, with the sway fading to
//      nothing before the limit so a tree never pops between leaning and still;
//   6. the material is transparent (alpha tested or blended). A solid draw is
//      not foliage; it can still be leaned once its own model is known, which is
//      what keeps a tree's opaque trunk welded to its canopy;
//   7. the client is NOT animating it. This is what keeps characters, creatures
//      and mounts out of the wind, and it is measured rather than guessed: the
//      pose read this frame has to equal the pose read last frame. The client's
//      static doodads repeat exactly (the signature is camera independent), its
//      animated models never do.
//
// Batches of one model stay coherent because the decision belongs to the
// INSTANCE - its world ground contact and orientation - and not to the batch:
// the opaque trunk of a tree follows the alpha-tested canopy that revealed it,
// even when the client draws the trunk first.
//
// The sway adds no shader constant of its own: it reads the client's palette,
// writes the leaned blocks into the same registers, and restores them. That is
// why the disabled path can be asserted as "no Get/SetVertexShaderConstantF".
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <wrl/client.h>
#include "../Core/FrameContext.h"
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

// Device-side bookkeeping only: the model-shape gates themselves are policy and
// live in VegetationWind.h (renderer::kVegetationMaxBones, renderer::kVegetationMinModelHeight,
// renderer::kVegetationMaxModelHeight, renderer::kVegetationMinFootprint,
// renderer::kVegetationFootprintToHeight, renderer::kVegetationPoseTolerance), where the portable
// suite can pin them.
inline constexpr unsigned kMaxStableFrames = 8;
inline constexpr uint64_t kStaleFrames = 30;
inline constexpr unsigned kMaxMeshProfiles = 512;
inline constexpr unsigned kMaxInstances = 1024;

// One vertex-declaration layout, cached by identity like every other live D3D9
// object in this project. The client keeps a handful of declarations, so this
// only ever holds a few entries.
struct MeshProfile
{
    bool skinned = false;
    uint64_t lastSeenFrame = 0;
};

// One placed model. `key` is its identity (world ground contact, skeleton size
// and orientation) and doubles as the per-instance phase seed, so two tufts a
// yard apart never shimmer in lockstep.
struct Instance
{
    float pivot[3]{};
    float height = 0.f;
    float footprint = 0.f;
    renderer::VegetationBones pose;
    uint64_t key = 0;
    uint64_t rootKey = 0;      // the c31 block of this instance this frame
    uint64_t rootKeyFrame = ~0ull;
    uint64_t poseFrame = 0;
    uint64_t lastSeenFrame = 0;
    uint64_t leanedFrame = ~0ull;
    unsigned stable = 0;
    bool swaying = false;
};

// The last full palette read, reused by every further batch of the same
// instance in the same frame. Keyed by the raw root block, so a different
// model - or the same model seen through another camera in this frame - always
// re-reads.
struct PaletteCache
{
    float registers[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4]{};
    uint64_t rootKey = 0;
    uint64_t frame = ~0ull;
    bool valid = false;
};

inline std::unordered_map<const void*, MeshProfile> g_meshes;
inline std::unordered_map<uint64_t, Instance> g_instances;
inline PaletteCache g_palette;
inline bool g_anySwaying = false;

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
// leans" shortcut are reset, and instances the client has not drawn for a while
// are dropped. Motion phases live in the field and the wind, never here, so a
// reset or an Alt+Tab cannot move anything.
inline void BeginFrame() noexcept
{
    wind.BeginFrame();
    g_anySwaying = false;
    const uint64_t frame = renderer::FrameContext::Current().frameIndex;
    for (auto it = g_instances.begin(); it != g_instances.end();)
        it = frame > it->second.lastSeenFrame + kStaleFrames ? g_instances.erase(it) : std::next(it);
}

// A device reset drops every cached identity: the pointers, the palette and the
// pose history are all device resources or device observations.
inline void Reset(IDirect3DDevice9*) noexcept
{
    g_meshes.clear();
    g_instances.clear();
    g_palette = {};
    g_anySwaying = false;
    probe.Clear();
}

// One optional HUD line, drawn by the wind debug text only while [Vegetation]
// DebugView is on. Empty otherwise, so a vegetation-off session shows exactly
// the HUD it showed before the feature existed.
inline std::string DebugLine()
{
    if (!settings.debugView) return {};
    return renderer::VegetationDebugLine(wind.Status(), settings, renderer::FrameContext::Current().wind);
}

namespace detail
{
    // The skinned M2 vertex layout, cached per declaration. The declaration is
    // already tracked by the device hooks, so this needs no device call.
    inline bool Skinned(IDirect3DVertexDeclaration9* declaration, uint64_t frame)
    {
        if (!declaration) return false;
        auto found = g_meshes.find(static_cast<const void*>(declaration));
        if (found != g_meshes.end())
        {
            found->second.lastSeenFrame = frame;
            return found->second.skinned;
        }
        if (g_meshes.size() >= kMaxMeshProfiles)
        {
            auto oldest = g_meshes.begin();
            for (auto it = g_meshes.begin(); it != g_meshes.end(); ++it)
                if (it->second.lastSeenFrame < oldest->second.lastSeenFrame) oldest = it;
            g_meshes.erase(oldest);
        }
        D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
        UINT count = std::size(elements);
        MeshProfile profile;
        if (SUCCEEDED(declaration->GetDeclaration(elements, &count)))
        {
            bool position = false, indices = false, weights = false;
            for (UINT i = 0; i < count; ++i)
            {
                const D3DVERTEXELEMENT9& element = elements[i];
                if (element.Stream != 0) continue;
                if (element.Usage == D3DDECLUSAGE_POSITION && element.Type == D3DDECLTYPE_FLOAT3)
                    position = true;
                if (element.Usage == D3DDECLUSAGE_BLENDINDICES &&
                    (element.Type == D3DDECLTYPE_UBYTE4 || element.Type == D3DDECLTYPE_D3DCOLOR))
                    indices = true;
                if (element.Usage == D3DDECLUSAGE_BLENDWEIGHT) weights = true;
            }
            profile.skinned = position && indices && weights;
        }
        profile.lastSeenFrame = frame;
        return g_meshes.emplace(static_cast<const void*>(declaration), profile).first->second.skinned;
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
            const Instance& instance = entry.second;
            if (!instance.swaying || instance.rootKey != rootKey) continue;
            if (instance.rootKeyFrame == frame) return &entry.second;
            if (instance.leanedFrame != ~0ull && frame >= instance.leanedFrame &&
                frame - instance.leanedFrame <= 2)
                return &entry.second;
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
        if (g_instances.size() >= kMaxInstances)
        {
            auto oldest = g_instances.begin();
            for (auto it = g_instances.begin(); it != g_instances.end(); ++it)
                if (it->second.lastSeenFrame < oldest->second.lastSeenFrame) oldest = it;
            g_instances.erase(oldest);
        }
        Instance fresh;
        fresh.key = key;
        fresh.lastSeenFrame = frame;
        return &g_instances.emplace(key, fresh).first->second;
    }

    // The client's own matrices: a later batch of the same instance sees the
    // values the client uploaded, not the lean this renderer already restored.
    inline bool ReadPalette(IDirect3DDevice9* device, uint64_t rootKey, uint64_t frame,
        float (&out)[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4])
    {
        if (g_palette.valid && g_palette.frame == frame && g_palette.rootKey == rootKey)
        {
            std::memcpy(out, g_palette.registers, sizeof(g_palette.registers));
            return true;
        }
        if (FAILED(device->GetVertexShaderConstantF(renderer::kM2BoneBaseRegister, out[0],
                renderer::kM2MaxBones * renderer::kM2RegistersPerBone)))
            return false;
        std::memcpy(g_palette.registers, out, sizeof(g_palette.registers));
        g_palette.rootKey = rootKey;
        g_palette.frame = frame;
        g_palette.valid = true;
        return true;
    }
}

// ---------------------------------------------------------------------------
// One draw: read, decide, lean, restore
// ---------------------------------------------------------------------------
struct Scope
{
    // `skip` follows the other effects' scopes: overlay and internal passes pass
    // true and are never touched.
    Scope(IDirect3DDevice9* device, bool skip)
    {
        m_device = device;
        if (skip || !device || !enabled || !active || !settings.enabled) return;
        const renderer::FrameContext& frame = renderer::FrameContext::Current();
        if (!frame.wind.DrivesVegetation() || !frame.cameraValid || !frame.viewRawValid) return;

        // A model drawn with a transparent material is a cutout: the leaves, the
        // grass, the cloth. WoW draws its foliage either alpha tested or alpha
        // blended, so BOTH count; a solid draw cannot be a plant and never
        // reveals an instance. Solid draws (a tree's own opaque trunk, for
        // example) are only leaned once their model is known, which is why they
        // do not have to pay for the palette read at all until then.
        const bool foliageMaterial = renderer::g_trackedState.alphaTest || renderer::g_trackedState.alphaBlend;
        if (!foliageMaterial && !g_anySwaying) return;
        if (!renderer::g_trackedState.currentVS) return; // fixed function is not the M2 path
        // A verified rigid skinned M2 shader, i.e. a draw whose c31..c33 this
        // project has proven to be an object -> view bone palette in the frame
        // camera's own space. An unlisted variant - the client's shadow-depth,
        // portrait, UI or post shaders - is never touched.
        if (!renderer::IsRigidM2Shader(renderer::g_trackedState.vsHash)) return;
        if (!detail::Skinned(renderer::g_trackedState.currentVDecl, frame.frameIndex)) return;

        float root[3][4]{};
        if (FAILED(device->GetVertexShaderConstantF(renderer::kM2BoneBaseRegister, root[0], renderer::kM2RegistersPerBone)))
            return;
        if (!renderer::M2BoneBlockValid(root[0], root[1], root[2])) return;
        const uint64_t rootKey = detail::RootKey(root);

        if (!foliageMaterial)
        {
            Instance* known = detail::SwayingByRoot(rootKey, frame.frameIndex);
            if (!known) return;
            float registers[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4]{};
            if (!detail::ReadPalette(device, rootKey, frame.frameIndex, registers)) return;
            const auto bones = renderer::AnalyzeVegetationBones(registers,
                renderer::kM2MaxBones * renderer::kM2RegistersPerBone, &frame.viewRaw.m[0][0], &frame.cameraPosition.x);
            if (!bones.valid)
            {
                ++Status().skippedPalette;
                probe.Observe(false, 0, frame.frameIndex);
                return;
            }
            Lean(frame, *known, bones, registers, bones.bones);
            return;
        }

        ++Status().candidates;
        float registers[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4]{};
        if (!detail::ReadPalette(device, rootKey, frame.frameIndex, registers)) return;
        const auto bones = renderer::AnalyzeVegetationBones(registers,
            renderer::kM2MaxBones * renderer::kM2RegistersPerBone, &frame.viewRaw.m[0][0], &frame.cameraPosition.x);
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
        if (bones.height < renderer::kVegetationMinModelHeight || bones.height > renderer::kVegetationMaxModelHeight)
        {
            ++Status().skippedSize;
            return;
        }
        if (bones.footprint >
            std::max(renderer::kVegetationFootprintToHeight * bones.height, renderer::kVegetationMinFootprint))
        {
            ++Status().skippedShape;
            return;
        }

        Instance* instance = detail::InstanceFor(renderer::VegetationInstanceKey(bones), frame.frameIndex);
        if (!instance) return;
        instance->height = bones.height;
        instance->footprint = bones.footprint;
        instance->rootKey = rootKey;
        instance->rootKeyFrame = frame.frameIndex;
        for (int i = 0; i < 3; ++i) instance->pivot[i] = bones.pivot[i];

        // The animation test. A static doodad repeats its pose to within the
        // reconstruction error of two world reads; the client's animated models
        // never do. A stale reading (the instance was not drawn for a while) is
        // a first reading again, so a hidden model cannot be approved on old
        // evidence - and a second batch of the SAME frame is the same reading,
        // not a disagreement, so it neither approves nor resets anything.
        const bool sameFrame = instance->poseFrame == frame.frameIndex;
        const bool comparable = instance->pose.valid && instance->poseFrame != 0 &&
            frame.frameIndex > instance->poseFrame && frame.frameIndex - instance->poseFrame <= 5;
        if (comparable && renderer::VegetationPoseDelta(instance->pose, bones) <= renderer::kVegetationPoseTolerance)
            instance->stable = std::min(instance->stable + 1, kMaxStableFrames);
        else if (comparable)
        {
            // The client is moving it after all. It stops leaning immediately.
            if (instance->swaying)
            {
                instance->swaying = false;
                g_anySwaying = false;
                for (const auto& entry : g_instances) g_anySwaying = g_anySwaying || entry.second.swaying;
            }
            ++Status().skippedAnimated;
            instance->stable = 0;
        }
        else if (!sameFrame) instance->stable = 0;
        if (!sameFrame)
        {
            instance->pose = bones;
            instance->poseFrame = frame.frameIndex;
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
    // Lean every validated bone of the instance about its world ground contact.
    // One rotation for the whole model, exactly as VegetationWind.h documents:
    // every bone receives the same transform, so no two batches can be pulled
    // apart, the distance from the pivot is preserved (a bend, not a stretch)
    // and the tip travel is what the size class and the wind strength asked for.
    void Lean(const renderer::FrameContext& frame, Instance& instance, const renderer::VegetationBones& bones,
        const float (&registers)[renderer::kM2MaxBones * renderer::kM2RegistersPerBone][4], unsigned readBones)
    {
        // The palette's translations live in view space, so the pivot has to as
        // well: the same reconstruct-and-verify ladder the torch path uses,
        // run backwards. A view transform that does not round-trip (a scaled or
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
            std::memcpy(m_saved + bone * renderer::kM2RegistersPerBone, block, renderer::kM2RegistersPerBone * sizeof(float[4]));
            float out[3][4]{};
            renderer::ApplyVegetationLean(lean, block, out);
            std::memcpy(leaned + bone * renderer::kM2RegistersPerBone, out, renderer::kM2RegistersPerBone * sizeof(float[4]));
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
