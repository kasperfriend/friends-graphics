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

    uint64_t RendererDiagnostics::Frames() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_counters.totalFrames;
    }

    void RendererDiagnostics::OnFrameEnd()
    {
        // The render thread's whole-frame batch is committed under the lock
        // exactly once here; between frames the Record* paths never touch
        // m_mutex at all. The batch commit and the frame count share one
        // critical section: a second acquisition after the first would cost
        // every frame an extra lock/unlock pair for nothing.
        {
            std::lock_guard lock(m_mutex);
            if (m_frameBatch.drawCalls || m_frameBatch.cameraCaptures ||
                m_frameBatch.depthCaptures || m_frameBatch.compositedFrames)
            {
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
            ++m_counters.totalFrames;

            if (m_counters.totalFrames % 600 == 120)
            {
                FlushAggregatedLog();
            }
        }
    }

    void RendererDiagnostics::RecordDrawCall()
    {
        // No lock and no device read on purpose: this is called from every draw
        // hook, and the line it feeds is read by anyone asking what the renderer
        // is doing. It used to be that NOTHING fed it unless the shadow map or the
        // water capture happened to want a classification, so a session with a
        // million draws logged `draws=0` - a number that reads like a renderer
        // that never ran.
        //
        // Nothing else is touched here: a second, per-frame counter would have to
        // be reset by a frame boundary this call cannot see, and it was dead state
        // (written every draw, read by nothing) until the stall line was given the
        // session total to difference instead (see DrawCalls).
        m_drawCalls.fetch_add(1, std::memory_order_relaxed);
    }

    void RendererDiagnostics::RecordDrawCall(bool classified, bool isWater, bool isShadowCaster, bool isUI)
    {
        // Render-thread-only: lock-free batch, committed in OnFrameEnd.
        // The session total is NOT counted here: it is fed by the no-arg
        // RecordDrawCall from every draw hook into the atomic m_drawCalls,
        // which is what DrawCalls() (the stall line) reads.
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
        // `draws=` is every draw the client made; the four numbers after it are
        // the subset a feature had to classify (the shadow map's casters, the
        // water and ground surfaces), so a session whose features are switched
        // off reads its true draw count with zeros next to it - which is the
        // difference between the line and the `draws=0` it used to print for a
        // renderer that was drawing a million of them.
        out << "[Perf] frames=" << m_counters.totalFrames
            << " draws=" << m_drawCalls.load(std::memory_order_relaxed)
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
