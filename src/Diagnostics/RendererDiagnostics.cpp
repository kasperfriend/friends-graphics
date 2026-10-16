#include "RendererDiagnostics.h"
#include "../Core/ShaderCache.h"
#include "../Scene/MaterialType.h"
#include <fstream>
#include <filesystem>
#include <sstream>

namespace renderer
{
    RendererDiagnostics& RendererDiagnostics::Instance()
    {
        static RendererDiagnostics instance;
        return instance;
    }

    void RendererDiagnostics::SetLogPath(const std::wstring& path)
    {
        std::lock_guard lock(m_mutex);
        m_logPath = path;
    }

    void RendererDiagnostics::OnFrameBegin()
    {
    }

    void RendererDiagnostics::OnFrameEnd()
    {
        // The render thread's whole-frame batch is committed under the lock
        // exactly once here; between frames the Record* paths never touch
        // m_mutex at all.
        if (m_frameBatch.drawCalls || m_frameBatch.cameraCaptures ||
            m_frameBatch.depthCaptures || m_frameBatch.compositedFrames)
        {
            std::lock_guard lock(m_mutex);
            m_counters.totalDrawCalls += m_frameBatch.drawCalls;
            m_counters.classifiedDrawCalls += m_frameBatch.classifiedDrawCalls;
            m_counters.unknownDrawCalls += m_frameBatch.unknownDrawCalls;
            m_counters.shadowCasterDraws += m_frameBatch.shadowCasterDraws;
            m_counters.waterDraws += m_frameBatch.waterDraws;
            m_counters.uiDraws += m_frameBatch.uiDraws;
            m_counters.cameraCaptures += m_frameBatch.cameraCaptures;
            m_counters.depthCaptures += m_frameBatch.depthCaptures;
            m_counters.compositedFrames += m_frameBatch.compositedFrames;
            m_frameBatch = FrameBatch{};
        }
        std::lock_guard lock(m_mutex);
        ++m_counters.totalFrames;

        if (m_counters.totalFrames % 600 == 120)
        {
            FlushAggregatedLog();
        }
    }

    void RendererDiagnostics::RecordDrawCall(bool classified, bool isWater, bool isShadowCaster, bool isUI)
    {
        // Render-thread-only: lock-free batch, committed in OnFrameEnd.
        ++m_frameBatch.drawCalls;

        if (classified)
            ++m_frameBatch.classifiedDrawCalls;
        else
            ++m_frameBatch.unknownDrawCalls;

        if (isWater)
            ++m_frameBatch.waterDraws;
        if (isShadowCaster)
            ++m_frameBatch.shadowCasterDraws;
        if (isUI)
            ++m_frameBatch.uiDraws;
    }

    void RendererDiagnostics::RecordDrawCall(const DrawClassification& dc)
    {
        bool classified = (dc.material != MaterialType::Unknown);
        RecordDrawCall(classified, dc.isWater, dc.castsShadow, dc.isUI);
    }

    void RendererDiagnostics::RecordCameraCapture()
    {
        ++m_frameBatch.cameraCaptures;
    }

    void RendererDiagnostics::RecordDepthCapture()
    {
        ++m_frameBatch.depthCaptures;
    }

    void RendererDiagnostics::RecordComposite()
    {
        ++m_frameBatch.compositedFrames;
    }

    void RendererDiagnostics::Log(const std::string& category, const std::string& message)
    {
        std::lock_guard lock(m_mutex);
        if (m_logPath.empty())
            return;

        std::ofstream out(std::filesystem::path(m_logPath), std::ios::app);
        if (out)
        {
            out << "[" << category << "] " << message << "\n";
        }
    }

    void RendererDiagnostics::LogOnce(const std::string& category, const std::string& key, const std::string& message)
    {
        std::lock_guard lock(m_mutex);
        if (m_loggedOnceKeys.count(key) > 0)
            return;

        m_loggedOnceKeys.insert(key);

        if (m_logPath.empty())
            return;

        std::ofstream out(std::filesystem::path(m_logPath), std::ios::app);
        if (out)
        {
            out << "[" << category << "] " << message << "\n";
        }
    }

    void RendererDiagnostics::FlushAggregatedLog()
    {
        if (m_logPath.empty())
            return;

        std::ofstream out(std::filesystem::path(m_logPath), std::ios::app);
        if (!out)
            return;

        auto& sc = ShaderCache::Instance();
        out << "[Perf] frames=" << m_counters.totalFrames
            << " draws=" << m_counters.totalDrawCalls
            << " classified=" << m_counters.classifiedDrawCalls
            << " unknown=" << m_counters.unknownDrawCalls
            << " shadows=" << m_counters.shadowCasterDraws
            << " water=" << m_counters.waterDraws
            << " ui=" << m_counters.uiDraws
            << " camera=" << m_counters.cameraCaptures
            << " depth=" << m_counters.depthCaptures
            << " composited=" << m_counters.compositedFrames
            << " shaderCacheHits=" << sc.GetHits()
            << " shaderCacheMisses=" << sc.GetMisses()
            << "\n";
    }
}
