// Minimal windows.h shim so the pure-logic headers under test can be compiled
// on non-Windows CI machines (g++). On Windows this is bypassed entirely: the
// tools/*.cmd runners do not add this directory to the include path.
#if !defined(_WIN32)
#pragma once
#include <cwchar>
#include <cstring>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <chrono>
#include <ctime>
#include <cstdio>

typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef int INT;
typedef int BOOL;
typedef const wchar_t* LPCWSTR;
typedef wchar_t* LPWSTR;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef FALSE
#define FALSE 0
#endif

inline int _wcsicmp(const wchar_t* a, const wchar_t* b)
{
    for (; *a && *b; ++a, ++b) {
        wchar_t x = *a, y = *b;
        if (x >= L'A' && x <= L'Z') x += L'a' - L'A';
        if (y >= L'A' && y <= L'Z') y += L'a' - L'A';
        if (x != y) return x < y ? -1 : 1;
    }
    if (*a == *b) return 0;
    return *a ? 1 : -1;
}

// Time APIs the diagnostic headers touch (MenuDiagnostics timestamps,
// weather ticks). Real wall-clock UTC for the SYSTEMTIME variant.
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
inline void GetSystemTime(SYSTEMTIME* t) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tsec = system_clock::to_time_t(now);
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &tsec);
#else
    gmtime_r(&tsec, &gm);
#endif
    t->wYear = static_cast<WORD>(gm.tm_year + 1900);
    t->wMonth = static_cast<WORD>(gm.tm_mon + 1);
    t->wDayOfWeek = static_cast<WORD>(gm.tm_wday);
    t->wDay = static_cast<WORD>(gm.tm_mday);
    t->wHour = static_cast<WORD>(gm.tm_hour);
    t->wMinute = static_cast<WORD>(gm.tm_min);
    t->wSecond = static_cast<WORD>(gm.tm_sec);
    t->wMilliseconds = static_cast<WORD>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
}
typedef unsigned long long DWORD64;
typedef DWORD64 ULONGLONG;
typedef long long LONGLONG;
typedef void* HWND;
struct LARGE_INTEGER { long long QuadPart; };
inline LARGE_INTEGER QueryPerformanceCounter() {
    return LARGE_INTEGER{std::chrono::steady_clock::now().time_since_epoch().count()};
}
inline LARGE_INTEGER QueryPerformanceFrequency() { return LARGE_INTEGER{1000000000LL}; }
#ifndef VK_F9
#define VK_F9 0x78
#endif
inline short GetAsyncKeyState(int) { return 0; }
// Milliseconds, like the real API: the freshness windows in the product
// (LocationTuning's 10 ms identity throttle) are written against this unit, so a
// shim that ran the clock 100x fast would silently shrink every one of them.
inline DWORD64 GetTickCount64() {
    return static_cast<DWORD64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
inline DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }
inline DWORD GetCurrentProcessId() { return 4242; }
inline DWORD GetWindowThreadProcessId(void*, DWORD* out) { if (out) *out = 4242; return 1; }
inline void* GetForegroundWindow() { return reinterpret_cast<void*>(0x1); }
inline BOOL IsIconic(void*) { return FALSE; }
inline BOOL IsWindow(void*) { return TRUE; }

namespace shiminix {
inline std::wstring lower(std::wstring s) { for (auto& c : s) if (c >= L'A' && c <= L'Z') c += L'a' - L'A'; return s; }
// The edges of a value as the real profile API hands them back. Worked out by
// reading values back on Windows rather than guessed: leading and trailing blanks
// (space, tab, vertical tab) in the file are stripped; if what is left is enclosed
// in one matching pair of single or double quotes that pair is discarded and the
// blanks inside it are kept; an unmatched pair, or a pair in the wrong order, is
// left alone. The renderer's own cached parse implements the same rule, and the
// crossing suite compares the two character by character, so this has to be the
// API's rule and not a convenient one.
inline std::wstring ProfileValue(const std::wstring& text, size_t begin, size_t end) {
    const auto blank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\v'; };
    while (begin < end && blank(text[begin])) ++begin;
    while (end > begin && blank(text[end - 1])) --end;
    if (end - begin >= 2) {
        const wchar_t quote = text[begin];
        if ((quote == L'"' || quote == L'\'') && text[end - 1] == quote) { ++begin; --end; }
    }
    return text.substr(begin, end - begin);
}

inline std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
// Call counters, so a harness can report what a crossing actually paid for at the
// API boundary rather than only how long it took: the whole point of the product's
// cache is that these numbers stay flat while the settings grow.
struct ApiStats {
    unsigned long long fileOpens = 0;   // whole-file parses
    unsigned long long stats = 0;       // GetFileAttributesExW
    unsigned long long nameLists = 0;   // GetPrivateProfileSectionNamesW
    unsigned long long writes = 0;      // WritePrivateProfileStringW
    void Reset() { *this = ApiStats{}; }
};
inline ApiStats& Stats() { static ApiStats s; return s; }
}

