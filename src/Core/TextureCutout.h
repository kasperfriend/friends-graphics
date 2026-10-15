#pragma once
// Is this draw's own texture a cut-out?
//
// The material test the sway has always used asks the DEVICE two questions: is
// alpha blending on, is alpha testing on. On a client that marks its foliage
// materials that way the answer is enough. On a client whose shader system
// compiles the alpha test INTO the pixel shader - a `discard` on the sampled
// alpha, which is what WoW's shader path does - both states are off while grass
// is being drawn, and every plant reads as a solid prop. That is the report
// this file exists to answer: a field full of grass, `238 solid` on the HUD,
// and nothing leaning.
//
// The texture is the evidence that cannot be turned off that way. A leaf, a
// blade of grass and a canopy are drawn as cards with a cut-out texture: the
// pixels outside the leaf are transparent and the shader discards them. A
// crate, a boulder, a piece of armour and a lamp post are drawn with a texture
// that has no transparent texel at all - if it did, they would have holes in
// them. One look at the texture the draw is using therefore separates the two,
// on any client, with no shader list and no manifest.
//
// The read itself (a level 0 lock) is the expensive half and lives on the
// device side. Everything here is a pure function over the bytes that read
// returned, which is what makes every format below testable in the portable
// suite - and a transparent texel must never be invented out of a decoder bug,
// because a false "cut-out" now means a solid prop in the wind.
#include <cstddef>
#include <cstdint>

namespace renderer
{
    // The formats a WoW client uploads. Anything else is Unknown: a format this
    // reader cannot decode is never proof of a cut-out, so the draw is left
    // exactly as the client drew it.
    enum class TextureFormat
    {
        Unknown,
        Dxt1,      // 8 bytes per 4x4 block, 1-bit alpha when colour0 <= colour1
        Dxt2,      // 4-bit explicit alpha, premultiplied
        Dxt3,      // 4-bit explicit alpha
        Dxt4,      // interpolated alpha, premultiplied
        Dxt5,      // interpolated alpha
        A8R8G8B8,  // 4 bytes: B, G, R, A
        X8R8G8B8,  // 4 bytes, no alpha at all
        A4R4G4B4,  // 2 bytes, alpha in the high nibble
        A1R5G5B5,  // 2 bytes, alpha in the top bit
        A8,        // 1 byte, alpha only
        L8         // 1 byte, luminance only
    };

    enum class TextureCutout
    {
        Unknown, // not read, not decodable, or too little of it was read
        Opaque,  // no meaningful transparency: a solid material
        Cutout   // transparent texels: this texture is cut out, not drawn solid
    };

    struct TextureAlphaProbe
    {
        TextureCutout verdict = TextureCutout::Unknown;
        uint64_t hash = 0;        // FNV-1a over the bytes that were examined
        unsigned texels = 0;      // texels examined, never padding
        unsigned transparent = 0; // how many of those are transparent
        unsigned green = 0;       // opaque texels that are green-dominant
        float fraction = 0.f;     // transparent / texels

        bool readable() const noexcept { return verdict != TextureCutout::Unknown; }
        // green / (texels - transparent): what share of the visible texture is
        // green. Zero when nothing was read.
        float greenFraction() const noexcept
        {
            const unsigned opaque = texels > transparent ? texels - transparent : 0u;
            return opaque ? float(green) / float(opaque) : 0.f;
        }
        // A FOLIAGE texture: a cut-out whose visible texels are largely green - a
        // leaf, a blade of grass, a canopy. This is the second, positive half of
        // the material gate, and the D3D-side answer to the question the WXL
        // modules answer with a model's file name: skin, metal, cloth, leather,
        // a saddle, a shoulder pad and a sword are cut out as rarely as they are
        // green, and when they are (a ragged cloak) they are not green either.
        bool foliage = false;
    };

    // A texture is a cut-out when at least this share of it is transparent. A
    // foliage texture is mostly holes (its leaves sit in a corner of it), while
    // a solid texture with one transparent row along an edge is not foliage.
    inline constexpr unsigned kTextureCutoutPercent = 5;
    // Below this many examined texels the share means nothing - and a texture
    // that small is not the texture of a plant in any case.
    inline constexpr unsigned kTextureAlphaMinTexels = 256;
    // One texture is examined up to this many bytes. A 512x512 DXT5 is 256 KiB
    // and is read whole; anything larger is sampled across the whole image by
    // taking every k-th row, so a transparent corner is found wherever it is.
    inline constexpr size_t kTextureAlphaScanBytes = 256u * 1024u;
    // What "green" means, in the same spirit as the cut-out share: the green
    // channel has to lead both others by a margin, and be bright enough to be a
    // plant and not a dark shadow of one.
    inline constexpr unsigned kTextureGreenLeadPercent = 108; // green >= 1.08x red and blue
    inline constexpr unsigned kTextureGreenFloor = 40;        // of 255
    // And this much of the visible texture has to be green before it is foliage.
    inline constexpr unsigned kTextureFoliageGreenPercent = 25;

