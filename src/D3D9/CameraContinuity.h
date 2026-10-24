#pragma once

#include <algorithm>
#include <cmath>

namespace renderer {

// Standard D3D row-vector perspective projection, including the valid
// infinite-far-plane form where P22 is exactly 1 (or rounds to 1 in float).
// Orthographic, reversed-depth, and otherwise unrelated projection matrices
// remain ineligible as camera evidence.
inline bool IsStandardD3DPerspectiveProjection(float p00, float p11, float p22,
                                               float p23, float p32, float p33) noexcept
{
    return std::isfinite(p00) && std::isfinite(p11) && std::isfinite(p22) &&
           std::isfinite(p23) && std::isfinite(p32) && std::isfinite(p33) &&
           p00 > 0.0f && p11 > 0.0f && p22 >= 1.0f - 1.0e-6f && p32 < 0.0f &&
           std::fabs(p23 - 1.0f) <= 0.001f && std::fabs(p33) <= 0.001f;
}

// Decides whether a candidate view matrix is the frame's real camera.
//
// CameraCapture reads the projection, the light direction and the daylight
// factors off whichever scene draw happens to bind the full vertex block first.
// That is sound only while every qualifying draw shares one camera - and a
// target/portrait-style draw does not: it is a close-up camera of its own, so
// the frame's sky and edge fog suddenly inherit the portrait's light. (That is
// the reported "click an NPC and the world brightens": selecting a target is
// the one routine UI action that adds such a draw to the scene pass.)
//
// The gate keeps the last accepted camera (position + orthonormal basis rows)
// and accepts a candidate when it is plausibly the same camera: near it in
// space, with its basis barely rotated in one frame. A rejected candidate
// changes nothing - the caller simply does not latch, and the real scene draw
// that follows in the same frame still can. A genuine teleport or cut is not
// locked out forever: three consecutive candidates that agree with EACH OTHER
// re-anchor the gate (a portrait stream also agrees with itself, but the real
// camera's draw arrives every frame too, and three in a row are required while
// the accepted camera keeps being refreshed by its own draws meanwhile).
class CameraContinuityGate {
public:
    bool Valid() const { return valid; }
    void Reset() { valid=false; candidates=0; }
    const float* Position() const { return camera; }

    bool MatchesViewport(unsigned viewportWidth, unsigned viewportHeight) const {
        return valid && viewportWidth == width && viewportHeight == height;
    }

    // A full-frame shader capture can have a valid current orientation that the
    // continuity gate rejects after a hard camera turn. It is safe to offer that
    // orientation to sky-pose corroboration only when its independently decoded
    // eye still agrees with the accepted world anchor.
    bool NearAcceptedEye(const float position[3], float maxDistance) const {
        return valid && std::isfinite(maxDistance) && maxDistance >= 0.f &&
            std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) &&
            Distance(position, camera) <= maxDistance;
    }

    bool MatchesBasis(const float viewBasis[3][4], unsigned viewportWidth, unsigned viewportHeight) const {
        return MatchesViewport(viewportWidth, viewportHeight) && BasisDot(viewBasis, basis) > .7f;
    }

    // Sky fallback can see a legitimate camera turn of many degrees in one
    // frame (for example, snapping the mouse-look camera straight up). Keep the
    // same world position/viewport continuity guard as the normal capture, but
    // do not mistake a pitch change for a different camera.
    bool AcceptFallback(const float viewTranslation[4], const float viewBasis[3][4],
                        unsigned viewportWidth, unsigned viewportHeight) {
        const float position[3]={
            -(viewTranslation[0]*viewBasis[0][0]+viewTranslation[1]*viewBasis[0][1]+viewTranslation[2]*viewBasis[0][2]),
            -(viewTranslation[0]*viewBasis[1][0]+viewTranslation[1]*viewBasis[1][1]+viewTranslation[2]*viewBasis[1][2]),
            -(viewTranslation[0]*viewBasis[2][0]+viewTranslation[1]*viewBasis[2][1]+viewTranslation[2]*viewBasis[2][2])};
        if (!valid) {
            Anchor(position, viewBasis, viewportWidth, viewportHeight);
            return true;
        }
        if (!MatchesViewport(viewportWidth, viewportHeight) || Distance(position, camera) > 40.f)
            return false;
        Anchor(position, viewBasis, viewportWidth, viewportHeight);
        return true;
    }