// Functional stand-in for WritePrivateProfileStringW: upserts or deletes one
// key in one section and rewrites the file, preserving every other line.
//
// It writes `key=value` with no padding, because that is what the real API
// writes. Getting this wrong is not cosmetic: a test that looks for a line it
// expects the API to have written will pass here and fail on a Windows runner,
// which is the one comparison this fixture exists to make possible.
inline UINT WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR fileName)
{
    if (!section || !key || !fileName) return 0;
    ++shiminix::Stats().writes;
    std::ifstream in(std::filesystem::path(fileName), std::ios::binary);
    std::string bytes;
    {
        char buffer[8192];
        while (in.read(buffer, sizeof buffer) || in.gcount())
            bytes.append(buffer, static_cast<size_t>(in.gcount()));
    }
    std::wstring text;
    for (unsigned char ch : bytes) text.push_back(static_cast<wchar_t>(ch));
    std::wistringstream in2(text);
    std::wstring line;
    bool found = false, hadSection = false;
    std::vector<std::wstring> out;
    std::wstring currentSection;
    while (std::getline(in2, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t first = line.find_first_not_of(L" \t");
        if (first != std::wstring::npos && line[first] == L'[') {
            size_t last = line.find_last_not_of(L" \t");
            if (last != std::wstring::npos && line[last] == L']') {
                currentSection = shiminix::trim(line.substr(first + 1, last - first - 1));
                out.push_back(line);
                hadSection = (_wcsicmp(currentSection.c_str(), section) == 0);
                continue;
            }
        }
        size_t eq = line.find(L'=');
        if (first != std::wstring::npos && eq != std::wstring::npos && !currentSection.empty()) {
            std::wstring k = shiminix::trim(line.substr(first, eq - first));
            if (hadSection && _wcsicmp(k.c_str(), key) == 0) {
                if (value) out.push_back(std::wstring(key) + L"=" + std::wstring(value));
                found = true;
                continue; // drop the old line when deleting
            }
        }
        out.push_back(line);
    }
    if (!found && value) {
        if (!hadSection) out.push_back(std::wstring(L"[") + section + L"]");
        out.push_back(std::wstring(key) + L"=" + std::wstring(value));
    }
    // Write narrow bytes on purpose: Linux wide file streams are unreliable
    // under a POSIX locale, and the test content is ASCII anyway.
    std::ofstream of(std::filesystem::path(fileName), std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < out.size(); ++i) {
        for (wchar_t c : out[i]) of.put(static_cast<char>(c > 127 ? '?' : c));
        if (i + 1 < out.size()) of.put('\n');
    }
    return 1;
}
// Functional stand-in for the INI read side, matching the writer above.
namespace shiminix {
inline bool IniLookup(const std::wstring& fileName, const std::wstring& section,
                      const std::wstring& key, std::wstring& out)
{
    std::ifstream in(std::filesystem::path(fileName), std::ios::binary);
    if (!in) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring text;
    for (unsigned char ch : bytes) text.push_back(static_cast<wchar_t>(ch));
    std::wistringstream lines(text);
    std::wstring line, current;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;
        if (line[first] == L'[') {
            size_t last = line.find_last_not_of(L" \t");
            if (last > first && line[last] == L']')
                current = trim(line.substr(first + 1, last - first - 1));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || current.empty()) continue;
        std::wstring k = trim(line.substr(first, eq - first));
        if (_wcsicmp(k.c_str(), key.c_str()) == 0 &&
            _wcsicmp(current.c_str(), section.c_str()) == 0) {
            out = trim(line.substr(eq + 1));
            return true;
        }
    }
    return false;
}
}

