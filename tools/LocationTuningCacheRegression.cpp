// Verifies the in-memory INI cache behind renderer::locationtuning against a
// reference that re-parses the files on every call (mirroring the documented
// GetPrivateProfileStringW behaviour the old code path relied on). Also
// measures the cost of a full zone-change settings reload, which used to
// re-read both INI files for every single key and is what caused the
// multi-second hitch when crossing a zone border.
#include "../src/Environment/LocationTuning.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

extern "C" {
unsigned LocationTuningOtherTuRead(LPCWSTR path, LPCWSTR key, INT fallback);
int LocationTuningOtherTuSave(LPCWSTR section, LPCWSTR key, int value);
}

static int failures = 0;

#define Require(cond, message) \
    do { \
        if (cond) { \
            std::printf("PASS %s\n", message); \
        } else { \
            std::printf("FAIL %s\n", message); \
            ++failures; \
        } \
    } while (0)

namespace reference {
// Faithful mini-emulation of GetPrivateProfileStringW for the subset the
// renderer uses: case-insensitive sections/keys, trimmed section and key,
// the API's own treatment of a value's edges (see ProfileValue), ';'/'#'
// comment lines, defaults for missing keys, ANSI bytes decoded one-to-one.

// The edges of a value as GetPrivateProfileStringW hands them back. Leading and
// trailing blanks (space, tab, vertical tab) that exist in the file are stripped;
// if what is left is enclosed in one matching pair of single or double quotes that
// pair is discarded and the blanks inside it are kept; an unmatched pair, or a pair
// in the wrong order, is left alone. This is the documented-and-measured behaviour
// of the API, not a convenience: the cached parse in LocationTuning.h implements
// the same rule and the differential below compares the two character by character.
std::wstring ProfileValue(const std::wstring& text, size_t begin, size_t end)
{
    const auto blank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\v'; };
    while (begin < end && blank(text[begin])) ++begin;
    while (end > begin && blank(text[end - 1])) --end;
    if (end - begin >= 2) {
        const wchar_t quote = text[begin];
        if ((quote == L'"' || quote == L'\'') && text[end - 1] == quote) { ++begin; --end; }
    }
    return text.substr(begin, end - begin);
}
std::wstring GetString(const fs::path& path, const std::wstring& section, const std::wstring& key, const std::wstring& fallback)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return fallback;
    std::string bytes;
    {
        char buffer[8192];
        while (file.read(buffer, sizeof buffer) || file.gcount())
            bytes.append(buffer, static_cast<size_t>(file.gcount()));
    }
    std::wstring text;
    for (unsigned char ch : bytes) text.push_back(static_cast<wchar_t>(ch));
    auto lower = [](std::wstring s) { for (auto& c : s) if (c >= L'A' && c <= L'Z') c += L'a' - L'A'; return s; };
    auto trim = [](const std::wstring& s) {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return std::wstring();
        size_t b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    std::wistringstream in(text);
    std::wstring line, current;
    const std::wstring wantSection = lower(trim(section));
    const std::wstring wantKey = lower(trim(key));
    while (std::getline(in, line)) {
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
        if (current != wantSection) continue;
        if (lower(trim(line.substr(0, eq))) != wantKey) continue;
        // First occurrence wins, and the value's edges go the way the API does.
        return ProfileValue(line, eq + 1, line.size());
    }
    return fallback;
}

int GetInt(const fs::path& path, const std::wstring& section, const std::wstring& key, int fallback)
{
    const std::wstring v = GetString(path, section, key, L"");
    if (v.empty()) return fallback;
    return static_cast<int>(wcstol(v.c_str(), nullptr, 10));
}
}

static std::string ToUtf8(const std::wstring& in)
{
    std::string out;
    out.reserve(in.size());
    for (wchar_t c : in) out += static_cast<char>(c > 127 ? '?' : c);
    return out;
}

