#pragma once
// Shared output helpers for the offline previewers in tools/.
//
// Both CloudPreview.cpp and WaterPreview.cpp render to a plain RGB byte buffer and have to
// turn it into a PNG without pulling in a dependency, and a published sheet has to say which
// settings produced it. So: a minimal PNG writer (zlib stored blocks - no compressor, and
// every viewer reads it) and a 5x7 bitmap font for the label bars. Nothing here affects what
// is rendered; it only affects how the frame is written out and annotated, which is why both
// tools can share it.
//
// The PNG bytes are the same bytes CloudPreview.cpp produced on its own before this header
// existed, so previously published sheets can be reproduced byte for byte.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace preview
{

inline void PushBigEndian(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(uint8_t(value >> 24)); out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 8));  out.push_back(uint8_t(value));
}

inline uint32_t Crc32(const uint8_t* data, size_t length, uint32_t crc = 0xffffffffU)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320U ^ (c >> 1) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xffU] ^ (crc >> 8);
    return crc;
}

inline void PushChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& payload)
{
    PushBigEndian(out, uint32_t(payload.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = Crc32(out.data() + start, out.size() - start) ^ 0xffffffffU;
    PushBigEndian(out, crc);
}

// rgb is width*height*3 bytes, top row first, no alpha, no filtering.
inline bool WritePng(const char* path, int width, int height, const std::vector<uint8_t>& rgb)
{
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
    for (int y = 0; y < height; ++y)
    {
        raw.push_back(0);  // filter: none
        const uint8_t* row = &rgb[(size_t(y) * width) * 3];
        raw.insert(raw.end(), row, row + size_t(width) * 3);
    }

    std::vector<uint8_t> zlib;
    zlib.push_back(0x78); zlib.push_back(0x01);
    size_t offset = 0;
    while (offset < raw.size())
    {
        const uint16_t block = uint16_t(std::min<size_t>(65535, raw.size() - offset));
        const bool last = offset + block >= raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(uint8_t(block & 0xff)); zlib.push_back(uint8_t(block >> 8));
        zlib.push_back(uint8_t(~block & 0xff)); zlib.push_back(uint8_t((~block >> 8) & 0xff));
        zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + block);
        offset += block;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) { a = (a + byte) % 65521U; b = (b + a) % 65521U; }
    PushBigEndian(zlib, (b << 16) | a);

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    std::vector<uint8_t> ihdr;
    PushBigEndian(ihdr, uint32_t(width));
    PushBigEndian(ihdr, uint32_t(height));
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    PushChunk(png, "IHDR", ihdr);
    PushChunk(png, "IDAT", zlib);
    PushChunk(png, "IEND", {});

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return true;
}

// --------------------------------------------------------------- labels ---
// A 5x7 pixel font, one row per string, '#' set. It is deliberately small and it is drawn
// into the image rather than left to a caption, so a sheet is self-describing: the tuning
// numbers a frame was rendered with travel with the frame, which is the half of the cloud
// screenshots that a reader cannot recover from the README text.
struct Glyph { char c; const char* rows[7]; };

inline const Glyph* GlyphFor(char c)
{
    static const Glyph table[] = {
    { ' ', {".....",".....",".....",".....",".....",".....","....."} },
    { 'A', {".###.","#...#","#...#","#####","#...#","#...#","#...#"} },
    { 'B', {"####.","#...#","#...#","####.","#...#","#...#","####."} },
    { 'C', {".###.","#...#","#....","#....","#....","#...#",".###."} },
    { 'D', {"####.","#...#","#...#","#...#","#...#","#...#","####."} },
    { 'E', {"#####","#....","#....","####.","#....","#....","#####"} },
    { 'F', {"#####","#....","#....","####.","#....","#....","#...."} },
    { 'G', {".###.","#...#","#....","#.###","#...#","#...#",".###."} },
    { 'H', {"#...#","#...#","#...#","#####","#...#","#...#","#...#"} },
    { 'I', {".###.","..#..","..#..","..#..","..#..","..#..",".###."} },
    { 'J', {"..###","...#.","...#.","...#.","...#.","#..#.",".##.."} },
    { 'K', {"#...#","#..#.","#.#..","##...","#.#..","#..#.","#...#"} },
    { 'L', {"#....","#....","#....","#....","#....","#....","#####"} },
    { 'M', {"#...#","##.##","#.#.#","#.#.#","#...#","#...#","#...#"} },
    { 'N', {"#...#","##..#","#.#.#","#..##","#...#","#...#","#...#"} },
    { 'O', {".###.","#...#","#...#","#...#","#...#","#...#",".###."} },
    { 'P', {"####.","#...#","#...#","####.","#....","#....","#...."} },
    { 'Q', {".###.","#...#","#...#","#...#","#.#.#","#..##",".####"} },
    { 'R', {"####.","#...#","#...#","####.","#.#..","#..#.","#...#"} },
    { 'S', {".####","#....","#....",".###.","....#","....#","####."} },
    { 'T', {"#####","..#..","..#..","..#..","..#..","..#..","..#.."} },
    { 'U', {"#...#","#...#","#...#","#...#","#...#","#...#",".###."} },
    { 'V', {"#...#","#...#","#...#","#...#","#...#",".#.#.","..#.."} },
    { 'W', {"#...#","#...#","#...#","#.#.#","#.#.#","##.##","#...#"} },
    { 'X', {"#...#","#...#",".#.#.","..#..",".#.#.","#...#","#...#"} },
    { 'Y', {"#...#","#...#",".#.#.","..#..","..#..","..#..","..#.."} },
    { 'Z', {"#####","....#","...#.","..#..",".#...","#....","#####"} },
    { '0', {".###.","#...#","#.##.","#.#.#",".##.#","#...#",".###."} },
    { '1', {"..#..",".##..","..#..","..#..","..#..","..#..",".###."} },
    { '2', {".###.","#...#","....#","...#.","..#..",".#...","#####"} },
    { '3', {"#####","...#.","..#..","...#.","....#","#...#",".###."} },
    { '4', {"...#.","..##.",".#.#.","#..#.","#####","...#.","...#."} },
    { '5', {"#####","#....","####.","....#","....#","#...#",".###."} },
    { '6', {"..#..",".#...","#....","####.","#...#","#...#",".###."} },
    { '7', {"#####","....#","...#.","..#..",".#...",".#...",".#..."} },
    { '8', {".###.","#...#","#...#",".###.","#...#","#...#",".###."} },
    { '9', {".###.","#...#","#...#",".####","....#","...#.",".##.."} },
    { '-', {".....",".....",".....","#####",".....",".....","....."} },
    { '_', {".....",".....",".....",".....",".....",".....","#####"} },
    { '.', {".....",".....",".....",".....",".....",".##..",".##.."} },
    { ',', {".....",".....",".....",".....",".##..",".##..",".#..."} },
    { ':', {".....",".##..",".##..",".....",".##..",".##..","....."} },
    { '/', {"....#","....#","...#.","..#..",".#...","#....","#...."} },
    { '%', {"##..#","##.#.","...#.","..#..",".#...","#.##.","#.##"} },
    { '(', {"...#.","..#..",".#...",".#...",".#...","..#..","...#."} },
    { ')', {".#...","..#..","...#.","...#.","...#.","..#..",".#..."} },
    { '=', {".....",".....","#####",".....","#####",".....","....."} },
    { '+', {".....","..#..","..#..","#####","..#..","..#..","....."} },
    { '#', {".#.#.","#####",".#.#.",".#.#.","#####",".#.#.","....."} },
    { '*', {".....",".#.#.",".###.","#####",".###.",".#.#.","....."} },
    };
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (const Glyph& g : table) if (g.c == c) return &g;
    return nullptr;
}

inline void PutPixel(std::vector<uint8_t>& rgb, int imageWidth, int imageHeight,
    int x, int y, const uint8_t colour[3])
{
    if (x < 0 || y < 0 || x >= imageWidth || y >= imageHeight) return;
    uint8_t* at = &rgb[(size_t(y) * imageWidth + x) * 3];
    at[0] = colour[0]; at[1] = colour[1]; at[2] = colour[2];
}

// Draws `text` at (x,y) in `colour`; 6px per glyph, 7px tall. Returns the x after the text.
inline int DrawText(std::vector<uint8_t>& rgb, int imageWidth, int imageHeight,
    int x, int y, const std::string& text, const uint8_t colour[3])
{
    const int start = x;
    for (char c : text)
    {
        const Glyph* g = GlyphFor(c);
        if (g)
        {
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (g->rows[row][col] == '#') PutPixel(rgb, imageWidth, imageHeight, x + col, y + row, colour);
            x += 6;
        }
        else x += 6;
    }
    return x - start;
}

// The contact-sheet label: a black bar with the panel's settings, closed by a blue rule, so a
// reader can tell the four frames apart without a caption per frame.
inline void LabelBar(std::vector<uint8_t>& rgb, int imageWidth, int imageHeight, int x0, int y0,
    int width, int height, const std::string& text)
{
    const uint8_t black[3] = { 0, 0, 0 };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            PutPixel(rgb, imageWidth, imageHeight, x0 + x, y0 + y, black);
    // A blue rule closes the bar, and the ink is off-white rather than pure white so the
    // label reads as chrome around the frame instead of content inside it.
    const uint8_t rule[3] = { 60, 96, 168 }, ink[3] = { 236, 236, 240 };
    for (int x = 0; x < width; ++x) PutPixel(rgb, imageWidth, imageHeight, x0 + x, y0 + height - 1, rule);
    DrawText(rgb, imageWidth, imageHeight, x0 + 4, y0 + (height - 7) / 2, text, ink);
}

} // namespace preview
