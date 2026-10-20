#pragma once
// The underwater medium: which side of the surface the camera is on, and what the
// water does to the light travelling between the camera and everything else.
//
// This header is the ONE definition of both halves, and it compiles without
// windows.h so a tool can run it directly:
//
//   * WaterEffect.h reads `PlaneOverCamera` on every matched liquid draw and
//     uploads the answer as the water pixel stage's own "am I under it" lane,
//     instead of asking a triangle which way it faces;
//   * src/Effects/Underwater.h renders the medium over the world the camera is
//     inside, and its ps_3_0 stage is a line-by-line mirror of `ShadePixel`
//     below - a change in one has to land in the other in the same commit;
//   * tools/UnderwaterRegression.cpp pins the numbers quoted here.
//
// Why the medium is one model and not two. The water surface already absorbs light
// along the column it stands over: `exp(-bodyStyle.x*waterStyle.y*column*
// absorbTint.rgb)`, converging on the body's own deep colour. A camera under that
// surface is looking at terrain and objects through the SAME water, so the world
// pass has to use the same coefficient, the same asymptote and the same scattering
// lobe, or the seam where a seabed meets the surface above it is a line a player can
// put a finger on. `MediumFor` therefore authors nothing: it re-spells the registers
// the water pixel stage was handed, in the same units.
//
// The mixing space is the display space the water shader works in, not linear
// radiance. That is a deliberate choice and the reason is the same seam: the water
// stage blends its column in display space (the client's own colour, the captured
// scene copy and the deep tint are all encoded bytes), so a physically linear medium
// that disagreed with the surface at the waterline would be a visible edge where the
// two meet. Both halves are wrong or right together.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "WaterNoise.h"

