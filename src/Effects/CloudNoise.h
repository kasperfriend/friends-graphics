#pragma once
// Procedural cloud noise, shared by the renderer and by tools/CloudPreview.cpp.
//
// Everything here is deterministic and tileable, which is what lets the
// offline preview renderer show exactly the sky the GPU will draw: same
// volumes, same weather map, same seeds.
//
// Three resources are produced:
//   * shape volume  (RGBA8, 64^3)  R = Perlin-Worley, GBA = Worley FBM at
//                                  three rising frequencies.
//   * detail volume (RGBA8, 32^3)  RGB = Worley FBM, used to erode the
//                                  silhouette into wisps and billows.
//   * weather map   (RGBA8, 512^2) R = coverage, G = cloud type
//                                  (stratus -> cumulus), B = density
//                                  variation, A = storm mask.
//
// This is the Guerrilla/Nubis layout that the modern game cloudscapes are
// built on. It is packed into four channels on purpose: one tex3D fetch has
// to return a whole FBM, because ps_3_0 has 32 temp registers and 512
// instruction slots to spend on the entire raymarch.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace renderer::cloudnoise
{
    struct Volume3D
    {
        int size = 0;
        std::vector<uint8_t> rgba;  // size^3 * 4, x fastest, then y, then z
    };

    struct Image2D
    {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    inline uint32_t HashU32(uint32_t x)
    {
        x ^= x >> 16; x *= 0x7feb352dU;
        x ^= x >> 15; x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    inline uint32_t HashCell(int x, int y, int z, uint32_t seed)
    {
        return HashU32(uint32_t(x) * 73856093U ^ uint32_t(y) * 19349663U ^ uint32_t(z) * 83492791U ^ seed);
    }

    inline float Unit(uint32_t h) { return float(h & 0xffffffU) * (1.f / 16777216.f); }

    inline int Wrap(int v, int n) { return ((v % n) + n) % n; }

    inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    inline float Fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
    inline float Saturate(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    // Remap used everywhere in the cloud literature: move a value from one
    // range to another, which is how coverage and erosion are applied
    // without clipping the gradient.
    inline float Remap(float v, float lo0, float hi0, float lo1, float hi1)
    {
        return lo1 + (v - lo0) / std::max(hi0 - lo0, 1e-5f) * (hi1 - lo1);
    }

    // --- Worley -----------------------------------------------------------
    // Cell noise that tiles because the cell indices wrap modulo the cell
    // count, not because the sample position wraps.
    //
    // The feature points are built once per grid instead of being hashed
    // again for all 27 neighbours of every texel: same values, about three
    // times less work, which is what makes generating a 64^3 volume at load
    // time affordable.
    struct PointGrid
    {
        int cells = 0;
        std::vector<float> points;  // cells^3 * 3
    };

    inline PointGrid BuildPointGrid(int cells, uint32_t seed)
    {
        PointGrid grid;
        grid.cells = cells;
        grid.points.resize(size_t(cells) * cells * cells * 3);
        for (int z = 0; z < cells; ++z)
            for (int y = 0; y < cells; ++y)
                for (int x = 0; x < cells; ++x)
                {
                    const uint32_t h = HashCell(x, y, z, seed);
                    float* p = &grid.points[(size_t(z) * cells * cells + size_t(y) * cells + x) * 3];
                    p[0] = Unit(h);
                    p[1] = Unit(HashU32(h ^ 0x9e3779b9U));
                    p[2] = Unit(HashU32(h ^ 0x85ebca6bU));
                }
        return grid;
    }

    inline float Worley(float x, float y, float z, const PointGrid& grid)
    {
        const int cells = grid.cells;
        const float fx = x * cells, fy = y * cells, fz = z * cells;
        const int cx = int(std::floor(fx)), cy = int(std::floor(fy)), cz = int(std::floor(fz));
        float best = 4.f;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                    const float* p = &grid.points[(size_t(Wrap(nz, cells)) * cells * cells +
                                                   size_t(Wrap(ny, cells)) * cells + size_t(Wrap(nx, cells))) * 3];
                    const float ex = float(nx) + p[0] - fx;
                    const float ey = float(ny) + p[1] - fy;
                    const float ez = float(nz) + p[2] - fz;
                    best = std::min(best, ex * ex + ey * ey + ez * ez);
                }
        return std::sqrt(best);
    }

    // Three octaves of inverted Worley: 1 means "inside a billow".
    struct WorleyStack
    {
        PointGrid a, b, c;
        void Build(int cells, uint32_t seed)
        {
            a = BuildPointGrid(cells, seed);
            b = BuildPointGrid(cells * 2, seed + 31U);
            c = BuildPointGrid(cells * 4, seed + 67U);
        }
        float Sample(float x, float y, float z) const
        {
            return Saturate((1.f - Worley(x, y, z, a)) * .625f +
                            (1.f - Worley(x, y, z, b)) * .25f +
                            (1.f - Worley(x, y, z, c)) * .125f);
        }
    };

    // --- Perlin -----------------------------------------------------------
    inline float GradDot(uint32_t h, float x, float y, float z)
    {
        switch (h & 15U)
        {
        case 0:  return  x + y;
        case 1:  return -x + y;
        case 2:  return  x - y;
        case 3:  return -x - y;
        case 4:  return  x + z;
        case 5:  return -x + z;
        case 6:  return  x - z;
        case 7:  return -x - z;
        case 8:  return  y + z;
        case 9:  return -y + z;
        case 10: return  y - z;
        case 11: return -y - z;
        case 12: return  x + y;
        case 13: return -x + y;
        case 14: return -y + z;
        default: return -y - z;
        }
    }

    inline float Perlin(float x, float y, float z, int period, uint32_t seed)
    {
        const float fx = x * period, fy = y * period, fz = z * period;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy)), iz = int(std::floor(fz));
        const float tx = fx - ix, ty = fy - iy, tz = fz - iz;
        const float ux = Fade(tx), uy = Fade(ty), uz = Fade(tz);
        float corner[8];
        for (int i = 0; i < 8; ++i)
        {
            const int dx = i & 1, dy = (i >> 1) & 1, dz = (i >> 2) & 1;
            const uint32_t h = HashCell(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
            corner[i] = GradDot(h, tx - dx, ty - dy, tz - dz);
        }
        const float x00 = Lerp(corner[0], corner[1], ux), x10 = Lerp(corner[2], corner[3], ux);
        const float x01 = Lerp(corner[4], corner[5], ux), x11 = Lerp(corner[6], corner[7], ux);
        return Lerp(Lerp(x00, x10, uy), Lerp(x01, x11, uy), uz);
    }

    // Tileable 2D gradient noise with INDEPENDENT periods per axis. The
    // weather map needs that: real cloud fields are stretched into streets
    // along the wind, and scaling the sample coordinate instead of the
    // period would break the wrap and put a seam in the sky every tile.
    inline float Grad2(uint32_t h, float x, float y)
    {
        const float a = Unit(h) * 6.283185307f;
        return std::cos(a) * x + std::sin(a) * y;
    }

    inline float Perlin2(float x, float y, int px, int py, uint32_t seed)
    {
        const float fx = x * px, fy = y * py;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const float tx = fx - ix, ty = fy - iy;
        const float ux = Fade(tx), uy = Fade(ty);
        float corner[4];
        for (int i = 0; i < 4; ++i)
        {
            const int dx = i & 1, dy = (i >> 1) & 1;
            corner[i] = Grad2(HashCell(Wrap(ix + dx, px), Wrap(iy + dy, py), 0, seed), tx - dx, ty - dy);
        }
        return Lerp(Lerp(corner[0], corner[1], ux), Lerp(corner[2], corner[3], ux), uy);
    }

    inline float PerlinFBM2(float x, float y, int px, int py, int octaves, uint32_t seed)
    {
        float sum = 0.f, amplitude = .5f, total = 0.f;
        for (int octave = 0; octave < octaves; ++octave)
        {
            sum += Perlin2(x, y, px << octave, py << octave, seed + uint32_t(octave) * 101U) * amplitude;
            total += amplitude;
            amplitude *= .5f;
        }
        return sum / total;  // about -1..1
    }

    // Tileable 2D Worley, returned INVERTED (1 at a feature point) so it
    // reads as a field of discrete cloud cells rather than a web of cracks.
    inline float Worley2(float x, float y, int cells, uint32_t seed)
    {
        const float fx = x * cells, fy = y * cells;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        float best = 4.f;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const uint32_t h = HashCell(Wrap(ix + dx, cells), Wrap(iy + dy, cells), 0, seed);
                const float px = float(ix + dx) + Unit(h);
                const float py = float(iy + dy) + Unit(HashU32(h));
                const float ex = fx - px, ey = fy - py;
                best = std::min(best, ex * ex + ey * ey);
            }
        return Saturate(1.f - std::sqrt(best));
    }

    inline float PerlinFBM(float x, float y, float z, int period, uint32_t seed)
    {
        float sum = 0.f, amplitude = .5f, total = 0.f;
        int p = period;
        for (int octave = 0; octave < 4; ++octave)
        {
            sum += Perlin(x, y, z, p, seed + uint32_t(octave) * 101U) * amplitude;
            total += amplitude;
            amplitude *= .5f;
            p *= 2;
        }
        return sum / total;  // about -1..1
    }

    // --- Resource generation ----------------------------------------------
    namespace detail
    {
        template <typename Fn>
        void ParallelFor(int begin, int end, Fn&& body)
        {
            const int span = end - begin;
            if (span <= 0) return;
            unsigned hw = std::thread::hardware_concurrency();
            if (hw == 0) hw = 2;
            const int workers = std::min<int>(int(hw), span);
            if (workers <= 1) { for (int i = begin; i < end; ++i) body(i); return; }
            std::vector<std::thread> pool;
            pool.reserve(size_t(workers));
            for (int w = 0; w < workers; ++w)
            {
                pool.emplace_back([=, &body] {
                    for (int i = begin + w; i < end; i += workers) body(i);
                });
            }
            for (auto& t : pool) t.join();
        }

        inline uint8_t ToByte(float v) { return uint8_t(Saturate(v) * 255.f + .5f); }

        // Procedural noise does not arrive with a usable histogram: four
        // octaves of Perlin pile up around the mean, so a coverage remap
        // written against 0..1 would only ever see the middle third. Every
        // channel the remaps depend on is therefore stretched across its own
        // measured 1st..99th percentile.
        inline void StretchChannel(std::vector<uint8_t>& rgba, int channel, float low = .01f, float high = .99f)
        {
            int histogram[256] = {};
            const size_t texels = rgba.size() / 4;
            for (size_t i = 0; i < texels; ++i) ++histogram[rgba[i * 4 + size_t(channel)]];
            const size_t lowCount = size_t(double(texels) * low);
            const size_t highCount = size_t(double(texels) * high);
            int lowValue = 0, highValue = 255;
            size_t running = 0;
            for (int v = 0; v < 256; ++v)
            {
                running += size_t(histogram[v]);
                if (running >= lowCount) { lowValue = v; break; }
            }
            running = 0;
            for (int v = 0; v < 256; ++v)
            {
                running += size_t(histogram[v]);
                if (running >= highCount) { highValue = v; break; }
            }
            if (highValue <= lowValue) return;
            const float scale = 255.f / float(highValue - lowValue);
            for (size_t i = 0; i < texels; ++i)
            {
                uint8_t& value = rgba[i * 4 + size_t(channel)];
                value = ToByte((float(value) - float(lowValue)) * scale * (1.f / 255.f));
            }
        }
    }

    // R = Perlin-Worley (the rounded base billow), GBA = Worley FBM at three
    // rising frequencies, used as one packed FBM when the base shape is built.
    inline Volume3D GenerateShape(int size = 64)
    {
        Volume3D v;
        v.size = size;
        v.rgba.assign(size_t(size) * size * size * 4, 0);
        WorleyStack low, s1, s2, s3;
        low.Build(4, 1301U);
        s1.Build(4, 2203U);
        s2.Build(8, 3307U);
        s3.Build(16, 4409U);
        detail::ParallelFor(0, size, [&](int z) {
            const float fz = (float(z) + .5f) / float(size);
            for (int y = 0; y < size; ++y)
            {
                const float fy = (float(y) + .5f) / float(size);
                for (int x = 0; x < size; ++x)
                {
                    const float fx = (float(x) + .5f) / float(size);
                    const float perlin = PerlinFBM(fx, fy, fz, 4, 7001U) * .5f + .5f;
                    // Guerrilla's Perlin-Worley: billowy Worley shape with
                    // Perlin's connective tissue punched through it.
                    const float perlinWorley = Saturate(Remap(perlin, low.Sample(fx, fy, fz) - 1.f, 1.f, 0.f, 1.f));
                    uint8_t* texel = &v.rgba[(size_t(z) * size * size + size_t(y) * size + x) * 4];
                    texel[0] = detail::ToByte(perlinWorley);
                    texel[1] = detail::ToByte(s1.Sample(fx, fy, fz));
                    texel[2] = detail::ToByte(s2.Sample(fx, fy, fz));
                    texel[3] = detail::ToByte(s3.Sample(fx, fy, fz));
                }
            }
        });
        detail::StretchChannel(v.rgba, 0);
        detail::StretchChannel(v.rgba, 1);
        return v;
    }

    // RGB = Worley FBM at three rising frequencies: the erosion octaves.
    inline Volume3D GenerateDetail(int size = 32)
    {
        Volume3D v;
        v.size = size;
        v.rgba.assign(size_t(size) * size * size * 4, 255);
        WorleyStack d1, d2, d3;
        d1.Build(4, 5501U);
        d2.Build(8, 6607U);
        d3.Build(16, 7703U);
        detail::ParallelFor(0, size, [&](int z) {
            const float fz = (float(z) + .5f) / float(size);
            for (int y = 0; y < size; ++y)
            {
                const float fy = (float(y) + .5f) / float(size);
                for (int x = 0; x < size; ++x)
                {
                    const float fx = (float(x) + .5f) / float(size);
                    uint8_t* texel = &v.rgba[(size_t(z) * size * size + size_t(y) * size + x) * 4];
                    texel[0] = detail::ToByte(d1.Sample(fx, fy, fz));
                    texel[1] = detail::ToByte(d2.Sample(fx, fy, fz));
                    texel[2] = detail::ToByte(d3.Sample(fx, fy, fz));
                    texel[3] = 255;
                }
            }
        });
        return v;
    }

    // The weather map is what turns one uniform deck into a sky with
    // clearings, fronts and towers. It tiles over a few kilometres of world
    // space and scrolls with the wind.
    inline Image2D GenerateWeather(int size = 512)
    {
        Image2D image;
        image.width = image.height = size;
        image.rgba.assign(size_t(size) * size * 4, 0);
        detail::ParallelFor(0, size, [&](int y) {
            const float fy = (float(y) + .5f) / float(size);
            for (int x = 0; x < size; ++x)
            {
                const float fx = (float(x) + .5f) / float(size);
                // Scale is everything here. This map tiles over ~34 km of
                // world and the march reaches ~26 km, so whatever frequency
                // dominates IS the composition of the sky: at period 3 the
                // whole visible dome is one mass parked on the horizon with
                // empty blue overhead, which is exactly what a flight-sim
                // sky looks like and exactly what a real one does not.
                //
                //   front   periods  3 x  4  ~ 11 x 8 km : where weather is
                //   group   periods  7 x 11 ~  5 x 3 km  : groups of clouds
                //   cell    14 cells         ~  2.4 km   : individual clouds
                //
                // The x/y periods differ on purpose: cloud streets line up
                // downwind, and independent periods keep the map tileable,
                // which scaling the sample coordinate would not.
                const float front = PerlinFBM2(fx, fy, 3, 4, 3, 9103U) * .5f + .5f;
                const float group = PerlinFBM2(fx, fy, 7, 11, 4, 9209U) * .5f + .5f;
                const float cell = Worley2(fx, fy, 14, 9533U);
                // Worley cells give discrete clouds, but only where the
                // larger fields already want weather - otherwise the sky
                // turns into evenly spaced polka dots.
                float coverage = Saturate(front * .38f + group * .40f + cell * front * .34f);
                coverage = Saturate(Remap(coverage, .26f, .72f, 0.f, 1.f));

                // Cloud type: low flat stratus at the fringes, tall cumulus
                // in the middle of a mass. Correlating it with coverage is
                // what makes the sky read as weather instead of noise.
                const float typeNoise = PerlinFBM2(fx, fy, 2, 3, 3, 9311U) * .5f + .5f;
                const float type = Saturate(typeNoise * .5f + coverage * .62f);

                const float variation = Saturate(PerlinFBM2(fx, fy, 9, 13, 4, 9413U) * .5f + .5f);

                uint8_t* texel = &image.rgba[(size_t(y) * size + x) * 4];
                texel[0] = detail::ToByte(coverage);
                texel[1] = detail::ToByte(type);
                texel[2] = detail::ToByte(variation);
            }
        });
        detail::StretchChannel(image.rgba, 0, .02f, .98f);
        detail::StretchChannel(image.rgba, 1, .02f, .98f);
        detail::StretchChannel(image.rgba, 2, .05f, .95f);

        // The storm mask is the deep core of a tall mass: both coverage and
        // type high at once. It has to be built from the EQUALISED channels,
        // after the stretch - computed from the raw fields it never reached
        // the top of its range at all, which is how it spent its first
        // version as a channel of zeroes.
        detail::ParallelFor(0, size, [&](int y) {
            for (int x = 0; x < size; ++x)
            {
                uint8_t* texel = &image.rgba[(size_t(y) * size + x) * 4];
                const float cover = float(texel[0]) * (1.f / 255.f);
                const float type = float(texel[1]) * (1.f / 255.f);
                texel[3] = detail::ToByte(Saturate(Remap(cover * type, .34f, .72f, 0.f, 1.f)));
            }
        });
        return image;
    }
}