    inline constexpr uint64_t kTextureAlphaHashOffset = 0xcbf29ce484222325ull;
    inline constexpr uint64_t kTextureAlphaHashPrime = 0x100000001b3ull;

    namespace detail
    {
        inline uint16_t ReadU16(const uint8_t* p) noexcept
        {
            return static_cast<uint16_t>(unsigned(p[0]) | (unsigned(p[1]) << 8));
        }

        // The alpha of one texel of an unpacked format, as 0..255.
        inline unsigned AlphaOfTexel(TextureFormat format, const uint8_t* texel) noexcept
        {
            switch (format)
            {
            case TextureFormat::A8R8G8B8: return texel[3];
            case TextureFormat::A4R4G4B4: return ((ReadU16(texel) >> 12) & 0xF) * 17;
            case TextureFormat::A1R5G5B5: return (ReadU16(texel) & 0x8000) ? 255u : 0u;
            case TextureFormat::A8: return texel[0];
            default: return 255u; // X8R8G8B8, L8: no alpha channel at all
            }
        }

        inline unsigned TexelBytes(TextureFormat format) noexcept
        {
            switch (format)
            {
            case TextureFormat::A8R8G8B8:
            case TextureFormat::X8R8G8B8: return 4;
            case TextureFormat::A4R4G4B4:
            case TextureFormat::A1R5G5B5: return 2;
            case TextureFormat::A8:
            case TextureFormat::L8: return 1;
            default: return 0;
            }
        }

        inline unsigned BlockBytes(TextureFormat format) noexcept
        {
            switch (format)
            {
            case TextureFormat::Dxt1: return 8;
            case TextureFormat::Dxt2:
            case TextureFormat::Dxt3:
            case TextureFormat::Dxt4:
            case TextureFormat::Dxt5: return 16;
            default: return 0;
            }
        }

        inline bool IsCompressed(TextureFormat format) noexcept { return BlockBytes(format) != 0; }

        // Is one decoded texel a plant's colour? Green leading both other
        // channels and bright enough to be lit foliage. Alpha is not looked at
        // here: the caller only asks about texels that are not transparent.
        inline bool IsGreenTexel(unsigned r, unsigned g, unsigned b) noexcept
        {
            return g >= kTextureGreenFloor && g * 100 >= r * kTextureGreenLeadPercent &&
                g * 100 >= b * kTextureGreenLeadPercent;
        }

        // The four colours of one DXT colour block, in the format's own order:
        // the two endpoints, then the two interpolated entries - which depend on
        // which endpoint is larger, exactly as the block's transparency does.
        inline void DxtColourBlock(const uint8_t* block, unsigned char rgb[16][3]) noexcept
        {
            const uint16_t c0 = ReadU16(block), c1 = ReadU16(block + 2);
            const auto expand = [](uint16_t c, unsigned char out[3]) {
                const unsigned r = (c >> 11) & 0x1Fu, g = (c >> 5) & 0x3Fu, b = c & 0x1Fu;
                out[0] = static_cast<unsigned char>((r << 3) | (r >> 2));
                out[1] = static_cast<unsigned char>((g << 2) | (g >> 4));
                out[2] = static_cast<unsigned char>((b << 3) | (b >> 2));
            };
            unsigned char entry[4][3]{};
            expand(c0, entry[0]);
            expand(c1, entry[1]);
            for (int i = 0; i < 3; ++i)
            {
                if (c0 > c1)
                {
                    entry[2][i] = static_cast<unsigned char>((2 * unsigned(entry[0][i]) + unsigned(entry[1][i])) / 3);
                    entry[3][i] = static_cast<unsigned char>((unsigned(entry[0][i]) + 2 * unsigned(entry[1][i])) / 3);
                }
                else
                {
                    entry[2][i] = static_cast<unsigned char>((unsigned(entry[0][i]) + unsigned(entry[1][i])) / 2);
                    entry[3][i] = 0;
                }
            }
            uint32_t indices = 0;
            for (int i = 0; i < 4; ++i) indices |= uint32_t(block[4 + i]) << (8 * i);
            for (int i = 0; i < 16; ++i)
            {
                const unsigned char* from = entry[(indices >> (2 * i)) & 3u];
                rgb[i][0] = from[0];
                rgb[i][1] = from[1];
                rgb[i][2] = from[2];
            }
        }

