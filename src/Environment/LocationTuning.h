#pragma once
#include <windows.h>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <cstdint>
#include "LocationContext.h"
namespace renderer::locationtuning {
inline std::wstring baseFile, localFile;
inline LocationContext active;
inline bool editable=false;
inline bool editZone=false;
inline bool editGlobal=false;
inline std::wstring GlobalScope(LPCWSTR section) { return std::wstring(L"Global.")+section; }

// The cache lives in the named namespace (not an anonymous one): the header is
// included by many translation units and every one of them must share the same
// parsed file, so that a save in the menu's translation unit invalidates the
// copy the atmosphere pass reads from. An inline function's static local is
// process-wide, which is exactly the storage this needs.
namespace cache {
struct IniCache {
    std::filesystem::file_time_type mtime{};
    unsigned long long size=0;
    bool loaded=false;
    std::map<std::wstring,std::map<std::wstring,std::wstring>> data;
};
inline std::map<std::wstring,IniCache>& IniCaches() { static std::map<std::wstring,IniCache> c; return c; }
inline std::wstring ToLowerWide(std::wstring s) { for (auto& c : s) if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a'; return s; }
inline std::wstring TrimWide(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
inline void LoadIniCache(const std::wstring& path) {
    auto& c = IniCaches()[path];
    std::error_code ec;
    auto t = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    unsigned long long sz=0;
    if (!ec) sz=static_cast<unsigned long long>(std::filesystem::file_size(std::filesystem::path(path), ec));
    // Coarse-timestamp filesystems (FAT-style two-second mtimes, some network
    // mounts) can serve a rewrite with an unchanged mtime; the size is the
    // second, independent half of the freshness check.
    if (c.loaded && !ec && t == c.mtime && sz == c.size) return;
    c.loaded = true;
    c.mtime = t;
    c.size = sz;
    c.data.clear();
    if (ec) return;
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return;
    std::string bytes;
    {
        char buffer[8192];
        while (file.read(buffer, sizeof buffer) || file.gcount())
            bytes.append(buffer, static_cast<size_t>(file.gcount()));
    }
    std::wstring text;
    // WritePrivateProfileStringW re-encodes a file in UTF-16 once it carries a
    // BOM, so a hand-saved Unicode INI must be read back as UTF-16: without this
    // every key in it would be unfindable and the whole file would read as
    // "missing" after the first edit.
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) {
        const auto* w = reinterpret_cast<const uint16_t*>(bytes.data() + 2);
        const size_t n = (bytes.size() - 2) / 2;
        text.reserve(n);
        for (size_t i = 0; i < n; ++i) text.push_back(static_cast<wchar_t>(w[i]));
    } else if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF) {
        const auto* w = reinterpret_cast<const uint16_t*>(bytes.data() + 2);
        const size_t n = (bytes.size() - 2) / 2;
        text.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            uint16_t u = w[i];
            text.push_back(static_cast<wchar_t>((u >> 8) | (u << 8)));
        }
    } else {
        text.reserve(bytes.size());
        for (unsigned char ch : bytes) text.push_back(static_cast<wchar_t>(ch));
    }
    std::wistringstream in(text);
    std::wstring line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        // GetPrivateProfileStringW skips lines whose first non-space character is
        // ';' or '#'; section lines are "[name]" with optional surrounding spaces.
        // The VALUE of a key line is kept verbatim after the first '=', spaces
        // included, exactly like the API.
        size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;
        if (line[first] == L';' || line[first] == L'#') continue;
        if (line[first] == L'[') {
            size_t last = line.find_last_not_of(L" \t");
            if (last == std::wstring::npos || line[last] != L']') continue;
            section = ToLowerWide(TrimWide(line.substr(first + 1, last - first - 1)));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || section.empty()) continue;
        std::wstring key = ToLowerWide(TrimWide(line.substr(0, eq)));
        if (key.empty()) continue;
        // A duplicated key inside one section keeps its FIRST value, exactly
        // like GetPrivateProfileStringW; the later copy must not shadow it.
        if (c.data[section].find(key) == c.data[section].end())
            c.data[section][key] = line.substr(eq + 1);
    }
}
inline bool TryGetCached(const std::wstring& path, const std::wstring& section, const std::wstring& key, std::wstring& out) {
    LoadIniCache(path);
    auto& c = IniCaches()[path];
    auto sit = c.data.find(ToLowerWide(section));
    if (sit == c.data.end()) return false;
    auto kit = sit->second.find(ToLowerWide(key));
    if (kit == sit->second.end()) return false;
    out = kit->second;
    return true;
}
inline void InvalidateIniCache(const std::wstring& path) {
    auto& c = IniCaches()[path];
    c.loaded = false;
    c.data.clear();
}
} // namespace cache
using cache::IniCaches;
using cache::ToLowerWide;
using cache::TrimWide;
using cache::LoadIniCache;
using cache::TryGetCached;
using cache::InvalidateIniCache;

