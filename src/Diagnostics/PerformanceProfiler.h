#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <array>
#include <mutex>

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    enum class PerfStage : uint32_t
    {
        ShadowMapBuild = 0,
        LegacyComposite,
        HeightFog,
        DirectionalVolumetricRaymarch,
        VolumetricTemporal,
        VolumetricUpsample,
        SunRadialGlare,
        PostProcess,
        TotalInjectedFrame,
        StageCount
    };

    enum class GpuPerfStage : uint32_t
    {
        ShadowMap = 0,
        DirectionalVolumetric,
        AtmosphereBoundary,
        AtmosphereIntegrate,
        AtmosphereTemporal,
        AtmosphereUpsample,
        AtmosphereComposite,
        VolumetricClouds,
        LegacyComposite,
        Water,
        TotalGpu,
        GpuStageCount
    };

    class PerformanceProfiler
    {
    public:
        static PerformanceProfiler& Instance();

        void SetLogPath(const std::wstring& path);
        void SetCpuProfilingEnabled(bool enabled) { m_cpuProfilingEnabled = enabled; }
        bool IsCpuProfilingEnabled() const { return m_cpuProfilingEnabled; }

        void SetGpuProfilingEnabled(bool enabled) { m_gpuProfilingEnabled = enabled; }
        bool IsGpuProfilingEnabled() const { return m_gpuProfilingEnabled; }

        void OnFrameBegin(IDirect3DDevice9* device);
        void OnFrameEnd(IDirect3DDevice9* device);
        void Reset(IDirect3DDevice9* device);

        void RecordCpuTime(PerfStage stage, LARGE_INTEGER start, LARGE_INTEGER end);

        // Async GPU timestamps
        void BeginGpuStage(IDirect3DDevice9* device, GpuPerfStage stage);
        void EndGpuStage(IDirect3DDevice9* device, GpuPerfStage stage);

    private:
        PerformanceProfiler();

        void FlushTimings();
        void EnsureGpuQueries(IDirect3DDevice9* device);
        // Called only while m_mutex is already held (Reset and OnFrameBegin).
        void ResetGpuQueriesUnlocked();

        std::wstring m_logPath;
        bool m_cpuProfilingEnabled = false;
        bool m_gpuProfilingEnabled = false;
        LARGE_INTEGER m_qpcFrequency{};

        std::array<double, static_cast<size_t>(PerfStage::StageCount)> m_accumulatedCpuMs{};
        std::array<uint32_t, static_cast<size_t>(PerfStage::StageCount)> m_sampleCounts{};
        uint32_t m_frameCounter = 0;

        // Async GPU Query Ring Buffer (depth 3)
        static constexpr uint32_t QUERY_RING_DEPTH = 3;
        struct GpuFrameSlot
        {
            ComPtr<IDirect3DQuery9> disjoint;
            ComPtr<IDirect3DQuery9> freq;
            ComPtr<IDirect3DQuery9> start[static_cast<size_t>(GpuPerfStage::GpuStageCount)];
            ComPtr<IDirect3DQuery9> end[static_cast<size_t>(GpuPerfStage::GpuStageCount)];
            bool issued[static_cast<size_t>(GpuPerfStage::GpuStageCount)]{};
            bool frameIssued = false;
        };

        GpuFrameSlot m_gpuRing[QUERY_RING_DEPTH];
        uint32_t m_gpuWriteSlot = 0;
        IDirect3DDevice9* m_gpuDeviceOwner = nullptr;
        bool m_gpuQueriesSupported = false;

        std::array<double, static_cast<size_t>(GpuPerfStage::GpuStageCount)> m_accumulatedGpuMs{};
        std::array<uint32_t, static_cast<size_t>(GpuPerfStage::GpuStageCount)> m_gpuSampleCounts{};

        mutable std::mutex m_mutex;
    };

    class ScopedCpuTimer
    {
    public:
        explicit ScopedCpuTimer(PerfStage stage)
            : m_stage(stage)
        {
            if (PerformanceProfiler::Instance().IsCpuProfilingEnabled())
            {
                QueryPerformanceCounter(&m_start);
            }
        }

        ~ScopedCpuTimer()
        {
            if (m_start.QuadPart != 0)
            {
                LARGE_INTEGER end;
                QueryPerformanceCounter(&end);
                PerformanceProfiler::Instance().RecordCpuTime(m_stage, m_start, end);
            }
        }

    private:
        PerfStage m_stage;
        LARGE_INTEGER m_start{};
    };

    class ScopedGpuTimer
    {
    public:
        ScopedGpuTimer(IDirect3DDevice9* device, GpuPerfStage stage)
            : m_device(device), m_stage(stage)
        {
            if (device && PerformanceProfiler::Instance().IsGpuProfilingEnabled())
                PerformanceProfiler::Instance().BeginGpuStage(device, stage);
        }

        ~ScopedGpuTimer()
        {
            if (m_device && PerformanceProfiler::Instance().IsGpuProfilingEnabled())
                PerformanceProfiler::Instance().EndGpuStage(m_device, m_stage);
        }

    private:
        IDirect3DDevice9* m_device = nullptr;
        GpuPerfStage m_stage;
    };
}