namespace renderer
{
namespace underwater
{

struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline Vec3 Mul(Vec3 a, Vec3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a)
{
    const float length = Length(a);
    return length > 1e-8f ? a * (1.f / length) : Vec3{ 0.f, 0.f, 1.f };
}
inline float Saturate(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
inline float Smoothstep(float e0, float e1, float x)
{
    const float t = Saturate((x - e0) / std::max(e1 - e0, 1e-6f));
    return t * t * (3.f - 2.f * t);
}
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// ------------------------------------------------------- the camera's side ---
// The signed height of a liquid tile's own plane over the camera, in yards:
// positive when the water is above the camera.
//
// Read from the draw's OWN matrix alone - deliberately not compared against any
// second matrix. An earlier version dotted the draw's origin with the live view's
// up row and demanded the draw's basis agree with the live view to a tenth of a
// degree; on a fully shader-driven client the fixed-function VIEW state is whatever
// the last fixed-function draw left in it, and that version read nothing on a real
// client (the effect never engaged). This form needs nothing but the matrix the
// draw itself shades with, which is true at the draw by construction.
//
// The derivation, in the row-vector convention this tree uses everywhere
// (p_view = p_local * M, so M's row i is the view-space image of the local axis i and
// M's row 3 is the view-space image of the local origin):
//
//   The liquid grid's local X and Y lie IN the plane and its local +Z points up out
//   of it (that is why the vertex stage reads `world.xy` for phase and lifts `.z`).
//   The plane's view-space normal is therefore n = cross(r0, r1), oriented toward
//   r2 so it points out of the water. A liquid vertex lies ON the plane, and its
//   view-space image is q = row3 + x*r0 + y*r1 + z*r2. The camera is the
//   view-space origin, so the plane's signed perpendicular distance over the camera
//   is dot(q, n) / |n| - positive exactly when the camera sits below the plane.
//
// That answers "how far above the camera is this water", and a second question has
// to be answered before it may submerse anyone: is the camera horizontally INSIDE
// this water at all? A pool on a mesa reads exactly the yards a swimmer under the
// same height of lake reads, and height alone cannot tell them apart - this is the
// failure that put the medium on dry land in Mulgore and kept it there. So the
// caller hands over TWO consumed vertices (the draw's first and last), the reading
// also measures how far the camera is, horizontally, from the segment between
// them and how long that segment is, and a reading only counts as local when the
// camera is within about the span of the water it drew. The segment is the draw's
// own diagonal for any grid-shaped chunk (first and last indices walk a grid from
// one corner to the opposite), so the gate scales with the water body itself
// rather than with a yardage somebody guessed.
//
// That is exact for any world chain built from translation, rotation and axis scales
// (the three basis rows mutually orthogonal); shear is refused, because then the
// cross of two rows is no longer the third row's direction and "up out of the plane"
// stops being one direction. The vertices are ones this draw actually consumes
// (the caller reads them; tiles may carry their height in the vertices rather than
// the matrix, and measuring at the origin would then answer for a plane one water
// level away from the real one). Reflection and off-screen draws are NOT
// refused here: which pass a draw belongs to is the render target's fact, and the
// caller gates on it (WaterEffect.h). An earlier version gated on the sign of the
// basis determinant instead; that sign is a handedness convention between the
// client's world and view spaces that nothing in this tree has ever established, and
// guessing it wrong refuses every ordinary draw while letting the mirrored one
// through. The sign is still reported for the log, never acted on.
// The triangles a liquid draw actually emits, in the order it emits their
// vertices. This is the difference between a sheet and its bounding box: a
// liquid chunk is a grid of individually enabled tiles, so the rectangle
// between the first and last consumed vertices routinely contains dry shore
// cells and the inside of a river bend. When the caller can hand over the
// draw's own triangles, "is the sheet over the camera" stops being a
// rectangle question and becomes the geometry's own answer.
enum class LiquidTopology : std::uint8_t
{
    TriangleList,
    TriangleStrip,
    TriangleFan
};

struct LiquidCoverage
{
    const Vec3* vertices = nullptr; // local positions, in emitted order
    std::uint32_t vertexCount = 0;
    LiquidTopology topology = LiquidTopology::TriangleList;
    // The vertices' exact local x/y extremes, when the caller already walked
    // the array once (the production reader validates every position finite
    // as it copies the draw in, so for it the extremes are free). With them,
    // PlaneOverCamera skips its own validation pass AND can refuse a sheet
    // whose whole bounding box is beyond locality reach without scanning a
    // single triangle - most liquid draws in a frame are somebody else's
    // chunk. `boundsKnown` is also the caller vouching that every vertex is
    // finite; without it the reading checks that itself, as before.
    float minX = 0.f, maxX = 0.f, minY = 0.f, maxY = 0.f;
    bool boundsKnown = false;
};

struct PlaneReading
{
    float yards = -1.f;
    bool valid = false;
    // The camera's horizontal distance to the sampled water (the segment between
    // the two consumed vertices, or the one point measured), and the horizontal
    // length of that segment - the draw's own extent. Yards, like the height.
    float horizontalYards = 0.f;
    float spanYards = 0.f;
    // The camera is horizontally within (or just beside) this water, so its plane
    // may say whether the camera is in it. A valid reading that is not local is a
    // lake somewhere else at about this height: true, and not this camera's water.
    bool local = false;
    // The stronger footprint statement the tracker uses before it will START a
    // submersion: with two consumed vertices the draw's in-plane extent is a
    // rectangle (the sampled pair are a grid chunk's opposite corners), the camera
    // can be put in that same local frame from the draw's orthogonal basis, and
    // `footprintYards` is how far outside that rectangle it lies (0 inside).
    // `local` alone deliberately stays the looser "same body, one chunk over"
    // answer - a swimmer's own chunk is often the one behind the camera - but a
    // raised sheet seen from dry land below it is local in that loose sense too,
    // which is exactly the false submersion reported when looking up at nearby
    // water from the shore. Nearness may sustain a swim; only being under the
    // sheet may start one.
    float footprintYards = 0.f;
    bool footprintKnown = false;
    bool containsCamera = false;
    // The footprint above came from the triangles the draw emits, not from
    // the first/last rectangle: `footprintYards` is then the distance to the
    // nearest emitted triangle and `containsCamera` is the sheet itself
    // answering, dry holes and all.
    bool coverageKnown = false;
    // The draw's basis determinant is negative. Reported only: the render target,
    // not this, decides whether a draw belongs to the player's view.
    bool negativeDeterminant = false;
    // Why an invalid reading is invalid, for the water log: a renderer that reads
    // the camera's side of the water from draws has to be able to say, on a client
    // where the medium never appears, which of its refusals is the one firing.
    // 0 on a valid reading. 1 a basis, translation or vertex that is not
    // finite and non-degenerate, 3 basis rows not mutually orthogonal (shear),
    // (locality is not a refusal of the measurement and has no code: it is `local`)
    // 4 a plane past kMaxPlaneYards or not finite. (2 was the old "mirrored view"
    // refusal and is kept unused so old logs stay readable.)
    int reason = 0;
};

inline float BasisDeterminant(const float m[4][4]) noexcept
{
    const Vec3 r0{ m[0][0], m[0][1], m[0][2] };
    const Vec3 r1{ m[1][0], m[1][1], m[1][2] };
    const Vec3 r2{ m[2][0], m[2][1], m[2][2] };
    return Dot(r0, Cross(r1, r2));
}

// Beyond this the reading is not a body of water a player can be inside: it is a draw
// whose c5..c8 carried no world offset, where the same expression reads the world's
// own z=0 plane instead of the liquid's. The caller holds the previous answer rather
// than trusting it.
inline constexpr float kMaxPlaneYards = 4000.f;

// How far past its own span of measured water a reading still counts as the
// camera's water. The first/last pair under-measures a chunk (a grid's sampled
// diagonal spans the chunk, but the camera may be just past the chunk's shore
// while plainly in the same lake), so the gate is span plus a shore's worth of
// yards, never a bare span. When only one vertex could be read there is no span
// to scale by and a loose radius stands in: wide enough that a lake the camera
// can swim in is never refused for want of a second vertex, narrow enough that
// the same-height lake on the next plateau is.
inline constexpr float kLocalSlackYards = 45.f;
inline constexpr float kLocalFallbackYards = 450.f;
// A footprint counts as known only when both in-plane extents are real distances,
// and it contains the camera with a shoreline's tolerance rather than to the
// last inch: the sampled corners under-measure a chunk by the triangles that
// were clipped at its edge, and a camera crossing the sheet at a chunk seam is
// in the water either way. Without a footprint, this much horizontal closeness
// to the one measured point is the most a reading may claim before it may start
// a submersion - the 450-yard locality fallback above is a statement about
// which lake a height belongs to, never evidence that the camera is in it.
inline constexpr float kFootprintSlackYards = 3.f;
inline constexpr float kFootprintMinExtentYards = 4.f;
// Emitted triangles need no chunk-seam allowance of yards: they are where the
// water is, and a camera a yard onto the sand beside them is on the sand. The
// slack left is for the seam between two chunks drawing the same sheet and
// for the wave's own edge, not for a shoreline.
inline constexpr float kCoverageSlackYards = .75f;
// Without any footprint, entry evidence is only the distance to one measured
// point or segment. That is a shoreline's scale, not a lake's: a few yards
// says the measured water itself is at the camera; the tens of yards an
// earlier version allowed said only that some water was near a player on the
// ground beside it, which is the reported fault in its purest form. A real
// footprint is at most a few frames behind such a reading (the caller's
// vertex lock budget), so entry waits for it rather than guessing.
inline constexpr float kEntryFallbackYards = 4.f;

inline PlaneReading PlaneOverCamera(const float modelView[4][4],
    const Vec3* firstVertex, const Vec3* lastVertex,
    const LiquidCoverage* coverage = nullptr) noexcept
{
    PlaneReading out;
    const Vec3 rows[3] = {
        { modelView[0][0], modelView[0][1], modelView[0][2] },
        { modelView[1][0], modelView[1][1], modelView[1][2] },
        { modelView[2][0], modelView[2][1], modelView[2][2] } };
    const Vec3 origin{ modelView[3][0], modelView[3][1], modelView[3][2] };
    double lengths[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        const Vec3 axis3 = rows[axis];
        if (!std::isfinite(axis3.x) || !std::isfinite(axis3.y) || !std::isfinite(axis3.z))
        { out.reason = 1; return out; }
        lengths[axis] = std::sqrt(double(Dot(axis3, axis3)));
        if (!(lengths[axis] > 1e-4) || !(lengths[axis] < 1e4)) { out.reason = 1; return out; }
    }
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        (firstVertex && (!std::isfinite(firstVertex->x) || !std::isfinite(firstVertex->y) ||
            !std::isfinite(firstVertex->z))) ||
        (lastVertex && (!std::isfinite(lastVertex->x) || !std::isfinite(lastVertex->y) ||
            !std::isfinite(lastVertex->z))))
    { out.reason = 1; return out; }
    // Mutual orthogonality, scale-free: |cos| between rows under .05 (within about
    // three degrees of square). Rotation and axis scales pass exactly; shear does
    // not, which is the only shape under which the cross below is not the plane's
    // normal. Basis MAGNITUDES are never compared: a fixture scaled in local units
    // reports the same plane as one that is not.
    for (int axis = 0; axis < 3; ++axis)
        for (int other = axis + 1; other < 3; ++other)
            if (std::fabs(double(Dot(rows[axis], rows[other]))) >
                .05 * lengths[axis] * lengths[other])
            { out.reason = 3; return out; }
    Vec3 normal = Cross(rows[0], rows[1]);
    if (Dot(normal, rows[2]) < 0.f) normal = normal * -1.f; // out of the water
    const double normalLength = std::sqrt(double(Dot(normal, normal)));
    if (!(normalLength > 1e-8)) { out.reason = 1; return out; }
    // The measured water in view space: the two consumed vertices in full, or the
    // draw's origin when none could be read (the caller's log says which).
    const auto onPlane = [&](const Vec3& v) {
        return Vec3{ origin.x + v.x * rows[0].x + v.y * rows[1].x + v.z * rows[2].x,
                     origin.y + v.x * rows[0].y + v.y * rows[1].y + v.z * rows[2].y,
                     origin.z + v.x * rows[0].z + v.y * rows[1].z + v.z * rows[2].z };
    };
    const Vec3 q1 = firstVertex ? onPlane(*firstVertex) : origin;
    const Vec3 q2 = lastVertex ? onPlane(*lastVertex) : q1;
    // A tilted sheet (a river) is overhead by the average of its measured ends -
    // a one-vertex reading would swear by whichever end it happened to sample.
    double yards =
        double(Dot(q1, normal) + Dot(q2, normal)) / (2. * normalLength);
    if (!std::isfinite(yards) || std::fabs(yards) > double(kMaxPlaneYards)) { out.reason = 4; return out; }
    // Sideways, in the plane's own horizontal: project the measured points level
    // with the camera and measure to the segment between them. The camera is the
    // origin, so the feet of those projections are the points themselves.
    const float invN = float(1. / normalLength);
    const Vec3 up{ normal.x * invN, normal.y * invN, normal.z * invN };
    const auto levelled = [&](const Vec3& q) {
        const float along = Dot(q, up);
        return Vec3{ q.x - along * up.x, q.y - along * up.y, q.z - along * up.z };
    };
    const Vec3 a = levelled(q1);
    const Vec3 b = levelled(q2);
    const Vec3 ab{ b.x - a.x, b.y - a.y, b.z - a.z };
    const double abLen2 = double(Dot(ab, ab));
    out.spanYards = float(std::sqrt(abLen2));
    double t = 0.;
    if (abLen2 > 0.) t = std::clamp(-double(Dot(a, ab)) / abLen2, 0., 1.);
    const Vec3 closest{ a.x + float(t) * ab.x, a.y + float(t) * ab.y, a.z + float(t) * ab.z };
    out.horizontalYards = float(std::sqrt(double(Dot(closest, closest))));
    // The camera in the draw's own in-plane coordinates. q = origin + x*r0 +
    // y*r1 + z*r2 and the camera is q=0; the rows are mutually orthogonal (that
    // was refused above otherwise), so each coordinate is one projection. With
    // both consumed vertices that puts the camera against the sheet's actual
    // rectangle instead of against its diagonal: beside a raised lake is then
    // beside it, however near the diagonal passes.
    out.footprintYards = out.horizontalYards;
    const double cameraX = -double(Dot(origin, rows[0])) / (lengths[0] * lengths[0]);
    const double cameraY = -double(Dot(origin, rows[1])) / (lengths[1] * lengths[1]);
    // The emitted triangles themselves, when the caller read them. A liquid
    // chunk is not a rectangle: its tiles are individually enabled, so dry
    // shore cells and the inside of a bend sit inside the first/last bounds
    // with no water over them. Distance and containment against the actual
    // triangles is the sheet's own answer, and the height at the camera's
    // own point on the nearest triangle replaces the mean of two ends that
    // may be a river's whole fall apart.
    if (coverage && coverage->vertices && coverage->vertexCount >= 3
        && std::isfinite(cameraX) && std::isfinite(cameraY))
    {
        const double px = cameraX * lengths[0];
        const double py = cameraY * lengths[1];
        bool sane = true;
        double minX = 1e30, maxX = -1e30, minY = 1e30, maxY = -1e30;
        if (coverage->boundsKnown)
        {
            // The caller validated every vertex finite and measured these
            // extremes as it read the draw in; lengths are roots, so scaling
            // the local extremes scales the box (see LiquidCoverage).
            minX = double(coverage->minX) * lengths[0];
            maxX = double(coverage->maxX) * lengths[0];
            minY = double(coverage->minY) * lengths[1];
            maxY = double(coverage->maxY) * lengths[1];
        }
        else for (std::uint32_t i = 0; i < coverage->vertexCount && sane; ++i)
        {
            const Vec3& v = coverage->vertices[i];
            sane = std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
            if (!sane) break;
            minX = std::min(minX, double(v.x) * lengths[0]);
            maxX = std::max(maxX, double(v.x) * lengths[0]);
            minY = std::min(minY, double(v.y) * lengths[1]);
            maxY = std::max(maxY, double(v.y) * lengths[1]);
        }
        // The box the sheet lives in, before a single triangle is touched.
        // Past locality reach of even the box (the same reach the tail of
        // this function will judge by, with a hair of guard for the float
        // the answer is reported in), the triangles could only ever agree:
        // not local, not containing - and those are the only answers a far
        // sheet's reading is spent on, since a non-local reading observes
        // nothing and classifies nothing. Most liquid draws in a frame are
        // exactly that (every chunk of a lake except the camera's own), and
        // the scan below measured microseconds per such chunk refining a
        // distance and a height that only the log reads. The distance
        // reported on this path is the box's own - a lower bound on the
        // nearest triangle's - and the height stays the measured ends' mean.
        const double boxSpan = sane ? std::hypot(maxX - minX, maxY - minY) : 0.;
        const double boxReach = (boxSpan > 4. ? boxSpan + double(kLocalSlackYards)
                                               : double(kLocalFallbackYards)) + .01;
        const double boxOutX = std::max(std::max(minX - px, 0.), px - maxX);
        const double boxOutY = std::max(std::max(minY - py, 0.), py - maxY);
        const double boxD2 = boxOutX * boxOutX + boxOutY * boxOutY;
        const bool farBox = sane && boxD2 > boxReach * boxReach;
        if (farBox)
        {
            out.coverageKnown = true;
            out.footprintKnown = true;
            out.footprintYards = float(std::sqrt(boxD2));
            out.horizontalYards = out.footprintYards;
            out.spanYards = float(boxSpan);
        }
        double bestD2 = 1e30;
        double bestW[3] = { 1., 0., 0. };
        Vec3 bestTri[3]{};
        bool found = false;
        const auto triangle = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2) {
            const Vec3 tri[3] = { coverage->vertices[i0], coverage->vertices[i1],
                                  coverage->vertices[i2] };
            double x[3], y[3];
            for (int corner = 0; corner < 3; ++corner)
            {
                x[corner] = double(tri[corner].x) * lengths[0];
                y[corner] = double(tri[corner].y) * lengths[1];
            }
            // The triangle's own box first: a triangle whose box is already
            // no nearer than the best found cannot beat it (the update below
            // is strict), and six compares retire most of a sheet once the
            // scan has any answer at all - the barycentric work and its
            // divisions are spent only where they can still matter.
            const double loX = std::min(std::min(x[0], x[1]), x[2]);
            const double hiX = std::max(std::max(x[0], x[1]), x[2]);
            const double loY = std::min(std::min(y[0], y[1]), y[2]);
            const double hiY = std::max(std::max(y[0], y[1]), y[2]);
            const double outX = px < loX ? loX - px : (px > hiX ? px - hiX : 0.);
            const double outY = py < loY ? loY - py : (py > hiY ? py - hiY : 0.);
            if (outX * outX + outY * outY >= bestD2) return;
            const double denom = (y[1] - y[2]) * (x[0] - x[2])
                + (x[2] - x[1]) * (y[0] - y[2]);
            if (!(std::fabs(denom) > 1e-9)) return; // a strip's degenerate join
            double w[3];
            w[0] = ((y[1] - y[2]) * (px - x[2]) + (x[2] - x[1]) * (py - y[2])) / denom;
            w[1] = ((y[2] - y[0]) * (px - x[2]) + (x[0] - x[2]) * (py - y[2])) / denom;
            w[2] = 1. - w[0] - w[1];
            double d2 = 0.;
            if (w[0] < -1e-9 || w[1] < -1e-9 || w[2] < -1e-9)
            {
                d2 = 1e30;
                for (int edge = 0; edge < 3; ++edge)
                {
                    const int aIndex = edge, bIndex = (edge + 1) % 3;
                    const double ex = x[bIndex] - x[aIndex], ey = y[bIndex] - y[aIndex];
                    const double len2 = ex * ex + ey * ey;
                    const double tEdge = len2 > 0.
                        ? std::clamp(((px - x[aIndex]) * ex + (py - y[aIndex]) * ey) / len2, 0., 1.)
                        : 0.;
                    const double dx = px - (x[aIndex] + tEdge * ex);
                    const double dy = py - (y[aIndex] + tEdge * ey);
                    if (dx * dx + dy * dy < d2)
                    {
                        d2 = dx * dx + dy * dy;
                        w[0] = w[1] = w[2] = 0.;
                        w[aIndex] = 1. - tEdge;
                        w[bIndex] = tEdge;
                    }
                }
            }
            else
            {
                double sum = 0.;
                for (double& weight : w) { weight = std::clamp(weight, 0., 1.); sum += weight; }
                if (!(sum > 0.)) return;
                for (double& weight : w) weight /= sum;
            }
            if (d2 < bestD2)
            {
                bestD2 = d2;
                for (int corner = 0; corner < 3; ++corner)
                {
                    bestW[corner] = w[corner];
                    bestTri[corner] = tri[corner];
                }
                found = true;
            }
        };
        if (sane && !farBox)
        {
            if (coverage->topology == LiquidTopology::TriangleStrip)
                for (std::uint32_t i = 2; i < coverage->vertexCount && bestD2 > 0.; ++i)
                    triangle(i - 2, i - 1, i);
            else if (coverage->topology == LiquidTopology::TriangleFan)
                for (std::uint32_t i = 2; i < coverage->vertexCount && bestD2 > 0.; ++i)
                    triangle(0, i - 1, i);
            else
                for (std::uint32_t i = 0; i + 2 < coverage->vertexCount && bestD2 > 0.; i += 3)
                    triangle(i, i + 1, i + 2);
        }
        if (sane && found)
        {
            double localYards = 0.;
            for (int corner = 0; corner < 3; ++corner)
                localYards += bestW[corner]
                    * (double(Dot(onPlane(bestTri[corner]), normal)) / normalLength);
            if (!std::isfinite(localYards) || std::fabs(localYards) > double(kMaxPlaneYards))
            { out.reason = 4; return out; }
            yards = localYards;
            out.coverageKnown = true;
            out.footprintKnown = true;
            out.footprintYards = float(std::sqrt(bestD2));
            out.containsCamera = out.footprintYards <= kCoverageSlackYards;
            out.horizontalYards = out.footprintYards;
            out.spanYards = float(std::hypot(maxX - minX, maxY - minY));
        }
    }
    if (!out.coverageKnown && firstVertex && lastVertex)
    {
        const double minX = std::min(double(firstVertex->x), double(lastVertex->x));
        const double maxX = std::max(double(firstVertex->x), double(lastVertex->x));
        const double minY = std::min(double(firstVertex->y), double(lastVertex->y));
        const double maxY = std::max(double(firstVertex->y), double(lastVertex->y));
        const double extentX = (maxX - minX) * lengths[0];
        const double extentY = (maxY - minY) * lengths[1];
        out.footprintKnown = std::isfinite(cameraX) && std::isfinite(cameraY)
            && extentX > double(kFootprintMinExtentYards)
            && extentY > double(kFootprintMinExtentYards);
        if (out.footprintKnown)
        {
            const double outsideX = std::max(std::max(minX - cameraX, 0.), cameraX - maxX) * lengths[0];
            const double outsideY = std::max(std::max(minY - cameraY, 0.), cameraY - maxY) * lengths[1];
            out.footprintYards = float(std::sqrt(outsideX * outsideX + outsideY * outsideY));
            out.containsCamera = out.footprintYards <= kFootprintSlackYards;
        }
    }
    // Local: within the draw's own span of the water it measured, plus a shore's
    // slack; without a second vertex, the loose fallback radius stands in.
    const float reach = out.spanYards > 4.f
        ? out.spanYards + kLocalSlackYards
        : kLocalFallbackYards;
    out.local = out.horizontalYards <= reach;
    out.negativeDeterminant = BasisDeterminant(modelView) < 0.f;
    out.yards = float(yards);
    out.valid = true;
    return out;
}

// May this reading start a submersion (or, in the water stage, classify its
// own draw as seen from under water)? One definition for every consumer:
// the sheet must be over the camera - by its emitted triangles when they
// were read, by its sampled rectangle when they were not, or, with no
// footprint at all, by measured water at the camera itself. Nearness to
// somebody's water is never this.
inline bool MayStartSubmersion(const PlaneReading& reading) noexcept
{
    return reading.valid && reading.local
        && (reading.footprintKnown ? reading.containsCamera
                                    : reading.horizontalYards <= kEntryFallbackYards);
}

// The band the water pixel stage fades its optical side across, in yards. It is the
// wave's own lift either side of the plane at a big sea, so a camera floating at the
// mean surface answers "half in" instead of snapping a whole tile from one medium to
// the other as a crest passes over it.
inline constexpr float kShaderBandYards = .35f;

// ---------------------------------------------------- the frame's answer ----
// Chosen across every liquid draw of a frame, then smoothed in time.
//
// A frame often draws no liquid at all, and that is the ordinary case for a swimmer:
// looking at the seabed, the surface is behind the camera and frustum-culled, so nothing
// reports a plane. Simply holding the last answer would keep the medium on such a diver -
// which is right - but it would also keep it on a player who walked up a beach with the
// lake behind them, which is not. So the tracker remembers the plane's own height in the
// world rather than the camera's distance from it, and on a frame that drew nothing it
// re-measures the camera against that remembered height. The interface is a fixed
// horizontal surface: a diver who looks down stays in it, a swimmer who rises leaves it,
// and a player who climbs out onto the sand leaves it too, all without a draw to say so.
// Two more guards keep that memory honest. A camera jump - a teleport, a hearth, a
// zone change - forgets the plane at once, because it belongs to the place the
// player left, and the memory expires after a few hundred yards of horizontal
// travel from where it was observed, because past that the remembered height no
// longer describes the water anywhere near the camera. Together with the reading
// itself refusing non-local water (PlaneOverCamera), those are what stands between
// a player and the medium turning up on dry land and staying there.
struct Tracker
{
    static inline constexpr float kSmoothingRate = 9.f;   // 1/s: about .33 s to 95%
    static inline constexpr float kEnterYards = .22f;     // submerged once this much water is over the camera
    static inline constexpr float kExitYards = .06f;      // and not again until it is under this
    static inline constexpr float kRampYards = 1.5f;      // the air/world crossfade completes here
    // A zone change, a teleport or a hearth moves the camera further in one frame than
    // any swim can. The threshold is the atmosphere's own history-guard distance.
    static inline constexpr float kCameraJumpYards = 50.f;
    // The remembered plane is forgotten once the camera is this far, horizontally,
    // from where it was observed. A swimmer never leaves his lake unobserved for
    // half a thousand yards without the surface drawing again first.
    static inline constexpr float kPlaneForgetYards = 450.f;
    // A remembered plane may finish an entry it saw from overhead for this
    // long after the last overhead reading (the smoothing needs a few frames
    // to cross the enter threshold, and the sheet can leave the frustum in
    // exactly those frames). Past it, memory sustains a swimmer already in
    // and starts nobody: a height remembered from beside a lake is a fact
    // about the lake, not evidence that whoever later walks downhill past
    // its level is in it.
    static inline constexpr float kOverheadMemorySeconds = 1.f;
    // How long a known footprint may keep saying "the camera is not under
    // this sheet" before the remembered plane stops answering over it - and
    // how far the camera must meanwhile have travelled from where a sheet
    // last actually covered it. Both are required: outside evidence alone
    // cannot retire a diver whose own chunk is the culled one (a visible
    // neighbour not containing him proves nothing about the sheet over
    // him), and travel alone is a swim. Outside evidence sustained while
    // leaving the last confirmed overhead neighbourhood is a beach walker
    // under a neighbouring sheet's level, which is the exit this buys.
    static inline constexpr float kOutsideMemorySeconds = .75f;
    static inline constexpr float kOutsideTravelYards = 8.f;

