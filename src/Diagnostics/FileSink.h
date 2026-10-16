#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// One open per diagnostic file per frame, instead of one per line.
//
// Every diagnostic this renderer writes used to be its own
// `std::ofstream(path, app)` - an open, a write and a close of a file in the
// GAME FOLDER, on the thread that draws. On its own that is cheap. In the game
// folder it is not: that is the folder a real-time antivirus scanner watches, so
// each open is a scan, and the players who found the crossing stall found it by
// deleting a big log - which is a scanner that had stopped having anything to
// re-read.
//
// And a crossing does not write one line. Walking a border confirms a new place
// every few hundred milliseconds, and each confirmation used to write the whole
// menu report on the spot - a dozen lines, one open - whether or not anything
// downstream re-applied. Most of those confirmations are the client's idea of the
// player's place flapping across a border, and the settle logic deliberately
// applies nothing on most of them, so the I/O landed on exactly the frames that
// were doing no work to show for it.
//
// So lines are buffered here and written by one open per file per flush. Present
// flushes, as does a device-availability transition, so a frame with nothing to
// say still touches no file at all and a burst still lands within the frame that
// produced it. A file is never held open between flushes: a player can move or
// delete it while the game runs, which is how these logs get read.
//
// Rotation is checked at most once a second per file rather than once per write.
// The stat is microseconds, but it was per line on the same path, and a growing
// file in the game folder is the thing the scanner keeps re-scanning - so the
// limit still applies, it is just not re-measured on every append. Against a
// two-megabyte cap, being up to a second late about turning the file over is
// nothing.
//
// A reader outside the frame loop has to ask for the flush itself. Present is
// what makes one, so anything that drives a subsystem directly - a regression
// that checks what a supporter would be sent, say - sees an unwritten file
// unless it flushes first. That is a contract, not a caveat: a buffered file is
// complete on disk at the end of every frame and at no point in the middle of
// one. menudiagnostics::Flush() is that ask, named for the file it completes.
//
// THE OWNERSHIP RULE, which is what decides whether a writer may be moved here:
// the sink may buffer a file only if it is that file's SOLE writer. If anything
// else appends to the same path directly, its line can land before an older line
// the sink is still holding - the flush happens at the end of the frame, so the
// window is one frame - and the log then reads out of order, which is the one
// thing a supporter cannot recover from after the fact.
//
// That is why the water, edge-fog and post-process logs are deliberately NOT
// here. Each is also appended to directly by its own module - and the post-process
// log by the frame profiler, every frame - and each of those writers is a bare
// `if (cond) ofstream(...)` whose condition would have to grow braces to take a
// flush first. Each also writes only when its own tuning actually changed, so
// what buffering would save there is one open on a crossing that is already
// re-applying nine subsystems. The menu report and the material tuning log have a
// single writer each, and those are here.
namespace renderer::filesink {

// A flush past this many buffered bytes for one file goes out on the spot, so a
// crash can never take more than one frame's worth of lines with it.
inline constexpr size_t kBurstBytes = 8192;
// The default a log is rotated past. MenuDiagnostics sets its own.
inline constexpr size_t kDefaultRotationBytes = 2 * 1024 * 1024;
// How often one file's size is re-measured against its rotation limit.
inline constexpr uint64_t kRotationCheckMs = 1000;

inline std::mutex& SinkMutex() { static std::mutex mutex; return mutex; }
inline std::unordered_map<std::wstring, std::string>& Pending() {
    static std::unordered_map<std::wstring, std::string> pending;
    return pending;
}
inline std::unordered_map<std::wstring, size_t>& RotationLimits() {
    static std::unordered_map<std::wstring, size_t> limits;
    return limits;
}
inline std::unordered_map<std::wstring, ULONGLONG>& RotationChecks() {
    static std::unordered_map<std::wstring, ULONGLONG> checks;
    return checks;
}
// What a rotated file is called. Most logs rotate to <name>.1; MenuDiagnostics.log
// rotates to <name>.previous, which is what the README tells a supporter to look
// for, so the suffix belongs to the file and not to the sink.
inline std::unordered_map<std::wstring, std::wstring>& RotationSuffixes() {
    static std::unordered_map<std::wstring, std::wstring> suffixes;
    return suffixes;
}

inline void SetRotationLimit(const std::wstring& path, size_t bytes) {
    if (path.empty() || !bytes) return;
    std::lock_guard lock(SinkMutex());
    RotationLimits()[path] = bytes;
}

inline void SetRotationSuffix(const std::wstring& path, const std::wstring& suffix) {
    if (path.empty() || suffix.empty()) return;
    std::lock_guard lock(SinkMutex());
    RotationSuffixes()[path] = suffix;
}

inline size_t RotationLimitOf(const std::wstring& path) {
    const auto found = RotationLimits().find(path);
    return found == RotationLimits().end() ? kDefaultRotationBytes : found->second;
}

inline std::wstring RotationSuffixOf(const std::wstring& path) {
    const auto found = RotationSuffixes().find(path);
    return found == RotationSuffixes().end() ? std::wstring(L".1") : found->second;
}

// Past its limit, the file is rotated to <name>.1: the oldest content is
// discarded and the recent content - the part anyone reading the log cares
// about - survives, so the folder never holds a file the scanner loses patience
// with. Callers hold the mutex.
inline void RotateIfLargeLocked(const std::wstring& path) {
    const ULONGLONG now = GetTickCount64();
    auto& last = RotationChecks()[path];
    if (last && now - last < kRotationCheckMs) return;
    last = now;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) == 0) return;
    if (static_cast<size_t>(info.nFileSizeLow) < RotationLimitOf(path)) return;
    const std::wstring rotated = path + RotationSuffixOf(path);
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(rotated), ec);
    std::filesystem::rename(std::filesystem::path(path), std::filesystem::path(rotated), ec);
}

