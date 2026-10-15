#pragma once
#include <d3d9.h>
#include <unordered_map>
#include <mutex>
#include "MaterialType.h"
#include "DrawCallContext.h"

namespace renderer
{
    namespace water { struct Observation; }

    class DrawCallClassifier
    {
    public:
        static DrawCallClassifier& Instance();

        DrawClassification Classify(
            IDirect3DDevice9* device,
            const DrawCallContext& ctx,
            const float capturedViewTranslation[3],
            bool capturedViewValid,
            uint64_t cameraCaptureShaderHash,
            const water::Observation* waterObservation = nullptr);

        void ClearCache();

        uint64_t GetCacheHits() const { return m_cacheHits; }
        uint64_t GetCacheMisses() const { return m_cacheMisses; }

    private:
        DrawCallClassifier() = default;

        DrawClassification ClassifyInternal(
            IDirect3DDevice9* device,
            const DrawCallContext& ctx,
            const float capturedViewTranslation[3],
            bool capturedViewValid,
            uint64_t cameraCaptureShaderHash);

        std::unordered_map<PipelineKey, DrawClassification> m_cache;
        uint64_t m_cacheHits = 0;
        uint64_t m_cacheMisses = 0;
    };
}