    // This frame's raw reading: the plane nearest the camera, of the draws that
    // reported one.
    float raw = -1.f;
    bool observed = false;
    // This frame also saw a sheet actually over the camera (MayStartSubmersion
    // on a positive reading), and the nearest such reading: the only evidence
    // allowed to create or renew the remembered plane below.
    float overheadRaw = -1.f;
    bool observedOverhead = false;
    // This frame saw a sheet whose known footprint puts the camera outside
    // it. Not an observation - the opposite of one.
    bool sawOutside = false;
    // The smoothed answer every consumer reads.
    float yards = -1.f;
    // 0 above the surface, 1 one and a half yards under it: the ramp the air medium
    // fades out on and the world medium fades in on, so crossing the surface moves one
    // into the other instead of swapping them.
    float ramp = 0.f;
    bool submerged = false;
    uint64_t heldFrames = 0;
    float camera[3] = { 0.f, 0.f, 0.f };
    bool cameraValid = false;
    // The liquid plane's own height in world yards, and whether one is known. This is the
    // state that survives a frame with no liquid draw: `raw` is re-derived from it against
    // the current camera instead of being held or dropped.
    float planeZ = 0.f;
    // Where (world x, y) the remembered plane was observed, so it can be forgotten
    // once the camera is no longer in its neighbourhood.
    float planeAnchorX = 0.f;
    float planeAnchorY = 0.f;
    bool planeKnown = false;
    // Seconds since a sheet was last seen actually over the camera, and
    // seconds for which known footprints have continuously said the camera
    // is outside them with no sheet over it. The first bounds what memory
    // may start; the second is what finally retires it on dry land.
    float overheadAge = 1e9f;
    float outsideSeconds = 0.f;
    // The state was deliberately cleared - a camera jump, or the remembered plane
    // expiring with distance - so a frame that observes nothing says "air" rather
    // than holding the last smoothed value. Without this the clear lasts exactly
    // one frame: the very next unobserved frame holds the old yards forever,
    // which is the false submersion this whole apparatus exists to prevent. An
    // observation clears the flag (the new place has answered for itself).
    bool cleared = false;

