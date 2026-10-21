// Native-shadow regressions: what a pixel shader's own bytecode says about the
// two things the enhancement touches, and the rules that decide whether a draw
// may be touched at all.
//
//   g++ -O2 -std=c++20 -Wall -Wextra -Werror -pedantic tools/ShadowRegression.cpp -o build/shadow-test
//   ./build/shadow-test
//
// The report this was written for: "Native shadows don't work at all." The
// live log of such a session says why in two lines -
//
//   [SOFTNESS] ACTIVE: scaled registers c3..10 by 1.45x for psHash=0x449b... cascadeStage=-1
//   [SOFTNESS] cascades bound at stage 4 but psHash=0xbeec8ff97f375ed1 is not in the receiver whitelist
//
// - a shader that is not a receiver was being scaled (no cascades were bound at
// its draws), while the shader that WAS drawn with the four cascades bound was
// refused because the enhancement only knew the handful of hashes somebody had
// disassembled by hand. A client update that recompiles its material shaders
// therefore turns both sliders off, permanently and silently.
//
// The rules pinned here replace "somebody looked at this hash" with two
// statements the shader can prove about itself:
//   * it samples the native cascade window (a receiver), and
//   * which constants it reads as two-component UV offsets (what softness
//     scales), or which `def` literal is its darkening/floor curve (what
//     strength patches).
// Both are shape tests over the instruction stream, and every one of them is
// exercised here with hand-built tokens rather than against a driver, because
// the token layout is the thing that has to be right.
#include "../src/Effects/ShadowReceiverBytecode.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    namespace bc = renderer::shadowreceiver;

    void Require(bool value, const char* name)
    {
        if (!value) throw std::runtime_error(name);
        std::printf("PASS %s\n", name);
    }

    std::string Read(const char* path)
    {
        std::ifstream in(path);
        if (!in) throw std::runtime_error(std::string("Missing ") + path);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // ---- token builders (the documented D3D9 layout) ------------------------
    // Register type is five bits: 30:28 low three, 12:11 high two.
    constexpr std::uint32_t kTemp = 0, kConst = 2, kSampler = 10;
    constexpr std::uint32_t opDcl = 0x1Fu, opDef = 0x51u, opMov = 0x01u,
                            opAdd = 0x02u, opTexld = 0x42u, opEnd = 0xFFFFu,
                            opComment = 0xFFFEu;

    std::uint32_t Inst(std::uint32_t opcode, std::uint32_t extraWords)
    {
        return (extraWords << 24) | opcode;
    }

    std::uint32_t Reg(std::uint32_t type, std::uint32_t index)
    {
        return ((type & 7u) << 28) | ((type & 0x18u) << 8) | index;
    }

    // A source operand: swizzle lives in bits 16..23, two bits per channel.
    std::uint32_t Src(std::uint32_t type, std::uint32_t index, std::uint32_t swizzle = 0xE4u)
    {
        return Reg(type, index) | (swizzle << 16);
    }

    std::uint32_t Dst(std::uint32_t type, std::uint32_t index, std::uint32_t writeMask = 0xFu)
    {
        return Reg(type, index) | (writeMask << 16);
    }

    std::uint32_t FloatWord(float value)
    {
        std::uint32_t word = 0;
        std::memcpy(&word, &value, 4);
        return word;
    }

    // A receiver in the shape the client emits: four cascades declared and
    // sampled, one Poisson offset per tap, and the darkening curve in a `def`.
    std::vector<std::uint32_t> Receiver(
        const std::vector<std::uint32_t>& offsetConstants = { 5, 6, 7, 8 },
        std::uint32_t samplerStage = 4,
        bool declareSamplers = true)
    {
        std::vector<std::uint32_t> code;
        if (declareSamplers)
            for (std::uint32_t stage = samplerStage; stage < samplerStage + 4; ++stage)
            {
                code.push_back(Inst(opDcl, 1));
                code.push_back(Reg(kSampler, stage));
            }
        for (std::uint32_t tap = 0; tap < 4; ++tap)
        {
            code.push_back(Inst(opTexld, 3));
            code.push_back(Dst(kTemp, 0));
            code.push_back(Src(3 /*texture*/, 0));
            code.push_back(Src(kSampler, samplerStage + tap));
        }
        for (std::uint32_t constant : offsetConstants)
        {
            code.push_back(Inst(opAdd, 3));
            code.push_back(Dst(kTemp, 1, 0x3u));            // .xy
            code.push_back(Src(kTemp, 0));
            code.push_back(Src(kConst, constant, 0x44u));   // .xyxy
        }
        code.push_back(Inst(opMov, 2));
        code.push_back(Dst(kTemp, 2));
        code.push_back(Src(kConst, 12));                    // .xyzw: not an offset
        code.push_back(Inst(opDef, 5));
        code.push_back(Dst(kConst, 12));
        for (float value : { .3f, .7f, 0.f, 0.f }) code.push_back(FloatWord(value));
        code.push_back(Inst(opEnd, 0));
        return code;
    }

    bc::Scan ScanOf(const std::vector<std::uint32_t>& code)
    {
        bc::Scan scan;
        bc::ScanBytecode(code.data(), code.size(), scan);
        return scan;
    }
}