inline bool HasGlobalValue(LPCWSTR section, LPCWSTR key) {
    if (localFile.empty()) return false;
    std::wstring value;
    if (!TryGetCached(localFile, GlobalScope(section), key, value)) return false;
    return !value.empty();
}
inline bool ClearGlobalValue(LPCWSTR section, LPCWSTR key) {
    bool ok = !localFile.empty() && WritePrivateProfileStringW(GlobalScope(section).c_str(), key, nullptr, localFile.c_str()) != 0;
    if (!localFile.empty()) InvalidateIniCache(localFile);
    return ok;
}
inline bool IsGlobal(LPCWSTR section) { return section && _wcsicmp(section, L"PostProcess") == 0; }
inline std::wstring Scope(unsigned level) {
    std::wstring result = L"Map." + std::to_wstring(active.mapId);
    if (level >= 1) result += L".Zone." + std::to_wstring(active.zoneId);
    if (level >= 2) result += L".Area." + std::to_wstring(active.areaId);
    return result;
}
inline bool Sync(const std::wstring& base, const LocationContext& location, bool verified) {
    const bool changed = !active.SamePlace(location) || editable != (location.valid && verified) || baseFile != base + L"GraphicsEffects.ini";
    baseFile = base + L"GraphicsEffects.ini";
    localFile = base + L"LocationGraphics.ini";
    active = location;
    editable = location.valid && verified;
    return changed;
}
inline DWORD ReadAtLevel(LPCWSTR section, LPCWSTR key, LPCWSTR fallback, LPWSTR output, DWORD size, LPCWSTR path, int topLevel) {
    auto copy = [&](const std::wstring& v) -> DWORD {
        if (size > 0) {
            size_t n = std::min(v.size(), static_cast<size_t>(size - 1));
            for (size_t i = 0; i < n; ++i) output[i] = v[i];
            output[n] = L'\0';
        }
        return static_cast<DWORD>(std::min(v.size(), static_cast<size_t>(size ? size - 1 : 0)));
    };
    if (!IsGlobal(section) && editable && path && baseFile == path) {
        for (int level = topLevel; level >= 0; --level) {
            const auto scope = Scope(level) + L"." + section;
            std::wstring value;
            if (TryGetCached(localFile, scope, key, value) && !value.empty()) {
                return copy(value);
            }
        }
    }
    std::wstring value;
    if (TryGetCached(path ? path : L"", section, key, value)) {
        return copy(value);
    }
    return copy(fallback ? std::wstring(fallback) : std::wstring());
}
inline DWORD ReadString(LPCWSTR section, LPCWSTR key, LPCWSTR fallback, LPWSTR output, DWORD size, LPCWSTR path) {
    if (!IsGlobal(section) && path && baseFile == path && HasGlobalValue(section, key)) {
        std::wstring value;
        if (TryGetCached(localFile, GlobalScope(section), key, value) && !value.empty()) {
            if (size > 0) {
                size_t n = std::min(value.size(), static_cast<size_t>(size - 1));
                for (size_t i = 0; i < n; ++i) output[i] = value[i];
                output[n] = L'\0';
            }
            return static_cast<DWORD>(std::min(value.size(), static_cast<size_t>(size ? size - 1 : 0)));
        }
        std::wstring fb = fallback ? std::wstring(fallback) : std::wstring();
        if (size > 0) {
            size_t n = std::min(fb.size(), static_cast<size_t>(size - 1));
            for (size_t i = 0; i < n; ++i) output[i] = fb[i];
            output[n] = L'\0';
        }
        return static_cast<DWORD>(std::min(fb.size(), static_cast<size_t>(size ? size - 1 : 0)));
    }
    return ReadAtLevel(section, key, fallback, output, size, path, active.areaId ? 2 : 1);
}
inline UINT ReadEditorInt(LPCWSTR section, LPCWSTR key, INT fallback, LPCWSTR path) {
    std::wstring value;
    if ((editGlobal || !editable) && !IsGlobal(section) && path && baseFile == path && HasGlobalValue(section, key))
        TryGetCached(localFile, GlobalScope(section), key, value);
    else if (editGlobal) TryGetCached(path ? path : L"", section, key, value);
    else {
        wchar_t buf[128]{}; ReadAtLevel(section, key, L"", buf, 128, path, editZone || !active.areaId ? 1 : 2);
        value = buf;
    }
    return value[0] ? static_cast<UINT>(wcstol(value.c_str(), nullptr, 10)) : static_cast<UINT>(fallback);
}
inline UINT ReadInt(LPCWSTR section, LPCWSTR key, INT fallback, LPCWSTR path) {
    wchar_t value[128]{}; ReadString(section, key, L"", value, 128, path);
    return value[0] ? static_cast<UINT>(wcstol(value, nullptr, 10)) : static_cast<UINT>(fallback);
}
inline bool HasScopedValue(LPCWSTR section, LPCWSTR key) {
    if (HasGlobalValue(section, key)) return true;
    if (!editable || localFile.empty()) return false;
    for (int level = active.areaId ? 2 : 1; level >= 0; --level) {
        const auto scope = Scope(level) + L"." + section;
        std::wstring value;
        if (TryGetCached(localFile, scope, key, value) && !value.empty()) return true;
    }
    return false;
}
inline bool Save(LPCWSTR section, LPCWSTR key, int value) {
    bool ok = false;
    if (IsGlobal(section)) ok = !baseFile.empty() && WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), baseFile.c_str()) != 0;
    else if (editGlobal || (!editable && HasGlobalValue(section, key))) ok = !localFile.empty() && WritePrivateProfileStringW(GlobalScope(section).c_str(), key, std::to_wstring(value).c_str(), localFile.c_str()) != 0;
    else if (!editable) ok = !baseFile.empty() && WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), baseFile.c_str()) != 0;
    else {
        const auto scope = Scope(editZone || !active.areaId ? 1 : 2) + L"." + section;
        ok = WritePrivateProfileStringW(scope.c_str(), key, std::to_wstring(value).c_str(), localFile.c_str()) != 0;
    }
    if (!baseFile.empty()) InvalidateIniCache(baseFile);
    if (!localFile.empty()) InvalidateIniCache(localFile);
    return ok;
}
// Cached helpers for callers that still use the raw Win32 profile API on other files.
inline bool ReadCachedString(LPCWSTR path, LPCWSTR section, LPCWSTR key, std::wstring& out) {
    return TryGetCached(path ? path : L"", section, key, out);
}
inline UINT ReadCachedInt(LPCWSTR path, LPCWSTR section, LPCWSTR key, INT fallback) {
    std::wstring v;
    if (TryGetCached(path ? path : L"", section, key, v) && !v.empty()) {
        return static_cast<UINT>(wcstol(v.c_str(), nullptr, 10));
    }
    return static_cast<UINT>(fallback);
}
}
