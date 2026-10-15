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
inline DWORD64 GetTickCount64() {
    return static_cast<DWORD64>(std::chrono::steady_clock::now().time_since_epoch().count() / 10000);
}
inline DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }
inline DWORD GetCurrentProcessId() { return 4242; }
inline DWORD GetWindowThreadProcessId(void*, DWORD* out) { if (out) *out = 4242; return 1; }
inline void* GetForegroundWindow() { return reinterpret_cast<void*>(0x1); }
inline BOOL IsIconic(void*) { return FALSE; }
inline BOOL IsWindow(void*) { return TRUE; }

namespace shiminix {
inline std::wstring lower(std::wstring s) { for (auto& c : s) if (c >= L'A' && c <= L'Z') c += L'a' - L'A'; return s; }
inline std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
}

// Functional stand-in for WritePrivateProfileStringW: upserts or deletes one
// key in one section and rewrites the file, preserving every other line.
inline UINT WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR fileName)
{
    if (!section || !key || !fileName) return 0;
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
                if (value) out.push_back(std::wstring(key) + L" = " + std::wstring(value));
                found = true;
                continue; // drop the old line when deleting
            }
        }
        out.push_back(line);
    }
    if (!found && value) {
        if (!hadSection) out.push_back(std::wstring(L"[") + section + L"]");
        out.push_back(std::wstring(key) + L" = " + std::wstring(value));
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
#endif
