#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <mutex>

namespace renderer
{
    struct DiagnosticCounters
    {
        uint64_t totalFrames = 0;
        // `totalDrawCalls` lives outside this struct: it is the one number every
        // draw feeds, and it is an atomic so that feeding it costs no lock (see
        // RendererDiagnostics::RecordDrawCall). Everything below is counted for
        // the draws a feature actually had to CLASSIFY - the shadow map's
        // casters, the water and ground surfaces - which is a much smaller set.
        uint64_t classifiedDrawCalls = 0;
        uint64_t unknownDrawCalls = 0;
        uint64_t shadowCasterDraws = 0;
        uint64_t waterDraws = 0;
        uint64_t uiDraws = 0;
        uint64_t cameraCaptures = 0;
        uint64_t depthCaptures = 0;
        uint64_t compositedFrames = 0;
    };

    struct DrawClassification;

    // Per-frame counter deltas, accumulated on the render thread WITHOUT a
    // lock and committed once per frame in OnFrameEnd. The draw hook used to
    // take the mutex on every single classified draw; batching means the
    // render thread only ever touches m_mutex once a frame, while the logged
    // totals and GetCounters() are byte-for-byte identical.
    struct FrameBatch
    {
        uint64_t drawCalls = 0;
        uint64_t classifiedDrawCalls = 0;
        uint64_t unknownDrawCalls = 0;
        uint64_t shadowCasterDraws = 0;
        uint64_t waterDraws = 0;
        uint64_t uiDraws = 0;
        uint64_t cameraCaptures = 0;
        uint64_t depthCaptures = 0;
        uint64_t compositedFrames = 0;
    };

    class RendererDiagnostics
    {
    public:
        static RendererDiagnostics& Instance();

        void SetLogPath(const std::wstring& path);

        void OnFrameEnd();

        // The draws the client has made this session, and the frames it took.
        // Both are read by the stall line, which asks the one question a hitch
        // raises: was the frame that took the gap an ordinary frame, or one that
        // drew the whole place again? (The per-frame number is the difference
        // between two reads of `DrawCalls`, so nothing has to be zeroed per
        // frame - the count itself is the only bookkeeping, and it is fed from
        // the draw hooks.)
        uint64_t DrawCalls() const noexcept { return m_drawCalls.load(std::memory_order_relaxed); }
        uint64_t Frames() const noexcept;

        // Every draw the client makes, counted. This is the `draws=` number in
        // the periodic [Perf] line, and it is deliberately on the cheapest path
        // there is: an atomic increment, no lock, no device read.
        void RecordDrawCall();
        // And the draws a feature had to classify, which is where the rest of
        // the line's numbers come from.
        void RecordDrawCall(bool classified, bool isWater, bool isShadowCaster, bool isUI);
        void RecordDrawCall(const DrawClassification& dc);
        void RecordCameraCapture();
        void RecordDepthCapture();
        void RecordComposite();

        void Log(const std::string& category, const std::string& message);
        void LogOnce(const std::string& category, const std::string& key, const std::string& message);

        const DiagnosticCounters& GetCounters() const { return m_counters; }

    private:
        RendererDiagnostics() = default;

        void FlushAggregatedLog();

        DiagnosticCounters m_counters;
        FrameBatch m_frameBatch;
        std::atomic<uint64_t> m_drawCalls{0};
        std::wstring m_logPath;
        std::unordered_set<std::string> m_loggedOnceKeys;
        mutable std::mutex m_mutex;
    };
}