static fs::path MakeSandbox()
{
    const fs::path dir = fs::temp_directory_path() / L"locationtuning_cache_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static void WriteFile(const fs::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

int main()
{
    using renderer::locationtuning::ReadInt;
    using renderer::locationtuning::ReadString;
    using renderer::locationtuning::HasGlobalValue;
    using renderer::locationtuning::HasScopedValue;
    using renderer::locationtuning::ReadEditorInt;
    using renderer::locationtuning::Save;
    using renderer::locationtuning::Sync;

    const fs::path dir = MakeSandbox();
    // The real g_basePath always ends in a directory separator (it comes from
    // GetModuleFileName), and locationtuning builds file names by plain
    // concatenation - keep that contract in the test.
    const std::wstring base = (dir / L"").wstring();
    const fs::path graphics = dir / L"GraphicsEffects.ini";
    const fs::path location = dir / L"LocationGraphics.ini";
    const std::wstring graphicsW = graphics.wstring();
    const std::wstring locationW = location.wstring();

    WriteFile(graphics,
        "; global tuning\r\n"
        "[Water]\r\n"
        "Enabled = 1\r\n"
        "WaterColour=0x808080\r\n"
        "RipplePermille=18\r\n"
        "ShowStatus=0\r\n"
        "[Atmosphere]\r\n"
        "ShaftPercent=60\r\n"
        "[Volume]\r\n"
        "Enabled=0\r\n"
        "[Notes]\r\n"
        "SpacedNote = hello  world \r\n"
        "QuotedNote=\"  kept  \"\r\n"
        "TabbedNote=\thello\t\r\n");
    WriteFile(location,
        "[Map.571.Zone.65.Area.4164.Water]\r\n"
        "RipplePermille = 42\r\n"
        "[Map.571.Zone.65.Area.4164.Atmosphere]\r\n"
        "ShaftPercent=70\r\n"
        "[Map.571.Zone.65.Water]\r\n"
        "WaterColour=0xAABBCC\r\n"
        "[Map.571.Water]\r\n"
        "RipplePermille=30\r\n"
        "[Global.Water]\r\n"
        "FoamEnabled=0\r\n");

    // ---- unverified client: everything must fall back to the global file ----
    Sync(base, renderer::LocationContext{}, false);
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 18, "unverified reads global file");
    Require(HasGlobalValue(L"Water", L"FoamEnabled"), "global mask visible");
    Require(!HasScopedValue(L"Water", L"RipplePermille"), "no scoped values without verified location");
    {
        wchar_t value[64]{};
        ReadString(L"Water", L"WaterColour", L"", value, 64, graphicsW.c_str());
        Require(std::wstring(value) == L"0x808080", "string read keeps hex verbatim");
        // The blanks a value was padded with are not part of it - the API strips
        // them, so a hand-aligned settings file reads the same as one that was not.
        // The blanks INSIDE the value, and inside a quoted one, are part of it.
        ReadString(L"Notes", L"SpacedNote", L"", value, 64, graphicsW.c_str());
        Require(std::wstring(value) == L"hello  world", "string read strips a value's surrounding blanks like the API");
        ReadString(L"Notes", L"QuotedNote", L"", value, 64, graphicsW.c_str());
        Require(std::wstring(value) == L"  kept  ", "string read keeps the blanks inside quotes");
        ReadString(L"Notes", L"TabbedNote", L"", value, 64, graphicsW.c_str());
        Require(std::wstring(value) == L"hello", "string read strips surrounding tabs like the API");
    }
    Require(ReadInt(L"Water", L"Missing", 7, graphicsW.c_str()) == 7, "missing key returns fallback");

    // ---- verified area: Area > Zone > Map > global, exactly like before ----
    Sync(base, {571, 65, 4164, true}, true);
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 42, "area override wins");
    Require(ReadInt(L"Map.571.Zone.65.Area.4164.Water", L"RipplePermille", 0, locationW.c_str()) == 42, "scoped section reads from local file directly");
    {
        wchar_t value[64]{};
        ReadString(L"Water", L"WaterColour", L"", value, 64, graphicsW.c_str());
        Require(std::wstring(value) == L"0xAABBCC", "zone water colour beats map/global");
    }
    Require(ReadInt(L"Water", L"Enabled", 0, graphicsW.c_str()) == 1, "global falls through below area");
    Require(HasScopedValue(L"Water", L"WaterColour"), "scoped colour detected");
    Require(HasScopedValue(L"Water", L"RipplePermille"), "scoped int detected");
    Require(!HasScopedValue(L"Water", L"Enabled"), "global-only key not scoped");

    // global mask outranks the location chain when inherited (or unverified)
    {
        wchar_t value[64]{};
        ReadString(L"Water", L"FoamEnabled", L"1", value, 64, graphicsW.c_str());
        Require(std::wstring(value) == L"1", "explicit global requires inherit toggle in verified location");
        renderer::locationtuning::SetInheritGlobal(L"Water", L"FoamEnabled", true);
        ReadString(L"Water", L"FoamEnabled", L"1", value, 64, graphicsW.c_str());
        Require(std::wstring(value) == L"0", "explicit global outranks location when inherit toggle is on");
        renderer::locationtuning::SetInheritGlobal(L"Water", L"FoamEnabled", false);
    }

    // sibling area inherits zone, not the sibling's override
    Sync(base, {571, 65, 9999, true}, true);
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 30, "sibling area falls to map");
    Sync(base, {571, 65, 0, true}, true);
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 30, "zone-level read ignores area");
    {
        wchar_t value[64]{};
        ReadString(L"Water", L"WaterColour", L"", value, 64, graphicsW.c_str());
        Require(std::wstring(value) == L"0xAABBCC", "zone colour still resolves at zone level");
    }

    // ---- save round-trips and invalidates the cache ----
    Sync(base, {571, 65, 4164, true}, true);
    Require(Save(L"Water", L"RipplePermille", 44), "save writes");
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 44, "save visible immediately");
    Sync(base, {571, 65, 9999, true}, true);
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 30, "saved override stays scoped to its area");
    Sync(base, {571, 65, 4164, true}, true);

    // unverified save falls back to the global section
    Sync(base, {571, 65, 4164, true}, false);
    Require(Save(L"Water", L"RipplePermille", 50), "unverified save");
    Require(reference::GetInt(graphics, L"Water", L"RipplePermille", -1) == 50, "unverified save lands in global file");
    Sync(base, {571, 65, 4164, true}, true);
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 44, "verified read keeps area value over global");

    // ---- differential: cached reader vs per-call reference reader ----
    const std::vector<std::pair<std::wstring, std::wstring>> sectionsKeys = {
        {L"Water", L"RipplePermille"}, {L"Water", L"WaterColour"}, {L"Water", L"Enabled"},
        {L"Water", L"FoamEnabled"}, {L"Water", L"ShowStatus"}, {L"Water", L"Missing"},
        {L"Water", L"ZoneWaves"}, {L"Atmosphere", L"ShaftPercent"}, {L"Atmosphere", L"Missing"},
        {L"Volume", L"Enabled"},
    };
    const std::vector<renderer::LocationContext> places = {
        {}, {571, 65, 4164, true}, {571, 65, 9999, true}, {571, 65, 0, true}, {571, 0, 0, true}, {0, 0, 0, false},
    };
    bool differential = true;
    std::string firstDiff;
    for (auto place : places) {
        Sync(base, place, place.valid);
        for (auto& sk : sectionsKeys) {
            wchar_t cached[64]{};
            ReadString(sk.first.c_str(), sk.second.c_str(), L"", cached, 64, graphicsW.c_str());
            const std::wstring expect = reference::GetString(graphics, sk.first, sk.second, L"");
            const std::wstring got(cached);
            // The reference only sees the global file; the cached reader applies the
            // location chain. For keys that exist in the chain the values differ by
            // design, so compare against the reference of the right source instead.
            if (got != expect) {
                const std::wstring localExpect =
                    renderer::locationtuning::HasScopedValue(sk.first.c_str(), sk.second.c_str())
                        ? L"" // handled below
                        : L"";
                (void)localExpect;
                // Recompute the expected value with the chain, using the reference on
                // both files so the differential stays independent of the cache.
                std::wstring chain;
                if (place.valid) {
                    const std::wstring area = L"Map." + std::to_wstring(place.mapId) + L".Zone." + std::to_wstring(place.zoneId) + L".Area." + std::to_wstring(place.areaId) + L"." + sk.first;
                    const std::wstring zone = L"Map." + std::to_wstring(place.mapId) + L".Zone." + std::to_wstring(place.zoneId) + L"." + sk.first;
                    const std::wstring map = L"Map." + std::to_wstring(place.mapId) + L"." + sk.first;
                    chain = reference::GetString(location, area, sk.second, L"");
                    if (chain.empty()) chain = reference::GetString(location, zone, sk.second, L"");
                    if (chain.empty()) chain = reference::GetString(location, map, sk.second, L"");
                }
                const std::wstring globalMask = reference::GetString(location, L"Global." + sk.first, sk.second, L"");
                const bool inheritsGlobal = renderer::locationtuning::HasInheritGlobal(sk.first.c_str(), sk.second.c_str());
                const bool masked = !globalMask.empty() && (!place.valid || inheritsGlobal);
                const bool chained = place.valid && !chain.empty();
                const std::wstring want = masked ? (globalMask.empty() ? std::wstring(L"") : globalMask)
                                       : chained ? chain
                                       : (!expect.empty() ? expect : std::wstring());
                if (got != want) {
                    differential = false;
                    firstDiff = ("section=" + ToUtf8(sk.first) + " key=" + ToUtf8(sk.second) + " place=" + std::to_string(place.mapId) + "/" + std::to_string(place.zoneId) + "/" + std::to_string(place.areaId));
                    break;
                }
            }
        }
        if (!differential) break;
    }
    Require(differential, firstDiff.empty() ? "cached reads match reference chain semantics" : ("first mismatch: " + firstDiff).c_str());

    // ---- external edit is picked up on the next read (mtime/size invalidation) ----
    // The edit goes through the profile API rather than through Save, so the
    // module is not told about it and the only thing that can reveal the change is
    // the file's own identity. That identity is the last-write time plus the size,
    // and the stamp a write gets comes from the system's cached idea of the
    // current time, which moves in ticks of roughly 15 ms - so two writes inside
    // one tick are stamped identically. Writing "146" over "44" moves the SIZE as
    // well, which makes the edit visible whichever way the timestamp lands. The
    // renderer's own writes never depend on this: WriteProfile drops the cache
    // outright instead of asking the identity.
    //
    // It used to byte-patch the file, looking for the line spelled
    // "RipplePermille = 44". That spelling is the Linux fixture's, not the API's -
    // the real API writes "RipplePermille=44" - so the needle was never found on a
    // Windows runner and this check and the three after it failed there while
    // passing here. Asking the API to make the edit removes the guess.
    Sync(base, {571, 65, 4164, true}, true);
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 44, "pre-edit read");
    Require(WritePrivateProfileStringW(L"Map.571.Zone.65.Area.4164.Water", L"RipplePermille", L"146",
                                       locationW.c_str()) != 0,
            "external edit made through the profile API, not through the module");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 146, "external edit visible on the next read");

    // A duplicate key appended by a careless hand-edit must NOT shadow the
    // first one (GetPrivateProfileStringW keeps the first occurrence).
    {
        std::ofstream patch(location, std::ios::binary | std::ios::app);
        patch << "[Map.571.Zone.65.Area.4164.Water]\\nRipplePermille=47\\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 146, "duplicate key keeps first value");

    // ---- the cache is process-wide: other translation units share it ----
    Sync(base, {571, 65, 4164, true}, true);
    const unsigned otherBefore = LocationTuningOtherTuRead(graphicsW.c_str(), L"RipplePermille", 0);
    Require(otherBefore == 146, "second TU reads the shared cache");
    Require(Save(L"Water", L"RipplePermille", 147), "save in first TU");
    Require(LocationTuningOtherTuRead(graphicsW.c_str(), L"RipplePermille", 0) == 147,
            "save visible immediately in the second TU without waiting on file metadata");
    Require(LocationTuningOtherTuSave(L"Water", L"RipplePermille", 148), "save in second TU");
    Require(ReadInt(L"Water", L"RipplePermille", 0, graphicsW.c_str()) == 148, "second TU save visible in the first");
    Save(L"Water", L"RipplePermille", 146); // restore for the alias check below

    // ---- the expensive part: a full zone-change reload must stay in the low ms ----
    // Populate LocationGraphics.ini the way a long session does: dozens of
    // visited areas, each with several scoped sections, mirroring the size a
    // player's file reaches after weeks of F7 saves.
    {
        std::ostringstream big;
        big << "[Global.Water]\nFoamEnabled=0\n";
        for (int z = 1; z <= 40; ++z)
            for (int a = 1; a <= 12; ++a) {
                big << "[Map." << z << ".Zone." << (z * 7) << ".Area." << (z * 100 + a) << ".Water]\n";
                big << "RipplePermille=" << (10 + a) << "\nWaterColour=0x" << (100000 + z * a) << "\n";
                big << "[Map." << z << ".Zone." << (z * 7) << ".Area." << (z * 100 + a) << ".Atmosphere]\n";
                big << "ShaftPercent=" << (40 + z) << "\n";
                big << "[Map." << z << ".Zone." << (z * 7) << ".Volume]\nEnabled=1\n";
            }
        WriteFile(location, big.str());
    }
    // The real HookedPresent reload chain reads ~45 water keys, ~50 volume keys,
    // ~55 cloud keys, ~35 atmosphere keys, plus every F7 menu row. Reproduce a
    // conservative 1500-key sweep per zone change.
    Sync(base, {39, 39 * 7, 39 * 100 + 11, true}, true);
    const int keysPerChange = 1500;
    auto t0 = std::chrono::steady_clock::now();
    int sum = 0;
    for (int i = 0; i < keysPerChange; ++i)
        sum += static_cast<int>(ReadInt(i % 2 ? L"Atmosphere" : L"Water", L"RipplePermille", 0, graphicsW.c_str()));
    auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("zone-change reload sweep of %d reads took %.3f ms (sum=%d)\n", keysPerChange, ms, sum);
    Require(sum != 0, "sweep actually read authored values");
    Require(ms < 25.0, "1500 cached reads finish in well under 25 ms");

    // And the same sweep through the UNCACHED semantics (reference reader) to
    // show the gap the old code paid on every zone border.
    auto u0 = std::chrono::steady_clock::now();
    int usum = 0;
    for (int i = 0; i < keysPerChange; ++i)
        usum += reference::GetInt(graphics, i % 2 ? L"Atmosphere" : L"Water", L"RipplePermille", 0);
    auto u1 = std::chrono::steady_clock::now();
    const double ums = std::chrono::duration<double, std::milli>(u1 - u0).count();
    std::printf("same %d uncached reads took %.3f ms (sum=%d)\n", keysPerChange, ums, usum);

    // Cache must not alias different files with the same base name.
    const fs::path otherDir = fs::temp_directory_path() / L"locationtuning_cache_test2";
    std::error_code ec;
    fs::remove_all(otherDir, ec);
    fs::create_directories(otherDir, ec);
    const fs::path otherGraphics = otherDir / L"GraphicsEffects.ini";
    WriteFile(otherGraphics, "[Water]\nRipplePermille=99\n");
    Sync((otherDir / L"").wstring(), {571, 65, 4164, true}, true);
    Require(ReadInt(L"Water", L"RipplePermille", 0, otherGraphics.wstring().c_str()) == 99, "second file not aliased");

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