    void UpdateBasis(const float viewBasis[3][4], unsigned viewportWidth, unsigned viewportHeight) {
        if (!valid) return;
        for (int r = 0; r < 3; ++r)
            std::copy(viewBasis[r], viewBasis[r] + 3, basis[r]);
        width = viewportWidth; height = viewportHeight;
        candidates = 0;
    }

    // viewTranslation: row 3 of the row-vector VIEW constants (v[3] = -c * R).
    // viewBasis: rows v[0..2], first three elements each (row i = world axis i in view space).
    bool Accept(const float viewTranslation[4], const float viewBasis[3][4],
                unsigned viewportWidth, unsigned viewportHeight) {
        // Camera position in world space: c = -t * R^T (row i of R dotted with t),
        // matching CameraCapture's c7 calculation (outConstants[7][i] -= v[3][j] * v[i][j]).
        const float position[3]={
            -(viewTranslation[0]*viewBasis[0][0]+viewTranslation[1]*viewBasis[0][1]+viewTranslation[2]*viewBasis[0][2]),
            -(viewTranslation[0]*viewBasis[1][0]+viewTranslation[1]*viewBasis[1][1]+viewTranslation[2]*viewBasis[1][2]),
            -(viewTranslation[0]*viewBasis[2][0]+viewTranslation[1]*viewBasis[2][1]+viewTranslation[2]*viewBasis[2][2])};
        if (!valid) {
            Anchor(position, viewBasis, viewportWidth, viewportHeight);
            return true;
        }
        if (viewportWidth != width || viewportHeight != height) {
            // A different frustum slice (portrait box, minimap, reflection):
            // never the scene camera, and not a teleport candidate either.
            return false;
        }
        if (Distance(position, camera) <= 40.f && BasisDot(viewBasis, basis) > .7f) {
            Anchor(position, viewBasis, viewportWidth, viewportHeight);
            return true;
        }
        // Teleport escape: consecutive candidates agreeing with each other.
        if (candidates > 0 && Distance(position, candidate) <= 15.f &&
            BasisDot(viewBasis, candidateBasis) > .99f) {
            if (++candidates >= 3) {
                Anchor(position, viewBasis, viewportWidth, viewportHeight);
                return true;
            }
        } else {
            std::copy(position, position + 3, candidate);
            for (int r = 0; r < 3; ++r)
                std::copy(viewBasis[r], viewBasis[r] + 3, candidateBasis[r]);
            candidates = 1;
        }
        return false;
    }

private:
    static float Distance(const float a[3], const float b[3]) {
        const float dx=a[0]-b[0], dy=a[1]-b[1], dz=a[2]-b[2];
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }
    // Lowest row agreement between two bases: 1 for identical cameras, and it
    // falls fast for a camera somewhere else, like a portrait box's.
    static float BasisDot(const float a[3][4], const float b[3][4]) {
        float lowest = 1.f;
        for (int r = 0; r < 3; ++r)
            lowest = std::min(lowest, a[r][0]*b[r][0] + a[r][1]*b[r][1] + a[r][2]*b[r][2]);
        return lowest;
    }
    void Anchor(const float position[3], const float viewBasis[3][4],
                unsigned viewportWidth, unsigned viewportHeight) {
        std::copy(position, position + 3, camera);
        for (int r = 0; r < 3; ++r)
            std::copy(viewBasis[r], viewBasis[r] + 3, basis[r]);
        width = viewportWidth; height = viewportHeight;
        valid = true; candidates = 0;
    }

    float camera[3]{}, candidate[3]{};
    float basis[3][4]{}, candidateBasis[3][4]{};
    unsigned width = 0, height = 0;
    int candidates = 0;
    bool valid = false;
};

