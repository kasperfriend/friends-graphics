#pragma once

// The structural half of the native-shadow enhancement: what a D3D9 SM2/SM3
// pixel shader's own instruction stream says about the two things the
// enhancement has to touch. Read-only, no d3d9.h, no Windows - so the rules
// here are compiled and exercised by the portable regression suite, and the
// renderer's copy of them cannot drift from the tested one.
//
// Why structure rather than a hash list: the enhancement started as a list of
// pixel-shader hashes whose constants had been disassembled by hand, and that
// list is exactly why it "does nothing" on a client that ships a receiver the
// list has never seen (Ascension updates its material shaders; the live log of
// such a session says "cascades bound at stage 4 but psHash=0x... is not in
// the receiver whitelist", once per session, forever). A hash says only "this
// is the shader somebody looked at". The bytecode can say what the shader
// DOES: that it samples the four native cascade slots, and which constants it
// adds to a texture coordinate as a two-component offset - which is what the
// softness slider scales.
//
// Both statements stay evidence, never guesses:
//   * a UV offset is a constant read with a swizzle that never touches .z or
//     .w (the kernel adds it to a texture coordinate's x/y), and
//   * a shadow darkening pair is a `def` literal shaped (darkening, floor, 0,
//     0) with darkening + floor == 1, which is the client's own
//     `visibility * darkening + floor` curve and nothing else in a receiver.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace renderer::shadowreceiver
{
// Instruction tokens, as the D3D9 byte format lays them out. SM2+ puts the
// instruction's extra-word count in bits 24..27 (the total is that plus the
// opcode word), which is how a stream can be walked without a full opcode
// table: every instruction, including the `comment` blocks a compiler emits,
// says how long it is.
inline constexpr std::uint32_t kOpcodeMask = 0xFFFFu;
inline constexpr unsigned kLengthShift = 24;
inline constexpr std::uint32_t kLengthMask = 0xFu;
inline constexpr std::uint16_t kOpcodeEnd = 0xFFFFu;
// A compiled function BEGINS with its version token - 0xFFFF0300 for ps_3_0,
// 0xFFFF0200 for ps_2_0, 0xFFFF0101 for ps_1_1 - which is not an instruction:
// its high half is all ones and its low half is major/minor. Read as one it
// looks like a fifteen-word instruction and the whole walk lands in the middle
// of real code, which is exactly how a first version of this scanner learned
// nothing from every shader it was shown.
inline constexpr std::uint32_t kVersionTokenMask = 0xFFFF0000u;
inline constexpr std::uint32_t kVersionTokenValue = 0xFFFF0000u;
inline constexpr std::uint16_t kOpcodeDef = 0x0051u;
// A comment block's payload is arbitrary words a compiler put there for human
// readers (the assembly listing). They are not operands and must never be read
// as any.
inline constexpr std::uint16_t kOpcodeComment = 0xFFFEu;

// Register tokens. The register TYPE is five bits, and they are not adjacent:
// bits 30:28 are its low three bits and bits 12:11 the high two (d3d9types.h's
// D3DSP_REGTYPE_MASK / D3DSP_REGTYPE_MASK2). That split is not a curiosity -
// it is the only thing that separates a constant (type 2, bit 11 clear) from a
// sampler (type 10, bit 11 set) when their low three bits agree, and mistaking
// one for the other is the difference between "this shader samples the
// cascades" and "this shader adds a constant to a UV".
inline constexpr std::uint32_t kRegisterMask = 0x7FFu;
inline constexpr unsigned kRegisterTypeShift = 28;
inline constexpr std::uint32_t kRegisterTypeMask = 0x7u;
inline constexpr std::uint32_t kRegisterTypeShift2 = 8;
inline constexpr std::uint32_t kRegisterTypeMask2 = 0x00001800u;
inline constexpr std::uint32_t kRegisterConstant = 2u;  // c0..c2047
inline constexpr std::uint32_t kRegisterSampler = 10u;
inline constexpr std::uint32_t kRegisterConstant2 = 11u; // c2048..c4095
inline constexpr std::uint32_t kRegisterConstant3 = 12u;
inline constexpr std::uint32_t kRegisterConstant4 = 13u;
// A source token's swizzle, two bits per component, x=0 y=1 z=2 w=3.
inline constexpr unsigned kSwizzleShift = 16;

struct DefLiteral
{
    std::uint32_t constant = 0;   // the cN the literal defines
    std::size_t dataWord = 0;     // index of its first float, in 32-bit words
    float value[4]{};             // the four floats as written by the compiler
};

struct Scan
{
    bool valid = false;             // the stream ended at END, not mid-instruction
    unsigned major = 0, minor = 0;  // from the function's version token, 0 when absent
    std::uint32_t xyConstants = 0;  // bit n: cN read with a swizzle that avoids .z and .w
    std::uint32_t anyConstants = 0; // bit n: cN read at all
    std::uint32_t samplerStages = 0;// bit n: sN declared or sampled
};

namespace detail
{
    inline std::uint32_t RegisterType(std::uint32_t token) noexcept
    {
        return ((token >> kRegisterTypeShift) & kRegisterTypeMask) |
               ((token & kRegisterTypeMask2) >> kRegisterTypeShift2);
    }

    inline std::uint32_t RegisterIndex(std::uint32_t token) noexcept
    {
        return token & kRegisterMask;
    }

    // Every component the shader reads from this source is .x or .y. A source
    // written as c5.xyxy (the common shape of a Poisson offset added to a UV)
    // qualifies; one read as a full vector, or with .z/.w anywhere, does not -
    // a shadow kernel's offsets are two-dimensional by construction, so a
    // three- or four-component constant is something else in the shader.
    inline bool ReadsOnlyXY(std::uint32_t token) noexcept
    {
        const unsigned z = (token >> (kSwizzleShift + 4)) & 3u;
        const unsigned w = (token >> (kSwizzleShift + 6)) & 3u;
        return z < 2u && w < 2u;
    }

    inline bool IsConstantToken(std::uint32_t token) noexcept
    {
        const std::uint32_t type = RegisterType(token);
        return type == kRegisterConstant || type == kRegisterConstant2 ||
               type == kRegisterConstant3 || type == kRegisterConstant4;
    }
}

// Walk one pixel shader's token stream. Records the constants it reads as
// two-component offsets and the sampler stages it samples; optionally hands
// back the `def` literals it declares (bounded by `maxLiterals`, the rest are
// still walked so the stream stays in step).
inline bool ScanBytecode(const std::uint32_t* code, std::size_t words, Scan& out,
                         DefLiteral* literals = nullptr, std::size_t maxLiterals = 0) noexcept
{
    out = Scan{};
    if (!code || words < 2) return false;
    std::size_t literalCount = 0;
    std::size_t start = 0;
    if ((code[0] & kVersionTokenMask) == kVersionTokenValue)
    {
        out.major = (code[0] >> 8) & 0xFFu;
        out.minor = code[0] & 0xFFu;
        start = 1;
    }
    for (std::size_t i = start; i < words;)
    {
        const std::uint32_t token = code[i];
        const std::uint16_t opcode = static_cast<std::uint16_t>(token & kOpcodeMask);
        if (opcode == kOpcodeEnd)
        {
            out.valid = true;
            break;
        }
        const std::size_t size = ((token >> kLengthShift) & kLengthMask) + 1;
        if (i + size > words) return false;
        if (opcode != kOpcodeDef && opcode != kOpcodeComment && size >= 2)
        {
            // A pixel shader NAMES its texture stages in `dcl`, in the
            // destination slot - and where that slot is depends on the
            // version: `dcl s0` (ps_2_0) is [opcode][dest], while
            // `dcl_2d s0` (ps_3_0) is [opcode][usage][dest], because the
            // usage token sits between them. Reading i+1 for both (the first
            // version of this scanner did) decodes a usage word as a register
            // and invents sampler stages that are not there.
            const std::size_t destOffset = size >= 3 && (out.major >= 3 || out.major == 0) ? 2 : 1;
            const std::uint32_t destination = code[i + destOffset];
            if (detail::RegisterType(destination) == kRegisterSampler)
            {
                const std::uint32_t index = detail::RegisterIndex(destination);
                if (index < 32) out.samplerStages |= (1u << index);
            }
        }
        if (opcode == kOpcodeDef)
        {
            // `def cN, x, y, z, w`: opcode, destination, then four raw floats.
            // The floats are never read as operands - a NaN-payload bit pattern
            // decodes as some register token or other, so a walk that looked at
            // them would invent constants that do not exist.
            if (size >= 6)
            {
                const std::uint32_t destination = code[i + 1];
                if (detail::IsConstantToken(destination))
                {
                    if (literals && literalCount < maxLiterals)
                    {
                        DefLiteral& literal = literals[literalCount];
                        literal.constant = detail::RegisterIndex(destination);
                        literal.dataWord = i + 2;
                        std::memcpy(literal.value, &code[i + 2], sizeof(literal.value));
                    }
                    ++literalCount;
                }
            }
        }
        else if (opcode == kOpcodeComment)
        {
            // Stepped over by its length; its payload is not shader code.
        }
        else for (std::size_t operand = 1; operand < size; ++operand)
        {
            // The first word is the destination and the rest are operands. A
            // destination token and a source token put the register number and
            // type in the same places but mean different things by bits 16-23
            // (write mask vs swizzle), so the destination is only ever asked
            // one question - is it a sampler? - which is the `dcl_2d sN` that
            // declares the cascade slots. Every other word is a source.
            const std::uint32_t word = code[i + operand];
            const std::uint32_t type = detail::RegisterType(word);
            const std::uint32_t index = detail::RegisterIndex(word);
            if (index >= 32) continue;
            if (type == kRegisterSampler) out.samplerStages |= (1u << index);
            else if (detail::IsConstantToken(word))
            {
                out.anyConstants |= (1u << index);
                if (detail::ReadsOnlyXY(word)) out.xyConstants |= (1u << index);
            }
        }
        i += size;
        // A stream whose last instruction ends exactly at the buffer's end is
        // complete even if no END token was written; anything else is not.
        if (i == words && !out.valid) out.valid = true;
    }
    return out.valid;
}

// How many of the native cascade slots (s4..s8 - the captured client binds
// its four cascades at s5 with one lower during some state transitions) this
// shader samples. Four is a receiver; fewer than three is some other material
// that happens to use a high texture slot.
inline unsigned CascadeStageCount(std::uint32_t samplerStages) noexcept
{
    unsigned count = 0;
    for (unsigned stage = 4; stage <= 8; ++stage)
        if (samplerStages & (1u << stage)) ++count;
    return count;
}

// The pair (darkening, floor) inside a `def` literal, if the literal is one:
// the client's own shadow curve, `result = visibility * darkening + floor`
// with darkening + floor == 1 (shadows never below `floor` brightness). The
// two shapes the verified receiver families use:
//   family A: def c8,  1.2, .3, .7, 0   -> pair at components 1,2
//   family B: def c12, .3, .7, 0, 0     -> pair at components 0,1
// Returns the index of the pair's first component, or -1. Nothing else in a
// receiver declares two complementary fractions that sum to one, so this is a
// shape test rather than a register guess - which is what lets the strength
// slider work on a receiver whose register layout nobody has disassembled.
// The third shape is the same pair written twice - (a, b, a, b) - which is what
// a compiler emits when the receiver feeds the curve to a `mad` over a
// two-component swizzle pair; the caller must then patch all four components,
// or the shader would read one scaled pair and one not.
inline bool ShadowReplicatedPair(const float value[4]) noexcept
{
    constexpr float epsilon = 1e-3f;
    return std::abs(value[2] - value[0]) <= epsilon && std::abs(value[3] - value[1]) <= epsilon;
}

inline int ShadowDarkeningPair(const float value[4]) noexcept
{
    constexpr float epsilon = 1e-3f;
    const auto fraction = [](float v) { return v > 0.f && v < 1.f; };
    if (value[2] == 0.f && value[3] == 0.f && fraction(value[0]) && fraction(value[1]) &&
        std::abs(value[0] + value[1] - 1.f) <= epsilon)
        return 0;
    if (value[3] == 0.f && fraction(value[1]) && fraction(value[2]) &&
        std::abs(value[1] + value[2] - 1.f) <= epsilon)
        return 1;
    if (fraction(value[0]) && fraction(value[1]) &&
        std::abs(value[0] + value[1] - 1.f) <= epsilon && ShadowReplicatedPair(value))
        return 0;
    return -1;
}
} // namespace renderer::shadowreceiver
