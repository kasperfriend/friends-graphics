#include "PerformanceProfiler.h"
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace renderer
{
    PerformanceProfiler& PerformanceProfiler::Instance()
    {
        static PerformanceProfiler instance;
        return instance;
    }

    PerformanceProfiler::PerformanceProfiler()
    {
        QueryPerformanceFrequency(&m_qpcFrequency);
    }

    void PerformanceProfiler::SetLogPath(const std::wstring& path)
    {
        std::lock_guard lock(m_mutex);
        m_logPath = path;
    }

    void PerformanceProfiler::ResetGpuQueriesUnlocked()
    {
        for (auto& slot : m_gpuRing)
        {
            slot.disjoint.Reset();
            slot.freq.Reset();
            for (auto& s : slot.start) s.Reset();
            for (auto& e : slot.end) e.Reset();
            slot.frameIssued = false;
            for (auto& issued : slot.issued) issued = false;
        }
        m_gpuDeviceOwner = nullptr;
        m_gpuQueriesSupported = false;
        m_gpuWriteSlot = 0;
    }

    void PerformanceProfiler::Reset(IDirect3DDevice9* /*device*/)
    {
        std::lock_guard lock(m_mutex);
        ResetGpuQueriesUnlocked();
    }

    void PerformanceProfiler::EnsureGpuQueries(IDirect3DDevice9* device)
    {
        if (m_gpuDeviceOwner == device)
            return;

        // OnFrameBegin holds m_mutex while entering this function. Reset()
        // takes that same mutex, so use the unlocked helper here rather than
        // recursively locking a non-recursive mutex on first use/device change.
        ResetGpuQueriesUnlocked();
        m_gpuDeviceOwner = device;
        if (!device) return;

        bool allOk = true;
        for (auto& slot : m_gpuRing)
        {
            if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, slot.disjoint.GetAddressOf())) ||
                FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, slot.freq.GetAddressOf())))
            {
                allOk = false;
                break;
            }

            for (size_t s = 0; s < static_cast<size_t>(GpuPerfStage::GpuStageCount); ++s)
            {
                if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, slot.start[s].GetAddressOf())) ||
                    FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, slot.end[s].GetAddressOf())))
                {
                    allOk = false;
                    break;
                }
            }
        }

        m_gpuQueriesSupported = allOk;
        if (!allOk)
        {
            ResetGpuQueriesUnlocked();
        }
    }

    void PerformanceProfiler::OnFrameBegin(IDirect3DDevice9* device)
    {
        std::lock_guard lock(m_mutex);
        if (!m_gpuProfilingEnabled || !device)
            return;

        EnsureGpuQueries(device);
        if (!m_gpuQueriesSupported)
            return;

        // Poll an older slot (2 frames ago) asynchronously with NO FLUSH
        uint32_t readSlot = (m_gpuWriteSlot + 1) % QUERY_RING_DEPTH;
        auto& slot = m_gpuRing[readSlot];

        if (slot.frameIssued && slot.disjoint && slot.freq)
        {
            BOOL disjoint = FALSE;
            UINT64 freq = 0;
            if (slot.disjoint->GetData(&disjoint, sizeof(disjoint), 0) == S_OK &&
                slot.freq->GetData(&freq, sizeof(freq), 0) == S_OK && !disjoint && freq > 0)
            {
                for (size_t s = 0; s < static_cast<size_t>(GpuPerfStage::GpuStageCount); ++s)
                {
                    if (slot.issued[s] && slot.start[s] && slot.end[s])
                    {
                        UINT64 startTime = 0, endTime = 0;
                        if (slot.start[s]->GetData(&startTime, sizeof(startTime), 0) == S_OK &&
                            slot.end[s]->GetData(&endTime, sizeof(endTime), 0) == S_OK &&
                            endTime >= startTime)
                        {
                            double ms = static_cast<double>(endTime - startTime) * 1000.0 / static_cast<double>(freq);
                            m_accumulatedGpuMs[s] += ms;
                            m_gpuSampleCounts[s]++;
                        }
                    }
                }
                slot.frameIssued = false;
            }
        }

        // Begin current frame slot
        auto& cur = m_gpuRing[m_gpuWriteSlot];
        for (size_t s = 0; s < static_cast<size_t>(GpuPerfStage::GpuStageCount); ++s)
        {
            cur.issued[s] = false;
        }
        if (cur.disjoint)
            cur.disjoint->Issue(D3DISSUE_BEGIN);
    }

    void PerformanceProfiler::BeginGpuStage(IDirect3DDevice9* device, GpuPerfStage stage)
    {
        if (!m_gpuProfilingEnabled || !m_gpuQueriesSupported || !device) return;
        size_t idx = static_cast<size_t>(stage);
        auto& cur = m_gpuRing[m_gpuWriteSlot];
        if (cur.start[idx])
        {
            cur.start[idx]->Issue(D3DISSUE_END);
            cur.issued[idx] = true;
        }
    }

    void PerformanceProfiler::EndGpuStage(IDirect3DDevice9* device, GpuPerfStage stage)
    {
        if (!m_gpuProfilingEnabled || !m_gpuQueriesSupported || !device) return;
        size_t idx = static_cast<size_t>(stage);
        auto& cur = m_gpuRing[m_gpuWriteSlot];
        if (cur.end[idx] && cur.issued[idx])
        {
            cur.end[idx]->Issue(D3DISSUE_END);
        }
    }

    void PerformanceProfiler::OnFrameEnd(IDirect3DDevice9* device)
    {
        std::lock_guard lock(m_mutex);

        if (m_gpuProfilingEnabled && m_gpuQueriesSupported && device)
        {
            auto& cur = m_gpuRing[m_gpuWriteSlot];
            if (cur.disjoint) cur.disjoint->Issue(D3DISSUE_END);
            if (cur.freq) cur.freq->Issue(D3DISSUE_END);
            cur.frameIssued = true;
            m_gpuWriteSlot = (m_gpuWriteSlot + 1) % QUERY_RING_DEPTH;
        }

        ++m_frameCounter;
        if (m_frameCounter >= 300)
        {
            FlushTimings();
            m_frameCounter = 0;
            m_accumulatedCpuMs.fill(0.0);
            m_sampleCounts.fill(0);
            m_accumulatedGpuMs.fill(0.0);
            m_gpuSampleCounts.fill(0);
        }
    }

    void PerformanceProfiler::RecordCpuTime(PerfStage stage, LARGE_INTEGER start, LARGE_INTEGER end)
    {
        if (m_qpcFrequency.QuadPart == 0) return;
        double elapsedMs = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(m_qpcFrequency.QuadPart);

        std::lock_guard lock(m_mutex);
        size_t idx = static_cast<size_t>(stage);
        if (idx < m_accumulatedCpuMs.size())
        {
            m_accumulatedCpuMs[idx] += elapsedMs;
            m_sampleCounts[idx]++;
        }
    }

    void PerformanceProfiler::FlushTimings()
    {
        auto getAvg = [this](size_t idx) -> double {
            if (m_sampleCounts[idx] == 0) return 0.0;
            return m_accumulatedCpuMs[idx] / static_cast<double>(m_sampleCounts[idx]);
        };

        auto getGpuAvg = [this](size_t idx) -> double {
            if (m_gpuSampleCounts[idx] == 0) return 0.0;
            return m_accumulatedGpuMs[idx] / static_cast<double>(m_gpuSampleCounts[idx]);
        };

        std::ostringstream ss;
        if (m_cpuProfilingEnabled)
        {
            ss << "\n[Perf] CPU timings (averaged over 300 frames):\n";
            ss << std::fixed << std::setprecision(3);
            ss << "  ShadowMap CPU setup           = " << getAvg(static_cast<size_t>(PerfStage::ShadowMapBuild)) << " ms\n";
            ss << "  LegacyComposite CPU           = " << getAvg(static_cast<size_t>(PerfStage::LegacyComposite)) << " ms\n";
            ss << "  HeightFog CPU                 = " << getAvg(static_cast<size_t>(PerfStage::HeightFog)) << " ms\n";
            ss << "  DirectionalVolumetricRaymarch = " << getAvg(static_cast<size_t>(PerfStage::DirectionalVolumetricRaymarch)) << " ms\n";
            ss << "  VolumetricTemporal            = " << getAvg(static_cast<size_t>(PerfStage::VolumetricTemporal)) << " ms\n";
            ss << "  VolumetricUpsample            = " << getAvg(static_cast<size_t>(PerfStage::VolumetricUpsample)) << " ms\n";
            ss << "  SunRadialGlare                = " << getAvg(static_cast<size_t>(PerfStage::SunRadialGlare)) << " ms\n";
            ss << "  PostProcess                   = " << getAvg(static_cast<size_t>(PerfStage::PostProcess)) << " ms\n";
            ss << "  TotalInjectedFrame CPU        = " << getAvg(static_cast<size_t>(PerfStage::TotalInjectedFrame)) << " ms\n";
        }

        if (m_gpuProfilingEnabled && m_gpuQueriesSupported)
        {
            ss << "[Perf] GPU timings (averaged over 300 frames, async queries):\n";
            ss << "  ShadowMap GPU                 = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::ShadowMap)) << " ms\n";
            ss << "  DirectionalVolumetric GPU     = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::DirectionalVolumetric)) << " ms\n";
            ss << "  AtmosphereBoundary GPU        = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::AtmosphereBoundary)) << " ms\n";
            ss << "  AtmosphereIntegrate GPU       = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::AtmosphereIntegrate)) << " ms\n";
            ss << "  AtmosphereTemporal GPU        = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::AtmosphereTemporal)) << " ms\n";
            ss << "  AtmosphereUpsample GPU        = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::AtmosphereUpsample)) << " ms\n";
            ss << "  AtmosphereComposite GPU       = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::AtmosphereComposite)) << " ms\n";
            ss << "  VolumetricClouds GPU          = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::VolumetricClouds)) << " ms\n";
            ss << "  LegacyComposite GPU           = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::LegacyComposite)) << " ms\n";
            ss << "  Water GPU                     = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::Water)) << " ms\n";
            ss << "  TotalGpu                      = " << getGpuAvg(static_cast<size_t>(GpuPerfStage::TotalGpu)) << " ms\n";
        }

        std::string outStr = ss.str();
        OutputDebugStringA(outStr.c_str());

        if (!m_logPath.empty())
        {
            try
            {
                std::ofstream out(std::filesystem::path(m_logPath), std::ios::app);
                if (out.is_open())
                {
                    out << outStr;
                }
            }
            catch (...) {}
        }
    }
}