    inline void BeginFrame() noexcept
    {
        raw = -1.f;
        observed = false;
        overheadRaw = -1.f;
        observedOverhead = false;
        sawOutside = false;
    }

    inline void Accept(float planeYards, bool overheadEvidence) noexcept
    {
        if (!std::isfinite(planeYards)) return;
        if (!observed || std::fabs(planeYards) < std::fabs(raw)) raw = planeYards;
        observed = true;
        if (overheadEvidence)
        {
            if (!observedOverhead || std::fabs(planeYards) < std::fabs(overheadRaw))
                overheadRaw = planeYards;
            observedOverhead = true;
        }
    }

    // `planeYards` is one draw's reading; the frame keeps the plane nearest the camera,
    // which is the interface the camera is actually at. Nearest also settles a mirrored
    // reflection draw that slipped past the determinant test: a mirror sits twice as far
    // from the plane as the camera does, so the real camera always wins the comparison.
    // A bare yards value carries no footprint to refuse it by, so it is taken
    // as overhead evidence; the production caller always has a PlaneReading
    // and goes through the gated overload below.
    inline void Observe(float planeYards) noexcept
    {
        Accept(planeYards, true);
    }

    // A reading observes only when it measured the camera's own water. A valid but
    // non-local height - a lake above or below the camera somewhere else - must not
    // answer for this camera, which is the fault that submerged dry land after a
    // teleport and, being re-observed every frame, survived restarts.
    inline void Observe(const PlaneReading& reading) noexcept
    {
        if (!reading.valid || !reading.local) return;
        // A positive reading answers only when the sheet is over the camera
        // (MayStartSubmersion) - not to start a submersion, and not to sustain
        // one either. An earlier form let nearness sustain an established
        // swim, on the reasoning that a diver's own chunk is often the culled
        // one; but that same looseness is what kept the medium on a player
        // who had climbed out onto ground below a neighbouring sheet's
        // level, re-observed every frame by water he was demonstrably not
        // in. Sustaining is the remembered plane's job in Advance, bounded
        // there by overhead recency and by the outside evidence recorded
        // here. Readings that say "air" are never gated: a nearby sheet may
        // always testify that the camera is above it - but testimony is all
        // it is (see Advance: it creates no memory that could later start
        // the medium when the camera walks downhill past its level).
        const bool overhead = MayStartSubmersion(reading);
        if (reading.yards > kExitYards && !overhead)
        {
            if (reading.footprintKnown && !reading.containsCamera) sawOutside = true;
            return;
        }
        Accept(reading.yards, overhead && reading.yards > kExitYards);
    }

    inline void Advance(float deltaSeconds, const float cameraWorld[3], bool cameraValidNow,
                        bool coveringPlaneInView = true) noexcept
    {
        const float dt = std::isfinite(deltaSeconds) ? std::clamp(deltaSeconds, 0.f, .25f) : 0.f;
        // All three axes, not only the height this line used first: the jump
        // test and the remembered plane's anchor spend x and y, and a NaN in
        // either would sail through every comparison below and anchor the
        // plane somewhere it could never expire from.
        const bool haveCamera = cameraValidNow && cameraWorld
            && std::isfinite(cameraWorld[0]) && std::isfinite(cameraWorld[1])
            && std::isfinite(cameraWorld[2]);
        if (observedOverhead) { overheadAge = 0.f; outsideSeconds = 0.f; }
        else overheadAge += dt;
        // A zone change, a teleport or a hearth: the remembered plane belongs to
        // somewhere the player is no longer, and the answer built from it with it.
        // This is a hard reset rather than a new reading to smooth toward, because the
        // value being left behind is not a measurement of anything the player is now
        // inside - the atmosphere's temporal history is dropped on exactly this
        // distance for exactly this reason. It is checked on EVERY frame, not only on
        // an unobserved one: a player who teleports INTO a place that keeps drawing
        // liquid whose plane reads over his head must not have to wait for a frame
        // with no water in view before the medium lets go. (On an observed frame the
        // fresh local reading below then re-measures and re-remembers.)
        if (haveCamera && cameraValid)
        {
            const float dx = cameraWorld[0] - camera[0];
            const float dy = cameraWorld[1] - camera[1];
            const float dz = cameraWorld[2] - camera[2];
            const float jump = kCameraJumpYards * kCameraJumpYards;
            if (dx * dx + dy * dy + dz * dz > jump)
            {
                if (!observed) raw = -1.f;
                yards = -1.f;
                ramp = 0.f;
                submerged = false;
                planeKnown = false;
                overheadAge = 1e9f;
                outsideSeconds = 0.f;
                cleared = true;
            }
        }
        if (observed)
        {
            heldFrames = 0;
            cleared = false;
            // Remember the plane itself, not the distance to it: the reading is the plane's
            // height over THIS frame's camera, so the two together are the plane's height -
            // and where it was seen, so the memory can expire with distance. ONLY an
            // overhead reading may create or renew that memory. The unrestricted form
            // remembered every local plane the frame accepted, air testimony included:
            // stand above a lake (its reading quite properly observes "air" and was
            // remembered), then walk downhill BESIDE it, and the direct reading is
            // refused by the footprint gate while the remembered height - re-measured
            // below - starts the medium anyway and the loose sustain then keeps it.
            // That is the Sholazar basin report, reproduced in the regression as the
            // walk-downhill-beside-the-lake sequence. A plane the camera was never
            // under is a fact about the lake, never a memory about the camera.
            if (haveCamera && observedOverhead)
            {
                planeZ = cameraWorld[2] + overheadRaw;
                planeAnchorX = cameraWorld[0];
                planeAnchorY = cameraWorld[1];
                planeKnown = true;
                // Again, after the jump reset above: a teleport INTO water
                // lands with exactly this fresh overhead reading, and it is
                // new evidence, not the stale memory the reset just dropped.
                overheadAge = 0.f;
                outsideSeconds = 0.f;
            }
        }
        else
        {
            ++heldFrames;
            // Known footprints in view, none of them over the camera, while
            // it travels away from where one last was: after a grace for
            // chunk seams and frustum flicker, that is not "no information" -
            // it is the geometry saying the camera left the water it can
            // see, and the remembered plane retires.
            //
            // But only on frames where the client could actually have been
            // looking at where the covering sheet would have to be. A liquid
            // body is drawn chunk against the client's frustum, so a camera
            // swimming under open water receives exactly the testimony a
            // walker on a bank below the sheet's level receives: local
            // sheets, every one answering "not over YOU". What separates
            // them is where the camera is looking - the swimmer's own
            // covering chunk is behind or above the view (it was last
            // confirmed where the anchor now sits, and the client only
            // draws what the frustum holds), while the walker has shore
            // and sky where his would be. When the caller reports the
            // covering region out of the client's view, "no sheet over the
            // camera" is a fact about the view and not about the water:
            // the evidence does not count, and the count restarts, so a
            // long swim cannot accrue it one glance at a time either.
            if (sawOutside && coveringPlaneInView) outsideSeconds += dt;
            else outsideSeconds = 0.f;
            if (sawOutside && outsideSeconds >= kOutsideMemorySeconds
                && haveCamera && planeKnown)
            {
                const float ax = cameraWorld[0] - planeAnchorX;
                const float ay = cameraWorld[1] - planeAnchorY;
                if (ax * ax + ay * ay > kOutsideTravelYards * kOutsideTravelYards)
                {
                    planeKnown = false;
                    cleared = true;
                }
            }
            if (haveCamera && planeKnown)
            {
                const float ax = cameraWorld[0] - planeAnchorX;
                const float ay = cameraWorld[1] - planeAnchorY;
                if (ax * ax + ay * ay > kPlaneForgetYards * kPlaneForgetYards)
                {
                    // The remembered water is somewhere else now: its height must not
                    // keep answering for the camera. Say "above", as after a jump.
                    planeKnown = false;
                    cleared = true;
                    raw = -1.f;
                }
                else
                {
                    // No liquid draw, but the interface is still where it was and the camera
                    // is where it is, which is the whole question - for a swimmer already
                    // in, or one whose overhead entry the smoothing has not finished
                    // crossing yet. A camera that is simply dry gets "air" from memory,
                    // however far below some remembered lake's level it has walked:
                    // memory sustains, it does not start.
                    const float remembered = planeZ - cameraWorld[2];
                    raw = (!submerged && remembered > kExitYards
                           && overheadAge > kOverheadMemorySeconds)
                        ? -1.f : remembered;
                }
            }
            else
            {
                // Nothing to measure against: hold the answer - unless the state was
                // cleared on purpose, in which case the answer IS "air" already.
                raw = (haveCamera && cleared) ? -1.f : yards;
            }
        }
        if (haveCamera)
        {
            for (int axis = 0; axis < 3; ++axis) camera[axis] = cameraWorld[axis];
            cameraValid = true;
        }
        // Exponential approach, bounded: a diver surfaces in a third of a second and the
        // medium has to be gone by then, while a bobbing swimmer at the surface must not
        // strobe between two media.
        yards = Lerp(yards, raw, 1.f - std::exp(-dt * kSmoothingRate));
        if (!std::isfinite(yards)) { yards = -1.f; raw = -1.f; }
        ramp = Smoothstep(kEnterYards, kRampYards, yards);
        // Hysteresis: entering takes more water over the camera than leaving does, so a
        // camera parked exactly at the surface cannot toggle a whole frame's fog pipeline
        // on and off.
        submerged = submerged ? yards > kExitYards : yards > kEnterYards;
        // A dry camera whose last overhead evidence is stale has no use for
        // a remembered plane, and keeping one is how a height comes back to
        // answer for ground it was never over. The next entry will be seen
        // by a draw - crossing a surface is the one moment the surface is
        // reliably in frame - and will remember its own plane then.
        if (!submerged && overheadAge > kOverheadMemorySeconds) planeKnown = false;
    }