// ---------------------------------------------------------------------------
// Sky fallback: which fixed-function VIEW may become this frame's camera
// ---------------------------------------------------------------------------
// A shader-driven client uploads its view as vertex-shader constants and only
// touches the fixed-function transforms on the draws that still need them -
// WoW's sun and moon billboards above all. Whatever else is left in D3DTS_VIEW
// on any other draw is an artefact of the last such draw, and a full-screen
// pass leaves it at identity. Reading that as "the camera" is what made the
// sky and the dynamic lights wrong in a city, where the terrain capture never
// latches because everything overhead is culled: the first fixed-function
// transform of the frame won, the world was anchored to it, and the sky then
// rotated with the mouse instead of with the world.
//
// The fallback therefore stops taking the first transform it sees. It collects
// every candidate the frame offers and publishes one only when it can be
// corroborated, by the frame's own sun or moon disc when one was drawn, or by
// the camera that was already accepted otherwise. A candidate that appears
// from nowhere is held for one frame first, so one stray full-screen transform
// cannot take the frame.

// Lowest row agreement between two view bases: 1 for the same orientation,
// falling fast for a different one. (Row i of a world->view basis is world
// axis i carried into view space; a rotation about one axis leaves the other
// two rows unchanged, so the minimum is the honest measure.)
inline float BasisAgreement(const float a[3][4], const float b[3][4]) noexcept
{
    float lowest = 1.f;
    for (int r = 0; r < 3; ++r)
        lowest = std::min(lowest, a[r][0] * b[r][0] + a[r][1] * b[r][1] + a[r][2] * b[r][2]);
    return lowest;
}

// Where a world direction lands on screen (0..1, v down) through a view basis
// and the projection's P00/P11 - the same convention CameraCapture and
// CelestialTracker use for the disc. False when the direction is at or behind
// the eye, where a screen position does not exist.
inline bool ProjectWorldDirection(const float basis[3][4], float projectionX, float projectionY,
                                  const float world[3], float& screenX, float& screenY) noexcept
{
    // basis holds rows of the row-vector world->view transform, so a world
    // direction becomes view space by dotting with its COLUMNS (this is the
    // same expression as MathTypes' TransformDirectionRowVector and
    // CelestialTracker's screen projection; dotting with the rows instead is
    // the inverse rotation, and projects the disc to the wrong half of the
    // screen for every yaw).
    const float vx = basis[0][0] * world[0] + basis[1][0] * world[1] + basis[2][0] * world[2];
    const float vy = basis[0][1] * world[0] + basis[1][1] * world[1] + basis[2][1] * world[2];
    const float vz = basis[0][2] * world[0] + basis[1][2] * world[1] + basis[2][2] * world[2];
    if (!(vz > 1e-3f) || !(projectionX > 1e-3f) || !(projectionY > 1e-3f))
        return false;
    screenX = 0.5f + 0.5f * (vx / vz) * projectionX;
    screenY = 0.5f - 0.5f * (vy / vz) * projectionY;
    return std::isfinite(screenX) && std::isfinite(screenY);
}

// What the frame's own celestial draw says about the camera. `world` is the
// stabilized world direction towards the body; `screenX/Y` where the client
// actually painted it this frame. Both come from independent observations
// (CelestialTracker's billboard hook and the captured light), so agreement is
// real evidence rather than a pose compared with itself.
struct DiscObservation
{
    bool available = false;
    float world[3] = { 0.f, 0.f, 1.f };
    float screenX = 0.f, screenY = 0.f;
    float projectionX = 0.f, projectionY = 0.f;
    // Normalized screen units, ~195 px at 1080p: wide enough to absorb the
    // tracker's own sampling and the frame's FOV, far tighter than the half
    // screen a basis borrowed from another camera moves the disc by.
    float tolerance = .18f;
};

// True when `basis` puts the observed body somewhere the client did not paint
// it, i.e. this basis is not the camera the frame was drawn with.
inline bool ContradictsDisc(const float basis[3][4], const DiscObservation& disc) noexcept
{
    if (!disc.available)
        return false;
    float x = 0.f, y = 0.f;
    if (!ProjectWorldDirection(basis, disc.projectionX, disc.projectionY, disc.world, x, y))
        return true; // the body is in front of the camera; this basis puts it behind
    return std::abs(x - disc.screenX) > disc.tolerance ||
           std::abs(y - disc.screenY) > disc.tolerance;
}

