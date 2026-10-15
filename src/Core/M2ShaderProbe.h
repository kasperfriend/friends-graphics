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
#include <cmath>
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

    // Draws that must never be leaned, whatever their constants happen to look
    // like. The caster variants are the client's native shadow-map producers,
    // captured from a live client (docs/round6-native-shadow-findings.md): they
    // skin in the LIGHT's space, so a lean built from the frame camera's up and
    // downwind would be a visibly broken shadow rather than a missing one. UI,
    // precipitation and water are the other passes this project has already
    // identified; none of them is a model in the frame camera.
    inline constexpr uint64_t kNonModelShaders[] = {
        0xd9e7756460af6296ull, // UI vertex shader
        0xc29c7060b723c0c6ull, // UI pixel shader
        0x8832bcc5ed4ff2a6ull, // native rain/sandstorm vertex shader
        0xec9ee827b8bf5afaull, // native snow vertex shader
        0x202b452e580a7742ull, 0x8535271f53b044b4ull, 0xea99ea307a197a84ull, // native shadow casters
        0x4f20b99325ed9c47ull, 0x78641e03a615f46aull, // their pixel shaders
        0x206d861fd0a721ddull, 0xfdd9528ed3ac30eaull, 0x48a82796bd612aebull,
        0x70faf83955e2b668ull // liquid vertex variants
    };

    // The captured unskinned model/particle path (src/Lighting/TorchSignatures.h):
    // the same client draws, but with ONE matrix at c31..c33 instead of a bone
    // palette. Those variants are known M2 model draws, so an unskinned draw can
    // be admitted on them - and its whole model is then leaned by that one
    // matrix, which is exactly right for a single-pivot tuft.
    inline bool IsUnskinnedM2Shader(uint64_t hash) noexcept
    {
        for (uint64_t candidate : kUnskinnedM2Shaders)
            if (candidate == hash) return true;
        return false;
    }

    inline bool IsNonModelShader(uint64_t hash) noexcept
    {
        for (uint64_t candidate : kNonModelShaders)
            if (candidate == hash) return true;
        return false;
    }

    // The client's M2 model layout, as live captures describe it: the model path
    // binds its projection in c2..c5 (row-major) instead of the c4..c7 the
    // terrain and WMO paths use. This project already decides "this draw is an M2
    // model" from exactly these six registers in two verified places
    // (src/Scene/DrawCallClassifier.cpp, and CameraCapture's sky fallback), so an
    // unknown M2 variant can be recognized by the block the client bound for it
    // instead of by a hash this project happened to capture somewhere else.
    inline bool IsM2ModelConstants(const float constants[6][4]) noexcept
    {
        if (!constants) return false;
        for (int row = 0; row < 6; ++row)
            for (int column = 0; column < 4; ++column)
                if (!std::isfinite(constants[row][column])) return false;
        return std::fabs(constants[5][2] - 1.0f) < 0.02f &&
            std::fabs(constants[5][0]) < 0.01f && std::fabs(constants[5][1]) < 0.01f &&
            std::fabs(constants[5][3]) < 0.01f &&
            constants[2][0] > 0.1f && constants[3][1] > 0.1f;
    }

    // The other two layouts this client's world draws use, read exactly the way
    // src/Scene/DrawCallClassifier.cpp reads them on live draws: WMO objects and
    // terrain keep their projection in c4..c7, with c6[3] == 1 and c7[2] < 0.
    // (That is also why a reader that wants to tell them apart from a model has
    // to look at nine registers, not six.) A draw whose constants are one of
    // these is a piece of the world, not a model, and must never be leaned.
    inline bool IsWmoOrTerrainConstants(const float constants[9][4]) noexcept
    {
        if (!constants) return false;
        for (int row = 0; row < 9; ++row)
            for (int column = 0; column < 4; ++column)
                if (!std::isfinite(constants[row][column])) return false;
        return constants[4][0] > 0.1f && constants[5][1] > 0.1f &&
            std::fabs(constants[6][3] - 1.0f) < 0.02f && constants[7][2] < -0.01f;
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