inline DWORD GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, int defaultValue,
                                   LPCWSTR fileName)
{
    if (!section || !key || !fileName) return static_cast<DWORD>(defaultValue);
    std::wstring value;
    if (!shiminix::IniLookup(fileName, section, key, value))
        return static_cast<DWORD>(defaultValue);
    try { return static_cast<DWORD>(std::stoi(value)); }
    catch (...) { return static_cast<DWORD>(defaultValue); }
}

// ---------------------------------------------------------------------------
// The profile (INI) API, faithfully enough to run src/Environment/LocationTuning.h
// - the per-zone/per-area settings cache - and to MEASURE it on Linux.
//
// These are deliberately SLOW: each call re-opens and re-parses the whole file,
// which is exactly what the real Win32 calls do and exactly the cost the product's
// section cache exists to avoid. A harness that answered from memory would measure
// its own cache instead of the one under test, so nothing here memoises anything.
//
// Semantics mirrored (they are the ones LocationTuning.h documents and the ones
// tools/LocationTuningCacheRegression.cpp asserts against on Windows):
//   * sections and keys match case-insensitively and are trimmed;
//   * a VALUE is returned exactly as written, spaces included - the contract
//     src/Environment/LocationStatus.h relies on for byte-exact SHA/RVA compares;
//   * the first occurrence of a duplicate key wins;
//   * ';' and '#' start a comment line;
//   * a missing file, section or key yields the caller's default;
//   * GetPrivateProfileSectionW returns "key=value" entries, double-null
//     terminated, and the character count copied (size-2 when truncated) - NOT a
//     bare key list, which is what the product's parser walks.
namespace shiminix {
struct IniLine { std::wstring key, value; };
// One whole-file parse into (section -> lines), in file order.
inline std::map<std::wstring, std::vector<IniLine>>& ParseWholeFile(const std::wstring& fileName, bool& ok)
{
    static std::map<std::wstring, std::vector<IniLine>> scratch;
    scratch.clear();
    ok = false;
    ++Stats().fileOpens;
    std::ifstream in(std::filesystem::path(fileName), std::ios::binary);
    if (!in) return scratch;
    ok = true;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring text;
    // WritePrivateProfileStringW writes a Unicode file once one has a BOM, and the
    // real read side honours it - so the shim has to as well, or a Unicode
    // settings file would look empty to the reference and not to the product.
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 2 && byte(0) == 0xFF && byte(1) == 0xFE) {
        text.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>(byte(i) | (static_cast<unsigned short>(byte(i + 1)) << 8)));
    } else {
        size_t start = 0;
        if (bytes.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) start = 3;
        text.reserve(bytes.size() - start);
        for (size_t i = start; i < bytes.size(); ++i) text.push_back(static_cast<wchar_t>(byte(i)));
    }
    std::wistringstream lines(text);
    std::wstring line, current;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;
        if (line[first] == L';' || line[first] == L'#') continue;
        if (line[first] == L'[') {
            size_t last = line.find_last_not_of(L" \t");
            if (last == std::wstring::npos || line[last] != L']') continue;
            current = lower(trim(line.substr(first + 1, last - first - 1)));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || current.empty()) continue;
        scratch[current].push_back({trim(line.substr(first, eq - first)), ProfileValue(line, eq + 1, line.size())});
    }
    return scratch;
}
}