class SkyPoseSelector
{
public:
    // A candidate is "continuous" with the camera we already have when its
    // rows still agree to within this. A mouse-look frame moves the view by a
    // degree or two; the bound only has to admit a genuine fast turn and
    // reject a transform from somewhere else.
    static constexpr float kContinuityFloor = .75f;    // ~41 degrees
    // A candidate that is not continuous is believed only after this many
    // consecutive frames say the same thing: long enough to outlive a stray
    // full-screen transform, short enough that a real snap is followed at once.
    static constexpr int kJumpConfirmationFrames = 2;
    // A frame may expose several full-frame transforms before the actual sky
    // camera. Keep enough alternatives that early model/reflection draws do not
    // crowd it out before the celestial pass arrives.
    static constexpr unsigned kMaxCandidates = 16;

    void Reset() noexcept
    {
        m_candidateCount = 0;
        m_haveReference = false;
        m_pendingValid = false;
        m_pendingFrames = 0;
    }

    // Called once per frame, before any draw: candidates are per frame.
    void BeginFrame() noexcept { m_candidateCount = 0; }

    bool HasCandidate() const noexcept { return m_candidateCount != 0; }
    unsigned CandidateCount() const noexcept { return m_candidateCount; }
    bool HasReference() const noexcept { return m_haveReference; }

    // A real capture re-synchronises the selector: the next sky candidate is
    // judged against the camera the draw stream itself just confirmed.
    void SetReference(const float basis[3][4]) noexcept
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m_reference[r][c] = basis[r][c];
        m_haveReference = true;
        m_pendingValid = false;
        m_pendingFrames = 0;
    }

    void Observe(const float basis[3][4]) noexcept
    {
        // Keep a handful, not one: the frame can offer the camera's own sky
        // transform and, before or after it, the leftover of some other pass.
        // Which one is the camera is decided in Resolve, against evidence that
        // only exists once the frame has been drawn.
        for (unsigned i = 0; i < m_candidateCount; ++i)
            if (BasisAgreement(basis, m_candidates[i]) > .9999f)
                return;
        if (m_candidateCount < kMaxCandidates)
        {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m_candidates[m_candidateCount][r][c] = basis[r][c];
            ++m_candidateCount;
        }
    }

    // Picks this frame's basis and says whether it is corroborated.
    bool Resolve(const DiscObservation& disc, float outBasis[3][4], bool& corroborated) noexcept
    {
        corroborated = false;
        if (!m_candidateCount)
            return false;

        // With a disc on screen the frame carries its own camera evidence:
        // take the candidate that reproduces where the client painted it.
        if (disc.available)
        {
            int best = -1;
            float bestError = 0.f;
            for (unsigned i = 0; i < m_candidateCount; ++i)
            {
                if (ContradictsDisc(m_candidates[i], disc))
                    continue;
                float x = 0.f, y = 0.f;
                ProjectWorldDirection(m_candidates[i], disc.projectionX, disc.projectionY,
                                      disc.world, x, y);
                const float error = std::max(std::abs(x - disc.screenX), std::abs(y - disc.screenY));
                if (best < 0 || error < bestError) { best = int(i); bestError = error; }
            }
            if (best >= 0)
            {
                Copy(m_candidates[best], outBasis);
                corroborated = true;
                SetReference(outBasis);
                return true;
            }
            // The observation may be stale or its independently tracked world
            // direction may not be usable this frame. Do not let contradictory
            // disc evidence veto an otherwise continuous camera; run the same
            // bounded continuity/jump confirmation below as a no-disc frame.
        }

        // No usable disc evidence: fall back to continuity with the accepted camera.
        if (m_haveReference)
        {
            int best = -1;
            float bestAgreement = 0.f;
            for (unsigned i = 0; i < m_candidateCount; ++i)
            {
                const float agreement = BasisAgreement(m_candidates[i], m_reference);
                if (agreement > bestAgreement) { bestAgreement = agreement; best = int(i); }
            }
            if (best >= 0 && bestAgreement >= kContinuityFloor)
            {
                Copy(m_candidates[best], outBasis);
                corroborated = true;
                SetReference(outBasis);
                return true;
            }
            // Discontinuous: believe it only once the frame has said the same
            // thing for kJumpConfirmationFrames, then let it become the
            // reference. A transform that changes every frame never gets there.
            const float (*chosen)[4] = m_candidates[0];
            if (m_pendingValid && BasisAgreement(chosen, m_pending) >= .999f)
            {
                Copy(chosen, m_pending);
                if (++m_pendingFrames >= kJumpConfirmationFrames)
                {
                    Copy(chosen, outBasis);
                    corroborated = true;
                    SetReference(outBasis);
                    m_pendingValid = false;
                    m_pendingFrames = 0;
                    return true;
                }
            }
            else
            {
                Copy(chosen, m_pending);
                m_pendingValid = true;
                m_pendingFrames = 1;
            }
            return false;
        }

        // Nothing accepted yet this session: the first candidate anchors.
        Copy(m_candidates[0], outBasis);
        corroborated = true;
        SetReference(outBasis);
        return true;
    }

