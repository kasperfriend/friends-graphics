#pragma once

#include <algorithm>
#include <cmath>

namespace renderer {

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

    bool MatchesBasis(const float viewBasis[3][4], unsigned viewportWidth, unsigned viewportHeight) const {
        return valid && viewportWidth == width && viewportHeight == height && BasisDot(viewBasis, basis) > .7f;
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

} // namespace renderer
