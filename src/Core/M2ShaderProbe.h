#pragma once
// The client's M2 bone palette: one description, in one place.
//
// Every consumer that reads the skinning constants the client uploads for a
// model has to agree on where they live and what they look like. The layout was
// verified live on a real client for torch draws
// (src/Lighting/LocalLightManager.cpp): object -> view bone matrices, three
// vertex shader registers per bone - three rows of [basis | translation] - with
// the first bone at c31 and at most 75 bones. That is c31..c255, i.e. the rest
// of the constant file, which is exactly why a shader that is NOT the skinned
// M2 path cannot have anything of its own up there, and why the reader must
// prove each block before touching it.
//
// A "probe" here is what one read of those registers looked like: how many
// blocks could be object -> view matrices, and in which frame. It is recorded
// for the diagnostic log and for the regression suite. Nothing in this header
// touches a device, so the pinning test compiles and runs without d3d9.
#include <cstdint>
#include "../Lighting/TorchSignatures.h"

namespace renderer
{
    // The rigid skinned M2 vertex shaders this project has verified live (the
    // same captured list src/Lighting/TorchSignatures.h pins for torch draws).
    // They are the draws whose c31..c33 are an object -> view BONE PALETTE.
    // Everything else is left alone: the unskinned M2 variants keep a single
    // matrix in the same registers, and a shadow-depth, portrait, UI or
    // post-process shader has never been proven to hold a palette there - or to
    // share the frame camera's space, which is what makes the lean geometry
    // correct in the first place.
    inline bool IsRigidM2Shader(uint64_t hash) noexcept
    {
        for (uint64_t candidate : kRigidM2Shaders)
            if (candidate == hash) return true;
        return false;
    }

    inline constexpr unsigned kM2BoneBaseRegister = 31;
    inline constexpr unsigned kM2MaxBones = 75;
    inline constexpr unsigned kM2RegistersPerBone = 3;
    // View-space distance cap for a block's translation. The client's own far
    // clip is far below this, so a "translation" beyond it is not a camera
    // distance and the block is not a model matrix.
    inline constexpr float kM2BoneMaxLength = 4000.f;

    struct M2ShaderProbe
    {
        bool palette = false;  // the last read looked like object -> view blocks
        unsigned bones = 0;    // how many of them were usable
        unsigned reads = 0;    // lifetime count of accepted palette reads
        uint64_t sequence = 0; // frame sequence of the last read

        void Clear() noexcept
        {
            palette = false;
            bones = 0;
        }

        void Observe(bool looksLikePalette, unsigned boneCount, uint64_t frameSequence) noexcept
        {
            palette = looksLikePalette;
            bones = looksLikePalette ? boneCount : 0;
            sequence = frameSequence;
            if (looksLikePalette) ++reads;
        }
    };
}