        inline void BlockColourIndices(TextureFormat format, const uint8_t* block,
            unsigned char rgb[16][3]) noexcept
        {
            if (format == TextureFormat::Dxt4 || format == TextureFormat::Dxt5)
                DxtColourBlock(block + 8, rgb); // the eight alpha bytes come first
            else
                DxtColourBlock(block, rgb);
        }

        // The colour of one unpacked texel, as 0..255 per channel. Luminance-only
        // formats read as grey, which is never green: a luminance texture is not
        // a leaf.
        inline void TexelColour(TextureFormat format, const uint8_t* texel, unsigned char rgb[3]) noexcept
        {
            switch (format)
            {
            case TextureFormat::A8R8G8B8:
            case TextureFormat::X8R8G8B8:
                rgb[0] = texel[2];
                rgb[1] = texel[1];
                rgb[2] = texel[0];
                return;
            case TextureFormat::A4R4G4B4:
            {
                const uint16_t value = ReadU16(texel);
                rgb[0] = static_cast<unsigned char>(((value >> 8) & 0xF) * 17);
                rgb[1] = static_cast<unsigned char>(((value >> 4) & 0xF) * 17);
                rgb[2] = static_cast<unsigned char>((value & 0xF) * 17);
                return;
            }
            case TextureFormat::A1R5G5B5:
            {
                const uint16_t value = ReadU16(texel);
                rgb[0] = static_cast<unsigned char>((((value >> 10) & 0x1F) * 255) / 31);
                rgb[1] = static_cast<unsigned char>((((value >> 5) & 0x1F) * 255) / 31);
                rgb[2] = static_cast<unsigned char>(((value & 0x1F) * 255) / 31);
                return;
            }
            default: // A8, L8 and anything else: grey
                rgb[0] = rgb[1] = rgb[2] = texel[0];
                return;
            }
        }