inline DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR fallback,
                                      LPWSTR output, DWORD size, LPCWSTR fileName)
{
    const std::wstring def = fallback ? fallback : L"";
    if (!section || !key || !output || size == 0 || !fileName) {
        if (output && size) output[0] = L'\0';
        return 0;
    }
    bool ok = false;
    const auto& parsed = shiminix::ParseWholeFile(fileName, ok);
    std::wstring result = def;
    if (ok) {
        const auto found = parsed.find(shiminix::lower(section));
        if (found != parsed.end()) {
            for (const auto& entry : found->second) {
                if (_wcsicmp(entry.key.c_str(), key) != 0) continue;
                result = entry.value; // first occurrence, verbatim - exactly like the API
                break;
            }
        }
    }
    const size_t length = result.size() < size - 1 ? result.size() : size - 1;
    if (length) std::wmemcpy(output, result.c_str(), length);
    output[length] = L'\0';
    return static_cast<DWORD>(length);
}

inline UINT GetPrivateProfileSectionW(LPCWSTR section, LPWSTR buffer, DWORD size, LPCWSTR fileName)
{
    if (!section || !buffer || size == 0 || !fileName) return 0;
    bool ok = false;
    const auto& parsed = shiminix::ParseWholeFile(fileName, ok);
    buffer[0] = L'\0';
    if (!ok) return 0;
    const auto found = parsed.find(shiminix::lower(section));
    if (found == parsed.end()) return 0;
    DWORD used = 0;
    for (const auto& entry : found->second) {
        const std::wstring text = entry.key + L"=" + entry.value;
        // The real API stops filling rather than split an entry, and reports
        // size-2 so a caller can tell "full" from "finished".
        if (used + text.size() + 2 > size) { buffer[used] = L'\0'; return size - 2; }
        for (wchar_t c : text) buffer[used++] = c;
        buffer[used++] = L'\0';
    }
    buffer[used] = L'\0';
    return used;
}

inline DWORD GetPrivateProfileSectionNamesW(LPWSTR buffer, DWORD size, LPCWSTR fileName)
{
    if (!buffer || size == 0 || !fileName) return 0;
    ++shiminix::Stats().nameLists;
    bool ok = false;
    const auto& parsed = shiminix::ParseWholeFile(fileName, ok);
    buffer[0] = L'\0';
    if (!ok) return 0;
    DWORD used = 0;
    for (const auto& entry : parsed) {
        if (used + entry.first.size() + 2 > size) { buffer[used] = L'\0'; return size - 2; }
        for (wchar_t c : entry.first) buffer[used++] = c;
        buffer[used++] = L'\0';
    }
    buffer[used] = L'\0';
    return used;
}

// ---------------------------------------------------------------------------
// File identity. LocationTuning's cache revalidates on (last write time, size),
// so the shim has to report both with real resolution - and a write has to move
// the timestamp, or every freshness test would pass for the wrong reason.
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard = 0 };
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
};
inline BOOL GetFileAttributesExW(LPCWSTR fileName, GET_FILEEX_INFO_LEVELS, WIN32_FILE_ATTRIBUTE_DATA* out)
{
    if (!fileName || !out) return FALSE;
    ++shiminix::Stats().stats;
    std::error_code ec;
    const std::filesystem::path path(fileName);
    if (!std::filesystem::exists(path, ec)) return FALSE;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) return FALSE;
    // 100 ns ticks since 1601, the FILETIME unit, from the file clock's own epoch.
    const auto ticks = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
        written.time_since_epoch()).count();
    out->dwFileAttributes = 0x80; // FILE_ATTRIBUTE_NORMAL
    out->ftCreationTime = {0, 0};
    out->ftLastAccessTime = {0, 0};
    out->ftLastWriteTime = {static_cast<DWORD>(ticks & 0xffffffff), static_cast<DWORD>(ticks >> 32)};
    const auto bytes = std::filesystem::file_size(path, ec);
    out->nFileSizeHigh = 0;
    out->nFileSizeLow = ec ? 0 : static_cast<DWORD>(bytes);
    return TRUE;
}

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
inline int WideCharToMultiByte(UINT, DWORD, const wchar_t* src, int count, char* out, int outSize, void*, void*)
{
    if (!src) return 0;
    const int n = count < 0 ? int(std::wcslen(src)) : count;
    if (!out || outSize <= 0) return n;
    const int copy = n < outSize ? n : outSize;
    for (int i = 0; i < copy; ++i) out[i] = static_cast<char>(src[i] > 0x7f ? '?' : src[i]);
    return copy;
}

#endif