// One open for whatever this file has waiting. Callers hold the mutex.
inline void FlushOneLocked(const std::wstring& path, std::string& text) {
    if (text.empty()) return;
    try {
        RotateIfLargeLocked(path);
        std::ofstream out(std::filesystem::path(path), std::ios::app);
        if (out) { out << text; out.flush(); }
    } catch (...) {}
    text.clear();
}

// Every file with something waiting, one open each.
inline void Flush() noexcept {
    try {
        std::lock_guard lock(SinkMutex());
        for (auto& entry : Pending()) FlushOneLocked(entry.first, entry.second);
    } catch (...) {}
}

// One file on the spot: what a device-availability transition does, so whatever
// is pending belongs to the state the transition is about to change.
inline void FlushFile(const std::wstring& path) noexcept {
    if (path.empty()) return;
    try {
        std::lock_guard lock(SinkMutex());
        const auto found = Pending().find(path);
        if (found != Pending().end()) FlushOneLocked(found->first, found->second);
    } catch (...) {}
}

// Buffer `text` for `path` verbatim - callers keep their own line endings, so a
// converted site writes exactly the bytes it wrote before.
inline void Append(const std::wstring& path, const std::string& text) {
    if (path.empty() || text.empty()) return;
    try {
        std::lock_guard lock(SinkMutex());
        std::string& pending = Pending()[path];
        pending += text;
        if (pending.size() >= kBurstBytes) FlushOneLocked(path, pending);
    } catch (...) {}
}

// The same, with the newline added: the common shape of a diagnostic line.
inline void AppendLine(const std::wstring& path, const std::string& text) {
    if (path.empty() || text.empty()) return;
    try {
        std::lock_guard lock(SinkMutex());
        std::string& pending = Pending()[path];
        pending += text;
        pending += '\n';
        if (pending.size() >= kBurstBytes) FlushOneLocked(path, pending);
    } catch (...) {}
}

// What the sink is holding, for a report that has to say whether the lines a
// player is missing are unwritten or merely unflushed.
inline size_t PendingBytes() {
    std::lock_guard lock(SinkMutex());
    size_t total = 0;
    for (const auto& entry : Pending()) total += entry.second.size();
    return total;
}
}