        // One 4x4 block, as "is this texel transparent" in row-major order. The
        // 4-bit and interpolated blocks are decoded from the format's own
        // description, never guessed: a texel is transparent below alpha 128,
        // which is the same threshold every one of these formats is compared to.
        inline void BlockTransparency(TextureFormat format, const uint8_t* block, bool transparent[16]) noexcept
        {
            for (int i = 0; i < 16; ++i) transparent[i] = false;
            if (format == TextureFormat::Dxt1)
            {
                // 1-bit alpha, and only in the mode where colour0 <= colour1: the
                // other mode is a fully opaque block by definition.
                if (ReadU16(block) > ReadU16(block + 2)) return;
                const uint32_t indices = uint32_t(block[4]) | (uint32_t(block[5]) << 8) |
                    (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
                for (int i = 0; i < 16; ++i) transparent[i] = ((indices >> (2 * i)) & 3u) == 3u;
                return;
            }
            if (format == TextureFormat::Dxt2 || format == TextureFormat::Dxt3)
            {
                // Eight bytes of explicit 4-bit alpha, two texels per byte.
                for (int i = 0; i < 16; ++i)
                {
                    const unsigned nibble = (block[i >> 1] >> ((i & 1) * 4)) & 0xF;
                    transparent[i] = nibble * 17 < 128;
                }
                return;
            }
            // DXT4/DXT5: two endpoints and sixteen 3-bit indices, interpolated
            // exactly as the format describes - including its two modes, which
            // are the difference between "0 is transparent" and "6 and 7 are".
            const unsigned a0 = block[0], a1 = block[1];
            uint64_t indices = 0;
            for (int i = 0; i < 6; ++i) indices |= uint64_t(block[2 + i]) << (8 * i);
            unsigned alpha[8]{};
            alpha[0] = a0;
            alpha[1] = a1;
            if (a0 > a1)
            {
                for (unsigned i = 2; i < 8; ++i) alpha[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
            }
            else
            {
                for (unsigned i = 2; i < 6; ++i) alpha[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
                alpha[6] = 0;
                alpha[7] = 255;
            }
            for (int i = 0; i < 16; ++i)
                transparent[i] = alpha[(indices >> (3 * i)) & 7u] < 128;
        }
    }

    // Examine a locked level 0 image. `pitch` is the byte distance between two
    // rows (D3D's own locked pitch: rows of texels for an unpacked format, rows
    // of 4x4 blocks for a compressed one), and `available` is how many bytes the
    // lock actually handed over - so a decoder can never read past it.
    inline TextureAlphaProbe ClassifyTextureCutout(TextureFormat format, const uint8_t* pixels, size_t pitch,
        unsigned width, unsigned height, size_t available) noexcept
    {
        using namespace detail;
        TextureAlphaProbe probe;
        if (!pixels || !pitch || !width || !height || format == TextureFormat::Unknown) return probe;

        const bool compressed = IsCompressed(format);
        const size_t unitBytes = compressed ? BlockBytes(format) : TexelBytes(format);
        const unsigned unitTexels = compressed ? 4u : 1u; // a block covers 4 texels across
        if (!unitBytes) return probe;
        const unsigned unitsPerRow = compressed ? (width + 3) / 4 : width;
        const unsigned rowCount = compressed ? (height + 3) / 4 : height;
        const size_t rowBytes = size_t(unitsPerRow) * unitBytes;
        if (!rowBytes) return probe;

        // As many rows as the budget allows, spread across the whole image: the
        // transparent half of a foliage texture is not always at the top.
        const size_t budgetRows = kTextureAlphaScanBytes / rowBytes;
        unsigned step = 1;
        if (budgetRows && budgetRows < rowCount)
            step = static_cast<unsigned>((rowCount + budgetRows - 1) / budgetRows);

        uint64_t hash = kTextureAlphaHashOffset;
        for (unsigned row = 0; row < rowCount; row += step)
        {
            const size_t offset = size_t(row) * pitch;
            if (offset >= available) break;
            const size_t room = available - offset;
            const size_t read = room < pitch ? room : pitch;
            const uint8_t* base = pixels + offset;
            if (read < rowBytes) break;
            for (size_t i = 0; i < rowBytes; ++i)
            {
                hash ^= base[i];
                hash *= kTextureAlphaHashPrime;
            }
            for (unsigned unit = 0; unit < unitsPerRow; ++unit)
            {
                const uint8_t* data = base + size_t(unit) * unitBytes;
                bool transparent[16]{};
                unsigned char colour[16][3]{};
                if (compressed)
                {
                    BlockTransparency(format, data, transparent);
                    BlockColourIndices(format, data, colour);
                }
                // Padding is never counted: a texture whose width is not a
                // multiple of four would otherwise look transparent at its edge,
                // and an opaque crate would read as a leaf.
                for (unsigned j = 0; j < (compressed ? 4u : 1u); ++j)
                {
                    const unsigned y = compressed ? row * 4 + j : row;
                    if (y >= height) break;
                    for (unsigned i = 0; i < unitTexels; ++i)
                    {
                        const unsigned x = compressed ? unit * 4 + i : unit;
                        if (x >= width) break;
                        const bool clear = compressed ? transparent[j * 4 + i]
                            : AlphaOfTexel(format, data) < 128;
                        ++probe.texels;
                        if (clear)
                        {
                            ++probe.transparent;
                            continue;
                        }
                        // Only the texels that survive the cut-out are asked
                        // about colour: a transparent texel's colour is whatever
                        // the encoder left there, and it is not on screen.
                        unsigned char rgb[3]{};
                        if (compressed)
                        {
                            rgb[0] = colour[j * 4 + i][0];
                            rgb[1] = colour[j * 4 + i][1];
                            rgb[2] = colour[j * 4 + i][2];
                        }
                        else
                            TexelColour(format, data, rgb);
                        if (IsGreenTexel(rgb[0], rgb[1], rgb[2])) ++probe.green;
                    }
                }
            }
        }
        probe.hash = hash;
        if (!probe.texels) return probe;
        probe.fraction = float(probe.transparent) / float(probe.texels);
        if (probe.texels < kTextureAlphaMinTexels) return probe;
        probe.verdict = uint64_t(probe.transparent) * 100 >= uint64_t(probe.texels) * kTextureCutoutPercent
            ? TextureCutout::Cutout : TextureCutout::Opaque;
        probe.foliage = probe.verdict == TextureCutout::Cutout &&
            uint64_t(probe.green) * 100 >= uint64_t(probe.texels - probe.transparent) * kTextureFoliageGreenPercent;
        return probe;
    }
}