int main() try
{
    std::printf("-- what a receiver's bytecode says about itself --\n");
    {
        const auto code = Receiver();
        bc::Scan scan;
        bc::DefLiteral literals[4];
        Require(bc::ScanBytecode(code.data(), code.size(), scan, literals, 4),
                "a well-formed instruction stream is walked to its END token");
        Require(scan.samplerStages & (1u << 4) && scan.samplerStages & (1u << 5) &&
                    scan.samplerStages & (1u << 6) && scan.samplerStages & (1u << 7),
                "the four cascades an s4-first receiver declares and samples are all seen");
        Require(bc::CascadeStageCount(scan.samplerStages) == 4,
                "four sampled cascade slots is what makes a shader a receiver");
        Require((scan.xyConstants & (1u << 5)) && (scan.xyConstants & (1u << 6)) &&
                    (scan.xyConstants & (1u << 7)) && (scan.xyConstants & (1u << 8)),
                "the four constants the kernel adds to a UV as .xyxy are the offsets the softness slider may scale");
        Require((scan.xyConstants & (1u << 12)) == 0,
                "a constant read as a full vector is not a UV offset, whatever register it lives in");
        Require(literals[0].constant == 12 && literals[0].dataWord == code.size() - 5,
                "the def literal is reported with the register it defines and where its floats live");
        Require(bc::ShadowDarkeningPair(literals[0].value) == 0,
                "the client's own .3/.7 darkening curve is recognised in that literal");

        // The second captured family declares its samplers five slots up.
        const auto high = ScanOf(Receiver({ 13, 14 }, 5));
        Require(bc::CascadeStageCount(high.samplerStages) == 4 && (high.xyConstants & (1u << 14)),
                "a receiver whose cascades sit at s5..s8 is found the same way - the stage is the client's choice, not a constant here");

        // A REAL FUNCTION BEGINS WITH ITS VERSION TOKEN. 0xFFFF0300 (ps_3_0)
        // is not an instruction; read as one its length field says fifteen
        // words and the walk lands in the middle of the shader - which is how
        // the first version of this scanner learned nothing at all.
        auto versioned = Receiver();
        versioned.insert(versioned.begin(), 0xFFFF0300u);
        const auto versionedScan = ScanOf(versioned);
        Require(versionedScan.valid && bc::CascadeStageCount(versionedScan.samplerStages) == 4 &&
                    (versionedScan.xyConstants & (1u << 5)),
                "a ps_3_0 version token is stepped over, and the instructions behind it are read as instructions");
        auto psTwo = Receiver();
        psTwo.insert(psTwo.begin(), 0xFFFF0200u);
        const auto psTwoScan = ScanOf(psTwo);
        Require(psTwoScan.valid && bc::CascadeStageCount(psTwoScan.samplerStages) == 4,
                "a ps_2_0 version token is stepped over the same way");
        Require(versionedScan.major == 3 && versionedScan.minor == 0 &&
                    psTwoScan.major == 2 && psTwoScan.minor == 0,
                "the version token is reported, because where a dcl keeps its register depends on it");

        // ps_3_0 writes `dcl_2d s4` as [opcode][usage][dest]. Reading the
        // usage word as the destination (the first version of this scanner
        // did) invents a sampler stage out of a semantic name: live, that is
        // "samples 5 cascade slots" for a shader that samples three.
        std::vector<std::uint32_t> declOnly;
        declOnly.push_back(0xFFFF0300u);
        for (std::uint32_t stage = 0; stage < 4; ++stage)
        {
            declOnly.push_back(Inst(opDcl, 2));
            declOnly.push_back(0x00000005u);              // D3DDECLUSAGE_TEXCOORD
            declOnly.push_back(Dst(kSampler, stage));
        }
        declOnly.push_back(Inst(opEnd, 0));
        const auto declScan = ScanOf(declOnly);
        Require(declScan.samplerStages == 0xFu,
                "a ps_3_0 dcl names its sampler in the word AFTER the usage token, or s0..s3 would read as s4..s7");

        // A shader that samples nothing but ordinary material textures is not a
        // receiver, even though it reads a constant the same way.
        auto material = Receiver({ 5 }, 0);
        const auto materialScan = ScanOf(material);
        Require(bc::CascadeStageCount(materialScan.samplerStages) == 0,
                "a material shader that samples s0..s3 is not a receiver");

        // `def` data is floats, never operands. A float whose bit pattern happens
        // to decode as a constant source must not invent one: the walk skips the
        // four data words of every def instruction.
        auto poisoned = Receiver();
        poisoned[poisoned.size() - 4] = Src(kConst, 9, 0x44u);
        const auto poisonedScan = ScanOf(poisoned);
        Require((poisonedScan.xyConstants & (1u << 9)) == 0,
                "a def's float payload is never read as an operand");

        // Comment blocks carry arbitrary words; the length field walks over them.
        std::vector<std::uint32_t> commented = { Inst(opComment, 2), Src(kConst, 21, 0x44u),
                                                 Src(kConst, 22, 0x44u) };
        const auto body = Receiver();
        commented.insert(commented.end(), body.begin(), body.end());
        const auto commentedScan = ScanOf(commented);
        Require((commentedScan.xyConstants & (1u << 21)) == 0 &&
                    (commentedScan.xyConstants & (1u << 22)) == 0 &&
                    commentedScan.valid,
                "a comment block is stepped over by its length, and its words are not operands");

        // A stream that ends mid-instruction is refused outright rather than
        // half-believed.
        std::vector<std::uint32_t> truncated = { Inst(opAdd, 3), Dst(kTemp, 1) };
        bc::Scan bad;
        Require(!bc::ScanBytecode(truncated.data(), truncated.size(), bad) && !bad.valid,
                "a truncated stream is refused, not walked past its end");
        Require(!bc::ScanBytecode(nullptr, 0, bad) && !bad.valid,
                "an empty or missing stream is refused");

        // A stream whose last instruction ends exactly at the buffer's end is
        // complete even without an END token.
        auto noEnd = Receiver();
        noEnd.pop_back();
        const auto noEndScan = ScanOf(noEnd);
        Require(noEndScan.valid && bc::CascadeStageCount(noEndScan.samplerStages) == 4,
                "a stream that ends exactly on an instruction boundary is complete, END token or not");

        // Swizzle shapes: only x/y readings count.
        auto xyOnly = Receiver({ 5, 6, 7, 8 });
        for (std::uint32_t constant = 0; constant < 4; ++constant)
        {
            xyOnly.insert(xyOnly.end() - 1, Inst(opAdd, 3));
            xyOnly.insert(xyOnly.end() - 1, Dst(kTemp, 3, 0x3u));
            xyOnly.insert(xyOnly.end() - 1, Src(kTemp, 0));
            // .xyzw, .xyzz, .xywy: the last two read a component above y.
            const std::uint32_t swizzle = constant == 0 ? 0xE4u : (constant == 1 ? 0xA4u : 0x64u);
            xyOnly.insert(xyOnly.end() - 1, Src(kConst, 16u + constant, swizzle));
        }
        const auto swizzleScan = ScanOf(xyOnly);
        Require((swizzleScan.xyConstants & (1u << 16)) == 0 &&
                    (swizzleScan.xyConstants & (1u << 17)) == 0 &&
                    (swizzleScan.xyConstants & (1u << 18)) == 0,
                "a constant read through .z or .w is not a two-component offset");
    }

    std::printf("\n-- the darkening curve is found by its values, not its register --\n");
    {
        const float familyA[4] = { 1.2f, .3f, .7f, 0.f };
        const float familyB[4] = { .3f, .7f, 0.f, 0.f };
        const float even[4] = { .5f, .5f, 0.f, 0.f };
        const float notCurve[4] = { .3f, .4f, 0.f, 0.f };
        const float opaque[4] = { 1.f, 0.f, 0.f, 0.f };
        const float unrelated[4] = { .3f, .7f, .1f, 0.f };
        Require(bc::ShadowDarkeningPair(familyA) == 1,
                "family A's (1.2, .3, .7, 0) declares its pair at components 1 and 2");
        Require(bc::ShadowDarkeningPair(familyB) == 0,
                "family B's (.3, .7, 0, 0) declares it at components 0 and 1");
        Require(bc::ShadowDarkeningPair(even) == 0,
                "any complementary pair counts, which is what lets an unseen receiver's curve be patched");
        Require(bc::ShadowDarkeningPair(notCurve) == -1 && bc::ShadowDarkeningPair(opaque) == -1 &&
                    bc::ShadowDarkeningPair(unrelated) == -1,
                "fractions that do not sum to one, a fully opaque curve, or a third component are not the curve");
        // A pair written twice: what a compiler emits when the curve feeds a
        // mad over a two-component swizzle pair. Both copies must move.
        const float replicated[4] = { .3f, .7f, .3f, .7f };
        Require(bc::ShadowDarkeningPair(replicated) == 0 && bc::ShadowReplicatedPair(replicated),
                "a complementary pair written twice is still the curve");
        const float notReplicated[4] = { .3f, .7f, .3f, .2f };
        Require(bc::ShadowDarkeningPair(notReplicated) == -1,
                "four non-zero components that are not two copies of one pair are not the curve");
    }

    std::printf("\n-- the renderer uses these rules, and only on proven receivers --\n");
    {
        const std::string shadow = Read("NativeShadowDiagnostics.h");
        Require(shadow.find("#include \"src/Effects/ShadowReceiverBytecode.h\"") != std::string::npos,
                "the renderer shares the scanner the portable suite compiles");
        Require(shadow.find("inline const LearnedReceiver* LearnReceiver(") != std::string::npos &&
                    shadow.find("learned = LearnReceiver(device, psHash, trackedShader);") != std::string::npos,
                "an unknown receiver is learned from its bytecode instead of being refused for its hash");
        Require(shadow.find("renderer::shadowreceiver::CascadeStageCount(scan.samplerStages)") != std::string::npos,
                "learning asks the shader how many cascade slots it samples");
        Require(shadow.find("const unsigned probe = CascadeStageProbe(device);") != std::string::npos,
                "and asks the device whether the cascades were actually bound at that draw");
        Require(shadow.find("if (!scan.xyConstants || (cascades < 3 && !probe))") != std::string::npos,
                "a shader has to pass both statements before anything is scaled");
        Require(shadow.find("ApplySoftnessMask(psHash, learned->xyConstants)") != std::string::npos,
                "a learned receiver is scaled on exactly the constants its bytecode reads as offsets");
        Require(shadow.find("LooksLikeUvOffset(") != std::string::npos &&
                    shadow.find("static bool loggedMaskMiss") != std::string::npos,
                "every draw still has to hold a value that looks like an offset before it is touched");
        // The hash that was scaled with no cascades bound: the live log's
        // "cascadeStage=-1" line. It may not be back on the softness list.
        const auto familyB = shadow.substr(shadow.find("static constexpr uint64_t kFamilyB[]"),
                                           shadow.find("};", shadow.find("static constexpr uint64_t kFamilyB[]")) -
                                               shadow.find("static constexpr uint64_t kFamilyB[]"));
        Require(familyB.find("0x449b66c5fb94fede") == std::string::npos,
                "a shader observed being scaled with no cascades bound is not on the softness list any more");
        Require(familyB.find("0xac726e53bca0ac1a") != std::string::npos &&
                    familyB.find("0x08b17d1abaad8ece") != std::string::npos,
                "the verified families keep their entries");
        Require(shadow.find("IsFamilyAStrengthHash") == std::string::npos,
                "strength no longer needs a per-hash register layout: the literal is found by its values");
        Require(shadow.find("renderer::shadowreceiver::ShadowDarkeningPair(literal.value)") != std::string::npos,
                "the patcher asks the shared rule which literal is the shadow curve");
        Require(shadow.find("inline bool IsCascadeShaped(const D3DSURFACE_DESC& desc)") != std::string::npos &&
                    shadow.find("desc.Width != desc.Height || desc.Width < 1024 || desc.Width > 4096")
                        != std::string::npos,
                "the cascade probe asks for a square depth target of cascade size, not one exact format");
        Require(shadow.find("for (int firstStage = 4; firstStage <= 8; ++firstStage)") != std::string::npos,
                "the probe knows the cascades can sit one slot lower than the capture first saw them");
        // A session where the sliders "do nothing" has to leave a reason behind.
        Require(shadow.find("inline void LogShadowDecline(") != std::string::npos &&
                    shadow.find("LogShadowDecline(\"toggle\"") != std::string::npos &&
                    shadow.find("LogShadowDecline(\"neutral\"") != std::string::npos &&
                    shadow.find("LogShadowDecline(\"offscreen\"") != std::string::npos &&
                    shadow.find("LogShadowDecline(\"bound\"") != std::string::npos &&
                    shadow.find("LogShadowDecline(\"caster\"") != std::string::npos,
                "every gate the enhancement can stop at names itself, once per session");
        Require(shadow.find("inline void NoteShadowDraw(") != std::string::npos &&
                    shadow.find("NoteShadowDraw(softnessActive, strengthShaderSwapped || strengthConstantTouched);")
                        != std::string::npos,
                "and every receiver draw is counted, so a log says whether anything was ever changed");
        Require(shadow.find("declares no darkening/floor literal") != std::string::npos,
                "a strength slider that cannot patch its receiver says so");
        // The bytecode a session actually hands over begins with a version
        // token; without stepping over it the walk reads fifteen words of real
        // code as an instruction.
        const std::string scanner = Read("src/Effects/ShadowReceiverBytecode.h");
        Require(scanner.find("kVersionTokenMask") != std::string::npos &&
                    scanner.find("if ((code[0] & kVersionTokenMask) == kVersionTokenValue)") != std::string::npos &&
                    scanner.find("start = 1;") != std::string::npos,
                "the scanner steps over the function's version token before reading instructions");
        Require(shadow.find("fallback.xyConstants = 0xFFFFu;") != std::string::npos &&
                    shadow.find("without readable bytecode") != std::string::npos,
                "a receiver whose bytecode cannot be read still gets the bounded window, with the value test as the guard");
        // THE GROUND. The receivers that shade terrain declared no curve
        // literal at all, so the strength slider could only darken the object
        // shadows - the report this round answers. A receiver whose curve is a
        // CONSTANT is found by the same complementary-pair rule, and only when
        // exactly one of its constants matches.
        Require(shadow.find("bool ApplyStrengthConstant(uint64_t psHash)") != std::string::npos &&
                    shadow.find("renderer::shadowreceiver::ShadowDarkeningPair(constants[r])") != std::string::npos,
                "a receiver that keeps its shadow curve in a constant is darkened too");
        Require(shadow.find("if (matches != 1)") != std::string::npos &&
                    shadow.find("left alone rather than guessed between") != std::string::npos,
                "two complementary pairs in one receiver is not a choice this code makes");
        Require(shadow.find("const float darkeningRegister = std::min(native[0], native[1]);") != std::string::npos,
                "the darkening is the smaller half of the pair - scaling the floor would brighten shadows instead");
        Require(shadow.find("if (strengthConstantActive) device->SetPixelShaderConstantF(strengthConstantRegister, strengthConstantOriginal, 1);")
                    != std::string::npos,
                "a constant that was changed for one draw is put back for the next");
        // The literal buffer was 8: a terrain-family shader declares far more
        // defs than that, so a curve late in the stream was never looked at.
        Require(shadow.find("renderer::shadowreceiver::DefLiteral literals[64];") != std::string::npos,
                "every def in a big shader is looked at, not the first eight");
        // And a shader with no curve at all is answered once, not per draw.
        Require(shadow.find("if (entry.attempted && !entry.hasLiteral) return nullptr;") != std::string::npos,
                "a receiver with no curve literal is not re-read on every one of its draws");
        // Softness, when the bytecode's mask holds no offset in this draw: the
        // live log showed a whole session with "0 softened" for that reason.
        Require(shadow.find("void ApplySoftnessScan(uint64_t psHash)") != std::string::npos &&
                    shadow.find("searching the low constant file instead") != std::string::npos &&
                    shadow.find("GetPixelShaderConstantF(0, original[0], 32)") != std::string::npos,
                "when the declared constants hold no offset, the low constant file is searched by value instead");
        // Evidence for the next family: the shader itself, in the log and on disk.
        Require(shadow.find("[SHADOW-DUMP] psHash=0x") != std::string::npos &&
                    shadow.find("D3DDisassemble(bytecode.data(), bytecode.size(),") != std::string::npos &&
                    shadow.find("ps_%016llx.asm") != std::string::npos,
                "a receiver neither slider can fully handle is described in the log and written out as assembly");
    }

    std::printf("\nALL SHADOW TESTS PASSED\n");
    return 0;
}
catch (const std::exception& error)
{
    std::printf("FAIL %s\n", error.what());
    return 1;
}