private:
    static void Copy(const float from[3][4], float to[3][4]) noexcept
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                to[r][c] = from[r][c];
    }

    float m_candidates[kMaxCandidates][3][4]{};
    unsigned m_candidateCount = 0;
    float m_reference[3][4]{};
    bool m_haveReference = false;
    float m_pending[3][4]{};
    int m_pendingFrames = 0;
    bool m_pendingValid = false;
};

// Converts a direction expressed in the captured camera's view space into

// world space, in the storage layout of viewRaw (the v[0..3] shader constants,
// whose transpose is the inverse view - CameraCapture builds c4..c7 exactly
// this way, and derives its world sun direction with this same formula). Do
// not "unify" this with CelestialStability's helper: that one consumes a
// D3DMATRIX from GetTransform, whose storage is the other transpose. Valid
// only when the capture's orthonormality check accepted the rows, which is
// exactly the precondition for viewRaw.
inline void ViewDirectionToWorld(const float viewRaw[16], const float viewDirection[3], float worldOut[3]) {
    // Row-vector world->view (pos_view = pos_world * V) has inverse rotation V^T:
    // world_i = dot(row_i(V), viewDirection), matching CameraCapture's sunWorld
    // (where invX/invY/invZ already hold the columns of V, so invX.x*lv.x + invY.x*lv.y + invZ.x*lv.z
    // is v[0][0]*lv.x + v[0][1]*lv.y + v[0][2]*lv.z).
    worldOut[0] = viewRaw[0]*viewDirection[0] + viewRaw[1]*viewDirection[1] + viewRaw[2]*viewDirection[2];
    worldOut[1] = viewRaw[4]*viewDirection[0] + viewRaw[5]*viewDirection[1] + viewRaw[6]*viewDirection[2];
    worldOut[2] = viewRaw[8]*viewDirection[0] + viewRaw[9]*viewDirection[1] + viewRaw[10]*viewDirection[2];
    const float length = std::sqrt(worldOut[0]*worldOut[0] + worldOut[1]*worldOut[1] + worldOut[2]*worldOut[2]);
    if (length > 1e-6f) { worldOut[0] /= length; worldOut[1] /= length; worldOut[2] /= length; }
}

// Convert a view-space point (such as a torch vertex recovered from the
// client's skinning constants) through the same accepted camera pose. Keeping
// the translation explicit prevents a view-relative point from becoming a
// light that follows the camera when its pitch/yaw changes.
inline void ViewPositionToWorld(const float viewRaw[16], const float camera[3],
                                const float viewPosition[3], float worldOut[3]) {
    worldOut[0] = camera[0] + viewRaw[0]*viewPosition[0] + viewRaw[1]*viewPosition[1] + viewRaw[2]*viewPosition[2];
    worldOut[1] = camera[1] + viewRaw[4]*viewPosition[0] + viewRaw[5]*viewPosition[1] + viewRaw[6]*viewPosition[2];
    worldOut[2] = camera[2] + viewRaw[8]*viewPosition[0] + viewRaw[9]*viewPosition[1] + viewRaw[10]*viewPosition[2];
}

} // namespace renderer