    // The signed yards the water pixel stage classifies with on a draw whose own
    // matrices gave no plane: the frame's answer, which is the same water.
    inline float HeldYards() const noexcept { return observed ? raw : yards; }
};

// Whether the client could have been looking at the sheet that would cover the camera.
//
// The tracker's outside-evidence exit - local footprints, none of them over the
// camera - is only testimony about the WATER when the covering region was eligible
// to be drawn. The client culls liquid chunks against its frustum, so a swimmer
// looking down at the seabed goes unobserved every frame no matter how much water
// is over him; his "no sheet over me" evidence is a fact about the view, and the
// same numbers arrive from a walker on a bank below the sheet's level who IS dry.
// The view is the difference, and the view is what this asks: sample the remembered
// plane over the camera and a little way along the view's horizontal forward, and
// report whether any of it lands inside the client's frustum (with a quarter frame
// of slack - the covering chunk only has to be eligible to draw, not centred).
//
// `viewRaw` is the captured view in v[0..3] storage: rows are the world axes in
// view space, the same convention PlaneOverCamera's callers read, so a point goes
// over by projection against the columns and the world direction of view +z is
// the column of third components, flattened. `xScale`/`yScale` are the projection's
// P00/P11 (projUnpack[2]/[3]): the screen mapping |x/z|*P00 <= 1, same as the
// celestial tracker's screenX. A degenerate pose (forward with no horizontal
// part - a camera staring straight down - or an unusable scale, refused by the
// caller) answers false: a frame that cannot test the frustum hands the exit no
// evidence, because refusing here retires nothing while passing here can retire
// a diver.
inline bool CoveringPlaneInView(const float viewRaw[16], const float cameraWorld[3],
                                float planeZ, float xScale, float yScale) noexcept
{
    if (!viewRaw || !cameraWorld || !(xScale > 0.f) || !(yScale > 0.f)
        || !std::isfinite(xScale) || !std::isfinite(yScale)
        || !std::isfinite(planeZ))
        return false;
    for (int axis = 0; axis < 12; ++axis)
        if (!std::isfinite(viewRaw[axis])) return false;
    // World direction of view +z, flattened to the horizontal plane.
    const float forwardX = viewRaw[2], forwardY = viewRaw[6];
    const float forwardLength = std::sqrt(forwardX * forwardX + forwardY * forwardY);
    if (!(forwardLength > 1e-4f)) return false;
    constexpr float kCoveringInViewMargin = 1.3f;
    for (const float distance : {0.f, 10.f, 25.f})
    {
        const float point[3] = { cameraWorld[0] + forwardX / forwardLength * distance,
                                 cameraWorld[1] + forwardY / forwardLength * distance,
                                 planeZ };
        const float viewX = point[0] * viewRaw[0] + point[1] * viewRaw[4]
            + point[2] * viewRaw[8] + viewRaw[12];
        const float viewY = point[0] * viewRaw[1] + point[1] * viewRaw[5]
            + point[2] * viewRaw[9] + viewRaw[13];
        const float viewZ = point[0] * viewRaw[2] + point[1] * viewRaw[6]
            + point[2] * viewRaw[10] + viewRaw[14];
        if (viewZ > 0.5f
            && std::fabs(viewX) <= viewZ * kCoveringInViewMargin / xScale
            && std::fabs(viewY) <= viewZ * kCoveringInViewMargin / yScale)
            return true;
    }
    return false;
}

// The signed yards one liquid draw may classify ITSELF with (the water
// stage's c218.w and everything folded from it). The same entry rule as the
// tracker, spent in the other consumer: a sheet beside and below-level of a
// camera on dry ground reads positive yards and is local, and classifying
// its pixels as seen from under water on that say-so was the tracker bug's
// twin in the surface stage. Air and interface readings keep their own
// answer; a positive reading that may not start a submersion falls back to
// the frame's settled one, which is itself gated the same way.
inline float ClassifiedYards(const PlaneReading& reading, const Tracker& tracker) noexcept
{
    if (!reading.valid || !reading.local) return tracker.HeldYards();
    if (reading.yards <= Tracker::kExitYards || MayStartSubmersion(reading))
        return reading.yards;
    return tracker.HeldYards();
}

// ------------------------------------------------------------- the medium ---
// Per-yard, per-channel extinction and the colour the column converges to, in the
// water pixel stage's own register units. `absorbRegister` is AbsorptionColour/128
// (c216.rgb), `deepRegister` is DeepColour/255 (c215.rgb) and `tintRegister` is
// WaterColour/128 (c214.rgb) - the exact three the shader was handed, so a zone's
// authored azure or murk is the same colour under the surface as in it.
struct Medium
{
    Vec3 extinction{ 0.f, 0.f, 0.f };
    Vec3 deep{ 0.f, 0.f, 0.f };
    Vec3 tint{ 1.f, 1.f, 1.f };
    // The green channel's first e-fold, in yards: the number a player reads as "how far
    // can I see", and the one the log line reports.
    float clarityYards = 0.f;
    bool valid = false;
};

// bodyStyle.x is `.088*(100/ClarityPercent)` and waterStyle.y is
// `DepthAbsorptionPercent/100` (0 with DEPTH off), which is the product the water stage
// multiplies its column by. Spelled here once, so the world pass cannot drift off the
// surface it has to meet.
inline constexpr float kExtinctionBase = .088f;

inline Medium MediumFor(float clarityPercent, float absorptionPercent, const Vec3& absorbRegister,
                        const Vec3& deepRegister, const Vec3& tintRegister, bool depthEnabled) noexcept
{
    Medium out;
    const float clarity = std::isfinite(clarityPercent) ? std::clamp(clarityPercent, 1.f, 400.f) : 100.f;
    const float absorption = depthEnabled && std::isfinite(absorptionPercent)
        ? std::clamp(absorptionPercent, 0.f, 1.f) : 0.f;
    const float rate = kExtinctionBase * (100.f / clarity) * absorption;
    Vec3 absorb = absorbRegister;
    for (float* channel : { &absorb.x, &absorb.y, &absorb.z })
        *channel = std::isfinite(*channel) ? std::clamp(*channel, 0.f, 4.f) : 1.f;
    out.extinction = absorb * rate;
    // The medium inherits the water's own colour, not merely its deep asymptote.
    // WaterColour is a multiplier (0x808080 is neutral in the register spelling),
    // so what carries a place's hue is the PROPORTION between its channels, not
    // their absolute level - a dark red pool and a bright red pool are the same
    // water at two exposures. Normalising the tint by its own luminance gives
    // that hue as a mean-one multiplier, and folding it into the deep colour
    // keeps DeepColour the author of how dark the column ends while WaterColour
    // authors what colour it ends AS: red water converges red, fel water green,
    // and a neutral tint (the shipped default, and the exact-agreement test's)
    // is an exact identity. The clamp keeps a pathological one-channel tint
    // from turning the asymptote into a laser.
    Vec3 tint = tintRegister;
    Vec3 deep = deepRegister;
    for (float* channel : { &tint.x, &tint.y, &tint.z })
        *channel = std::isfinite(*channel) ? std::clamp(*channel, 0.f, 4.f) : 1.f;
    for (float* channel : { &deep.x, &deep.y, &deep.z })
        *channel = std::isfinite(*channel) ? std::clamp(*channel, 0.f, 4.f) : 0.f;
    const float tintLuminance = .299f * tint.x + .587f * tint.y + .114f * tint.z;
    if (tintLuminance > 1e-4f)
    {
        const auto hue = [&](float channel) {
            return std::clamp(channel / tintLuminance, .25f, 3.5f);
        };
        deep = { deep.x * hue(tint.x), deep.y * hue(tint.y), deep.z * hue(tint.z) };
    }
    out.deep = deep;
    out.tint = tint;
    out.clarityYards = out.extinction.y > 1e-6f ? 1.f / out.extinction.y : 1e6f;
    // A medium that extinguishes nothing is not a medium: the pass reports itself
    // inactive rather than drawing an identity frame.
    out.valid = rate > 0.f;
    return out;
}

// The column the medium is integrated over. The water stage caps its own
// camera-to-surface ray at this many yards, and the world pass uses the same cap so a
// far plane and a deep ocean floor converge on the same colour instead of one of them
// going black.
inline constexpr float kFarColumnYards = 250.f;

// ---------------------------------------------------------------- tuning ----
struct Settings
{
    bool enabled = true;
    // The whole medium: extinction, in-scatter and the deep asymptote. 0 leaves the
    // client's own underwater fog standing alone.
    float strength = 1.f;
    // The net the surface overhead focuses onto whatever is under it, as a fraction of
    // the mean-preserving normalization below.
    float caustics = .55f;
    // Light arriving through a wavy surface comes down in beams; this is how much of
    // that structure the in-scattered light carries.
    float shafts = .6f;
    // Full-frame refraction wobble, as a fraction of kMaxDistortionTexels.
    float distortion = .5f;
    // The up-looking glow of the surface itself, including on pixels the client left as
    // sky because no liquid tile was drawn there.
    float surfaceGlow = .55f;
    // How dirty the water is, on top of what the zone's clarity authored: suspended silt
    // is not uniform, so this scales a world-space patch field and a depth gradient -
    // murkier with depth, patchier across the sea - rather than a second flat extinction.
    // 0 is the zone's own medium with no silt in it; 1 is the shipped silt; 3 triples it.
    float turbidity = 1.f;
    bool debug = false;
};

// Measured over four million lattice samples of the two octaves the caustic reads
// (tools/UnderwaterRegression.cpp re-measures and pins it): the sharpened net averages
// .195948 over its now-periodic lattice, so this divisor makes the modulation mean
// exactly one. Caustics move light around on the bed, they do not create it, and a
// term that only ever added would brighten every seabed in the game by its own average.
inline constexpr float kCausticNormalization = 5.1034f;

// The depth at which suspended silt has fully accumulated, in yards of water over the
// shaded point. Sediment hangs in the column rather than dissolving in it, so a pool is
// clearer at the surface than at the bed, and the difference is a gradient a swimmer
// reads as depth even in water too still to carry a net.
inline constexpr float kSiltYards = 18.f;

// Cosine of the water-to-air critical angle: sqrt(1-(1/1.333)^2) = .6613, the half-angle of
// the cone a submerged ray can actually leave the water through. Outside it the surface is a
// mirror from below, so no hill, sail or sky beyond the plane can arrive through it whatever
// the client drew there - the water stage states the same thing on its own pixels
// (totalInternal=1-smoothstep(.63,.69,NoV)), and this is that statement for the pixels no
// liquid tile covered. The band is wider here (.60-.72 against the surface's .63-.69) because
// this half reads the flat plane where the surface reads its waved normal, and the wave's own
// slope smears the window's edge by about that much.
inline constexpr float kSnellCos = .6613f;

// The near-surface layer an exiting IMAGE also has to cross, expressed as equivalent
// yards of the same medium at vertical incidence. Energy and image do not die at the
// same rate in dirty water: suspended silt and a rough, moving interface keep handing
// the downwelling light to the diver while scrambling which direction it came from.
// Round 13 charged that as (1 + turbidity) on the water overhead, which is the right
// trade at depth and almost no trade at all a yard under the surface - the place a
// swimmer actually looks up from. Four equivalent yards per unit of turbidity is the
// interface's own share: negligible in the clear zones (turbidity .3-.45, a yard and a
// half against a forty-yard e-fold), decisive in a swamp (turbidity 2.1-2.4 against a
// much shorter e-fold), and exactly nothing at turbidity 0, which keeps the crystal
// pane - and the bottom path's exact-agreement pin - intact.
inline constexpr float kInterfaceScatterYards = 4.f;

// The multiplier an exiting column pays, interface included. Spelled as a multiplier
// on the overhead column rather than as added yards because that is what both shader
// stages can spend: for a ray that reaches the plane the column is submersion/cos, so
// multiplying it by turbidity*kInterfaceScatterYards/submersion adds exactly
// kInterfaceScatterYards*turbidity/cos yards - the slant path through the same
// near-surface layer - without either stage dividing by a per-pixel cosine. The
// stages therefore receive (ExitWash - 1) in a constant lane the CPU fills from the
// submersion it already knows, and this function is the one spelling of it.
inline float ExitWash(float turbidity, float submersionYards, bool exited) noexcept
{
    if (!exited) return 1.f;
    const float dirt = std::isfinite(turbidity) ? std::clamp(turbidity, 0.f, 3.f) : 0.f;
    const float overhead = std::isfinite(submersionYards)
        ? std::max(submersionYards, 1.f) : 1.f;
    return 1.f + dirt * (1.f + kInterfaceScatterYards / overhead);
}

// The image-forming yards of a liquid fragment seen from inside the medium: the
// camera-to-surface segment with the exit wash on it inside the surface's own Snell
// band (.63-.69 on the waved normal, as the water stage spells totalInternal), and
// the far column outside it, where no transmitted picture exists at all. This is the
// statement ShadePixel makes for pixels no liquid tile covered, restated for the
// tile's own pixels, which the full-screen pass deliberately leaves to the water
// stage - so the plane and the gaps in the plane now answer the same question.
inline float InterfaceYards(float cameraColumnYards, float viewCos,
                            float turbidity, float submersionYards) noexcept
{
    const float column = std::isfinite(cameraColumnYards)
        ? std::clamp(cameraColumnYards, 0.f, kFarColumnYards) : 0.f;
    const float cosine = std::isfinite(viewCos) ? viewCos : 0.f;
    return Lerp(kFarColumnYards, column * ExitWash(turbidity, submersionYards, true),
                Smoothstep(.63f, .69f, cosine));
}

// The size of a turbidity patch, in caustic cells: one noise cell of the surface's
// own chop is a couple of yards, and a patch of silt or plankton is a feature of the
// landscape, so the field is sampled at a fortieth of the frequency - about a hundred
// yards across. This is the ONE camera-relative sample left in the pass, and it is
// left knowingly: anchoring it through the broad warp was built and measured at 522
// of the stage's 512 instruction slots (the warp's rotated coordinate costs five the
// shader does not have), and a phase of its own would need a constant lane the stage
// does not have either. The consequence is only that the murk patches breathe slowly
// with the swimmer instead of lying pinned; the net, the shafts and the medium are
// all anchored. TODO.md carries the item for whenever the budget or the ABI moves.
inline constexpr float kTurbidityPatchCells = 40.f;

// Where in that field the sampled neighbourhood sits. Sampled naively, the patch
// field's lattice has a VERTEX at coordinate zero - shadedWorld is camera-relative,
// so that vertex is the point directly under the camera, forever: the four cells
// meeting there interpolate as four flat wedges pinned to the swimmer's feet, one
// big 2x2 cross in the middle of a downward view (seen live, round 13). Offsetting
// the sample by a non-integer number of patch cells puts the nearest vertex about
// fifty yards off in each axis and keeps the (unavoidable, debt above) visible
// pattern a slow gradient underfoot instead of a surveying diagram. It also keeps
// the sampled window far inside one wrap period of the noise's own 64 cells, so
// the wrap seam never enters a reachable view either.
inline constexpr float kDirtSampleOffset[2] = { 37.31f, 57.17f };
// The sharpening exponent of the net: two ridge functions multiplied, then this.
// Measured peak/mean 5.06, which is the contrast a real caustic net carries, and 0.0% of
// sampled cells sit on a plateau - the failure mode that printed a lattice onto the foam
// (docs/water-rendering.md, "Foam").
inline constexpr float kCausticSharpen = 2.2f;
// Caustics are a shallow-water phenomenon: the net blurs and dies as the water over it
// deepens. This is the depth at which they are gone, in yards.
inline constexpr float kCausticReachYards = 28.f;
// The most the whole frame is allowed to wobble, in texels, at DISTORTION 100: the peak
// displacement, which is what the two-term sine sum below is divided down to. Anything
// larger than a texel and a half reads as the whole world swimming, and the effect is
// meant to be the one cue that does not depend on anything being visible in the water.
inline constexpr float kMaxDistortionTexels = 1.6f;
// The sum of a sine and .6 of another peaks at 1.6, so this divisor makes
// kMaxDistortionTexels the actual peak rather than 1.6 times it.
inline constexpr float kWobblePeak = 1.6f;
// The wobble's four rates are 1.7, 1.1, 1.3 and .9 per second - seventeen, eleven, thirteen
// and nine tenths - so twenty pi seconds is a period of all four at once, and wrapping the
// clock there keeps the motion continuous while the argument stays small enough to still
// have fraction bits in a float32 an hour into a dive. The water field wraps its phases for
// exactly the same reason.
inline constexpr float kWobblePeriodSeconds = 62.83185307179586476f;
// How far along the view ray the beam march samples the surface it is lit through.
inline constexpr int kShaftSamples = 3;
// The signed noise a beam sample contributes, per unit of the accumulator. Measured
// against the octave's own spread: one sample of `(value-.5)*2` has a standard deviation
// of .429, three along a ray about .25, so this gain puts a beam's peak near 1 and its
// duty cycle near a fifth of the frame.
inline constexpr float kShaftGain = 2.2f;
// The colour of the sky's own downwelling light, over the medium's deep tint: the
// body's own colour lifted by its WaterColour, never by a fixed blue - a fixed
// lift is a second authored colour wearing the first one's clothes, and it is
// what kept the glow overhead blue in water that is red or fel-green on the
// surface. The surface stage's zenith is a scaled fog colour for the same
// reason: the lift takes the place's hue or it is not the place's sky.
inline constexpr float kSkyLift = .18f;

// The wave field the pass reads. Built by the compositor for the very camera the pass
// reconstructs world positions against (WaterEffect.h's UnderwaterFieldAt), from the same
// integrated motion the water draws built theirs from, so the caustics and the beams
// overhead are the waves the player is looking at, moving at the speed they are moving
// at, anchored where they are anchored.
struct Field
{
    float lattice[4] = { 1.f, 0.f, 0.f, 1.f };   // world->lattice rows, scaled by 1/cell
    float chopPhase[2] = { 0.f, 0.f };           // the base octave's wrapped camera phase
    float finePhase[2] = { 0.f, 0.f };           // the doubled phase of the net's fine octave
    float warpPhase[2] = { 0.f, 0.f };           // the broad warp's, added after its rotation
    float seconds = 0.f;                         // for the frame wobble only
};

struct PixelInput
{
    Vec3 scene{ 0.f, 0.f, 0.f };   // the frame's own colour here, display space
    float distance = 0.f;          // yards from the camera to the shaded point
    Vec3 world{ 0.f, 0.f, 0.f };   // that point's offset from the camera, in world yards
    Vec3 ray{ 0.f, 0.f, 1.f };     // normalized view-space direction from the camera to it
    Vec3 normal{ 0.f, 0.f, 1.f };  // the shaded surface's world-space normal
    Vec3 up{ 0.f, 0.f, 1.f };      // world +Z, in view space
    Vec3 light{ 0.f, 0.f, 1.f };   // toward the sun/moon, in view space
    Vec3 lightColour{ 0.f, 0.f, 0.f };
    // The inverse view basis: the three view axes in world yards, which is what turns a
    // view-space direction into the world one the beam march needs.
    Vec3 axis[3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };
    float sunElevation = 0.f;
    float submersion = 0.f;        // yards of water over the camera
    // The frame's submersion ramp: 0 in air, 1 a yard and a half under the surface. The
    // ps_3_0 stage folds it into the medium's strength (`strength=saturate(deepColour.w)*
    // ramp`), so the whole effect - extinction, net, in-scatter, asymptote - interpolates out
    // together as a camera rises through the surface, and this model folds it the same way
    // rather than leaving the crossing as something only the shader does. 1 by default, which
    // is the settled state a diver is in.
    float ramp = 1.f;
};

struct PixelOutput
{
    Vec3 colour{ 0.f, 0.f, 0.f };
    Vec3 transmittance{ 1.f, 1.f, 1.f };
    float absorption = 0.f;
    Vec3 inScatter{ 0.f, 0.f, 0.f };
    // The yards of water this ray actually crossed, which is not always the distance to the
    // point it ended at: a ray that leaves the liquid on the way there crosses water only as
    // far as the plane. Published so a caller - and tools/UnderwaterRegression.cpp - can see
    // the column the extinction was taken over.
    float columnYards = 0.f;
    float caustic = 0.f;
    float shaft = 0.f;
};

inline Vec3 Transmittance(const Medium& medium, float distance) noexcept
{
    const float d = std::min(std::max(distance, 0.f), kFarColumnYards);
    return { std::exp(-medium.extinction.x * d), std::exp(-medium.extinction.y * d),
             std::exp(-medium.extinction.z * d) };
}

// The Henyey-Greenstein lobe the water stage scatters with, in its own spelling: g rises
// with the source's elevation and the dot is the one that stage already computed, so the
// murk and the crest glow agree about which way the light is coming from. `ray` points
// from the camera to the point, so the lobe's own `dot(v,l)` is its negation.
inline float PhaseG(float sunElevation) noexcept { return .3f + .34f * Saturate(sunElevation); }
inline float PhaseDenominator(float g, float vDotL) noexcept
{
    return std::max(1.f + g * g + 2.f * g * vDotL, 1e-3f);
}
inline float PhaseLobe(float g, float vDotL) noexcept
{
    const float denominator = PhaseDenominator(g, vDotL);
    return (1.f - g * g) * .17f / (denominator * std::sqrt(denominator));
}
inline Vec3 ScatterColour(const Medium& medium) noexcept { return medium.deep * 1.7f + medium.tint * .12f; }
inline Vec3 SkyGlowColour(const Medium& medium) noexcept
{
    return medium.deep * 1.6f + medium.tint * kSkyLift;
}

// The world-anchored lattice cell of a point, from the offset the pass reconstructs.
// Camera-relative on purpose: the water stage builds its field the same way and adds the
// camera's own wrapped phase, because a continent-scale coordinate multiplied by a
// lattice row runs out of float32 fraction bits and the noise collapses onto its lattice
// (docs/water-rendering.md, round 1).
inline void LatticeCell(const Field& field, const Vec3& world, float out[2]) noexcept
{
    out[0] = world.x * field.lattice[0] + world.y * field.lattice[1];
    out[1] = world.x * field.lattice[2] + world.y * field.lattice[3];
}

// The renderer's value noise with its lattice made truly periodic at the wrap. The
// shared NoiseGradient wraps the lattice INDEX into [0,64) before hashing but hands
// the cell at index 63 corners hashed as 64 - while the cell that wraps to 0 hashes
// that same shared corner as 0. The two disagree, so every 64 cells in each lattice
// axis there is a hard seam in the field. On the surface, where the noise bends
// normals, nobody reads the line; under a sharpened, five-times-gained caustic net it
// prints as one drawn line across the bed every 160 yards (and a fine octave doubles
// the grid). Wrapping the CORNER indices too makes the field exactly 64-periodic in
// both axes - which is what the camera phase's own wrap assumed all along. Used by
// the net only: the warp's seams sit 1280 cells apart under a low-contrast beam
// average, and the dirt keeps its cell vertex away from the camera by an offset, so
// neither pays the instruction cost in the stage (each is a different budget story;
// see kTurbidityPatchCells and the shaft march).
inline float NoiseValue64(float x, float y) noexcept
{
    const float ix = std::floor(x), iy = std::floor(y);
    const float fx = x - ix, fy = y - iy;
    const float wx = ix - std::floor(ix * (1.f / 64.f)) * 64.f;
    const float wy = iy - std::floor(iy * (1.f / 64.f)) * 64.f;
    const float ex = wx + 1.f >= 64.f ? wx - 63.f : wx + 1.f;
    const float ey = wy + 1.f >= 64.f ? wy - 63.f : wy + 1.f;
    const auto corner = [](float cx, float cy) {
        float value = water::Fraction((cx + cy * 64.f) * .1031f);
        value *= value + 33.33f;
        return water::Fraction(value * (value + value));
    };
    const float h00 = corner(wx, wy), h10 = corner(ex, wy);
    const float h01 = corner(wx, ey), h11 = corner(ex, ey);
    const float ux = fx * fx * (3.f - 2.f * fx), uy = fy * fy * (3.f - 2.f * fy);
    const float k = h00 - h10 - h01 + h11;
    return h00 + (h10 - h00) * ux + (h01 - h00) * uy + k * ux * uy;
}

inline float CausticNet(const Field& field, const Vec3& world) noexcept
{
    float cell[2];
    LatticeCell(field, world, cell);
    const float a = NoiseValue64(cell[0] + field.chopPhase[0], cell[1] + field.chopPhase[1]);
    // The fine octave's own coordinate: exactly twice the base frequency, with
    // finePhase carrying the doubled camera phase (UnderwaterFieldAt), so the sample
    // is 2*(cell + phase) - the chop's own second harmonic of the ABSOLUTE cell. The
    // constraints pick the number. The base octave is world-anchored because relative
    // cell plus camera phase is the absolute cell; the fine octave is anchored only
    // if its frequency multiplies BOTH halves, and because the phase wraps at the
    // lattice's 64-cell period that frequency must be an INTEGER, or the fine net
    // pops by the fractional remainder at every wrap the swimmer crosses. Twice is
    // the smallest such step; measured, the net's statistics with it are the old
    // net's (.195857 against .197391). The water surface's own fine ripple may shear
    // at 2.17/1.31 (a surface grain drifting is invisible under a drifting sea), but
    // this net is NAILED to the bed or it is a decal - the old spelling, which the
    // net inherited from that ripple, gave the two octaves two different rates
    // against the camera and the net crawled for both.
    const float b = NoiseValue64(cell[0] * 2.f + field.finePhase[0],
                                 cell[1] * 2.f + field.finePhase[1]);
    const float ridgeA = 1.f - std::fabs(2.f * a - 1.f);
    const float ridgeB = 1.f - std::fabs(2.f * b - 1.f);
    return std::pow(Saturate(ridgeA * ridgeB), kCausticSharpen);
}

// Where a ray of light reaching a point entered the water, in world-offset terms: walk
// back up toward the source until the liquid plane's own height over the camera.
// Sampling the surface's field THERE is what makes a beam a beam - the light arriving at
// this point came through that patch of surface, and a bright patch of surface makes a
// bright column under it.
inline Vec3 LightEntry(const Vec3& world, const Vec3& lightWorld, float submersion) noexcept
{
    const float rise = std::max(lightWorld.z, .08f);
    return world + lightWorld * ((submersion - world.z) / rise);
}

// The broad, fifty-yard warp the surface bends its crests with, at one lattice cell, in
// the same rotated/scaled coordinate the water's own two stages sample it from. One
// octave per beam sample is a budget decision: three samples of it cost what one sample
// of the crest family would, and a beam is a broad feature.
inline float BroadWarpCell(const Field& field, float cellX, float cellY) noexcept
{
    const float qx = (cellX * water::kWaveWarpCos + cellY * water::kWaveWarpSin)
        * water::kWaveWarpFrequency;
    const float qy = (-cellX * water::kWaveWarpSin + cellY * water::kWaveWarpCos)
        * water::kWaveWarpFrequency;
    // The warp's own phase, added after its rotation and scaling, which is the order the
    // water stage's WarpWavePlane adds it in: a beam anchored to the surface the player is
    // looking at has to move when that surface moves, at the rate it moves at. The phase
    // carries no time term, so this field is nailed to the world.
    return water::NoiseGradient(qx + field.warpPhase[0], qy + field.warpPhase[1]).value;
}

inline float BroadWarp(const Field& field, const Vec3& world) noexcept
{
    float cell[2];
    LatticeCell(field, world, cell);
    return BroadWarpCell(field, cell[0], cell[1]);
}

inline float ShaftDensity(const Field& field, const Vec3& world, const Vec3& lightWorld,
                          float submersion) noexcept
{
    // The march reads the warp where the light ENTERED for points along the ray, and
    // the entry point is affine in the march parameter - entry(t) = world*t +
    // L*(W - world.z*t)/rise is linear in t between LightEntry(0) and
    // LightEntry(world) - so its lattice cell is a lerp of the two ends' cells. One
    // entry evaluation per end instead of one per tap: the same numbers (bar float
    // rounding at the last place), and on the ps_3_0 side of the mirror about a
    // dozen slots this stage spent on making the caustic net's noise seamless.
    const Vec3 entry0 = LightEntry({ 0.f, 0.f, 0.f }, lightWorld, submersion);
    const Vec3 entry1 = LightEntry(world, lightWorld, submersion);
    float cell0[2], cell1[2];
    LatticeCell(field, entry0, cell0);
    LatticeCell(field, entry1, cell1);
    float total = 0.f;
    for (int sample = 1; sample <= kShaftSamples; ++sample)
    {
        const float t = float(sample) / float(kShaftSamples);
        total += BroadWarpCell(field, Lerp(cell0[0], cell1[0], t), Lerp(cell0[1], cell1[1], t));
    }
    // (mean-.5)*2*gain, with the centring done once over the mean instead of per sample:
    // the same number, and on the ps_3_0 side of the mirror it is six instruction slots
    // this stage does not have (it sits a handful under the 512 floor).
    return Saturate((total / float(kShaftSamples) - .5f) * (2.f * kShaftGain));
}

// The full-frame wobble, in uv. A camera inside the medium is looking through a moving
// interface it cannot focus past, which is the one cue that does not depend on anything
// being visible in the water; it is deliberately small, and it is uniform across the
// frame rather than per surface, because it belongs to the eye and not to the world.
inline void Wobble(const Settings& settings, float uvX, float uvY, float seconds,
                   float texelX, float texelY, float ramp, float out[2]) noexcept
{
    const float amount = Saturate(settings.distortion) * kMaxDistortionTexels * Saturate(ramp)
        / kWobblePeak;
    const float s = std::isfinite(seconds) ? seconds : 0.f;
    const float t = s - std::floor(s / kWobblePeriodSeconds) * kWobblePeriodSeconds;
    out[0] = amount * (std::sin(uvY * 38.f + t * 1.7f) + .6f * std::sin(uvY * 17.f - t * 1.1f)) * texelX;
    out[1] = amount * (std::sin(uvX * 34.f - t * 1.3f) + .6f * std::sin(uvX * 15.f + t * .9f)) * texelY;
}

// One pixel of the medium. The ps_3_0 stage in src/Effects/Underwater.h is this function,
// in the same order, with the same literals.
inline PixelOutput ShadePixel(const Settings& settings, const Medium& medium, const Field& field,
                              const PixelInput& in) noexcept
{
    PixelOutput out;
    // The same product the ps_3_0 stage takes: the authored amount of medium, times how far
    // the camera is into it. At ramp 0 this is 0, and every term below multiplies by it, so
    // "above water" is an exact identity in here and not only in prose.
    const float strength = Saturate(settings.strength) * Saturate(in.ramp);
    // 0. The medium ends at the surface. A ray looking up reaches the liquid plane before it
    //    reaches whatever the client drew beyond it - sky through an opening, a sail, a cliff
    //    face the water was never tiled over - and the segment past the plane is AIR, so
    //    counting it as water extinguishes the diver's brightest view by a column that is not
    //    there. This is the same statement the water pixel stage now makes about its own
    //    pixels (`column=min(lerp(effectiveDepth,interfaceYards,underwater),400.0)`): the water
    //    a submerged ray crosses is the camera-to-surface segment, and no more. Downward and
    //    level rays never reach the plane, so they are untouched, and a grazing upward ray
    //    meets it so far away that the minimum picks the real distance.
    const float rayUp = Dot(in.ray, in.up);
    // A select rather than a guarded assignment, matching the stage: a level ray would divide
    // by zero and a downward one would come out negative, and neither may reach the minimum.
    const float surfaceHit = (rayUp > 1e-3f && in.submersion > 0.f)
        ? in.submersion / std::max(rayUp, 1e-3f)
        : in.distance;
    const float columnYards = std::min(in.distance, surfaceHit);
    out.columnYards = columnYards;
    // And the point the net and the beam march read has to be the end of the WATER column, not
    // the end of the ray: for a pixel the client left as sky, that is where the ray leaves the
    // liquid, and marching a beam from a point in the air above the surface would march it
    // from nowhere. Scaling by the column's own fraction keeps this identical to the input for
    // every ray the clamp does not touch.
    const Vec3 shadedWorld = in.distance > 0.f ? in.world * (columnYards / in.distance) : in.world;
    // 0b. Water is not uniformly clear, and a flat extinction is the one thing that makes a
    //     medium read as a fog: silt hangs in the column, so murk grows with the water over
    //     the shaded point, and it arrives in patches a hundred yards across rather than
    //     everywhere at once. Both are properties of the PLACE - the world coordinate and the
    //     depth - not of the camera, so swimming through a patch changes what the water looks
    //     like the way swimming into a shadow changes what the light looks like. The product
    //     scales the column's exponent, which is the same as scaling the extinction per yard,
    //     and it is exactly mirrored in the ps_3_0 stage.
    const float under = in.submersion - shadedWorld.z;   // yards of water over this point
    float dirtCell[2];
    LatticeCell(field, shadedWorld * (1.f / kTurbidityPatchCells), dirtCell);
    const float patch = water::NoiseGradient(dirtCell[0] + kDirtSampleOffset[0],
                                             dirtCell[1] + kDirtSampleOffset[1]).value;
    // Turbidity scales the deviation, not the column: at 0 this is exactly 1 everywhere, so
    // the pass degenerates to the very column the surface's bottom path shades - the exact
    // agreement the regression pins - and at 1 the place gets the silt it was authored with.
    const float turbidity = std::clamp(settings.turbidity, 0.f, 3.f);
    const float silt = (.65f + .7f * patch)
        * (1.f + .9f * Saturate(under / kSiltYards));
    const float dirt = std::max(1.f + turbidity * (silt - 1.f), 0.f);
    // 0c. Snell's window. A ray the clamp above cut back to the plane continues into air only
    //     inside the critical cone; outside it the surface is a mirror from below, and what
    //     the client drew beyond the plane - a far shore at the horizon, sky past the liquid's
    //     draw distance - cannot arrive through it. Letting those pixels ride the short
    //     camera-to-plane column alone showed distant land from a yard under the surface as
    //     though through glass at an angle glass does not transmit at. So a ray that would
    //     leave the water outside the window is marched as though it never leaves: its
    //     effective column goes to the far cap, and steps 3-5 fill it with the medium's own
    //     scattered light and deep asymptote, which is what a mirrored underside reads as
    //     once the water between here and it has had its say. Rays that never reach the plane
    //     (downward, level, or ending on something submerged) are untouched, and inside the
    //     window the shore overhead stays as visible as the column over the diver allows -
    //     with one correction the optics demand and players asked for. Inside the window,
    //     the client drew a crisp sky and shore as though through a pane of glass. In real
    //     water the IMAGE of the world above dies faster than the ENERGY does: scattering
    //     does not just dim the incoming sky light, it scrambles its direction, so what
    //     survives a turbid column is the downwelling glow, not a readable picture of
    //     clouds. The exiting rays therefore pay an extra wash of the column, scaled by
    //     the same turbidity that dirties everything else - ExitWash: (1 + turbidity) on
    //     the water overhead plus the near-surface layer's own equivalent yards, so the
    //     trade is paid even by a swimmer a yard down, which is where the round-13 form
    //     let a dirty lake keep its pane of glass - while steps 3-4 still pay out the
    //     full scattered glow, which is the physical trade exactly: the dirtier the
    //     water, the sooner the window turns from a view into a lamp. At turbidity 0
    //     the wash is 1 and the window is the plain column-marched pane it was (and the
    //     bottom path's exact-agreement pin holds). Non-exiting rays - everything that
    //     ends under water - are untouched: their image was formed inside the medium
    //     and is only marched once.
    const float exitWindow = surfaceHit < in.distance
        ? Smoothstep(kSnellCos - .06f, kSnellCos + .06f, rayUp) : 1.f;
    const float exitWash = ExitWash(turbidity, in.submersion, surfaceHit < in.distance);
    const float effectiveYards = Lerp(kFarColumnYards, columnYards * dirt * exitWash, exitWindow);
    // 1. What survives of the world between here and the camera: the same per-channel
    //    Beer-Lambert the surface stands over, so red dies first and a deep pool reads
    //    teal rather than as a darker grey. STRENGTH interpolates the exponent's effect,
    //    not the coefficient, so 0 is exactly the frame this pass was handed.
    const Vec3 column = Transmittance(medium, effectiveYards);
    out.transmittance = { Lerp(1.f, column.x, strength), Lerp(1.f, column.y, strength),
                          Lerp(1.f, column.z, strength) };
    out.absorption = 1.f - out.transmittance.y;
    // 2. Caustics, on the surface BEFORE the medium eats them: a net thirty yards away is
    //    a net the water has already taken the red out of.
    float causticModulation = 1.f;
    // The gate the stage puts a [branch] on, and the reason it can: every factor in it is a
    // constant for the whole draw - the authored amount, the source's elevation and the frame's
    // own strength - so a tile takes it together and none of the work below happens at all when
    // caustics are off, when the sun is under the horizon or when STRENGTH is 0. Multiplied by
    // the same strength the transmittance is, because at STRENGTH 0 the modulation has to be
    // exactly 1: "no medium" means the frame this pass was handed, not that frame with a net
    // drawn on it.
    const float causticScale = Saturate(settings.caustics) * Saturate(in.sunElevation * 2.5f + .2f)
        * strength;
    if (causticScale > 0.f)
    {
        const float facing = Saturate(in.normal.z * 2.f - .2f);
        const float depthFade = Saturate(under * 1.5f) * Saturate(1.f - under / kCausticReachYards);
        out.caustic = causticScale * facing * depthFade;
        // Mean-preserving: at any amount the average modulation is exactly one, so the bed
        // is not brightened by its own caustics, only patterned.
        //
        // Evaluated only where it can be non-zero, which is an exact equivalence and not an
        // approximation: Lerp(1, net*N, 0) is 1 for any bounded net, so the two noise
        // evaluations are skipped rather than multiplied by zero. What skips them is a wall
        // (facing 0), a point past the net's reach (depthFade 0), a source below the horizon,
        // STRENGTH 0, and the sky - most of the pixels in most underwater frames. The ps_3_0
        // stage carries the same gate as a [branch], which fxc accepts there precisely
        // because nothing inside it samples with an implicit LOD.
        if (out.caustic > 0.f)
            causticModulation = Lerp(1.f, CausticNet(field, shadedWorld) * kCausticNormalization, out.caustic);
    }
    Vec3 colour = Mul(in.scene, out.transmittance) * causticModulation;
    // 3. Light scattered back out of the column toward the camera. The beams ride the same
    //    lobe and the same 1+2.2 shape the water stage gives its backlit crest, so the
    //    medium and the surface agree about which side of the water the source is on.
    const float g = PhaseG(in.sunElevation);
    const float vDotL = -Dot(in.ray, in.light);
    if (settings.shafts > 0.f && in.sunElevation > 0.f)
    {
        const Vec3 lightWorld = in.axis[0] * in.light.x + in.axis[1] * in.light.y
            + in.axis[2] * in.light.z;
        out.shaft = ShaftDensity(field, shadedWorld, lightWorld, in.submersion)
            * Saturate(settings.shafts) * Saturate(-vDotL * 1.4f - .25f);
    }
    const float lobe = PhaseLobe(g, vDotL) * Saturate(in.sunElevation * 2.5f + .2f);
    out.inScatter = Mul(ScatterColour(medium), in.lightColour)
        * (out.absorption * lobe * (1.f + 2.2f * out.shaft) * strength);
    // 4. Looking up, the medium is lit by the whole sky over the surface. This is the term
    //    that keeps a pixel the client left as sky - no liquid tile drawn there - reading as
    //    water overhead instead of as a flat deep colour, and with the column cut back to the
    //    plane at step 0 it reads as the sky seen through exactly the water over the diver's
    //    head: bright from just under the surface, and from down deep almost entirely this
    //    term and the asymptote, because at sixty yards there is no sky left to see.
    const float upLook = Saturate(Dot(in.ray, in.up));
    out.inScatter = out.inScatter
        + Mul(SkyGlowColour(medium), in.lightColour)
            * (out.absorption * Saturate(settings.surfaceGlow) * upLook
               * Saturate(in.sunElevation * 2.5f + .2f) * strength);
    colour = colour + out.inScatter;
    // 5. And the column's own asymptote: the colour the light that never arrives leaves
    //    behind, which is the body's authored deep colour and nothing else.
    out.colour = colour + medium.deep * (out.absorption * strength);
    return out;
}

}
}
