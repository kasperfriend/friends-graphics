// Measures and pins the cost of a ZONE/AREA CROSSING - the moment a player walks
// over a border and every per-zone and per-area setting has to be re-read.
//
// tools/LocationTuningCacheRegression.cpp covers the cached READS (one key at a
// time) and runs on Windows only, against the real profile API. This harness runs
// the same production header on Linux against tools/fixtures/windows.h, and asks
// the question a player actually feels, which the per-key sweep cannot answer:
// what does ONE crossing cost end to end, and how does that cost move as the
// player accumulates per-zone and per-area settings?
//
// A crossing is four things, in the order HookedPresent does them:
//   1. locationtuning::Sync            - adopt the place that settled
//   2. AppliedTuningStillInForce       - decide whether anything must be re-read
//   3. the reload sweep                - every key every subsystem reads
//   4. MarkTuningApplied               - remember what is now in force
// Steps 2 and 4 are the fingerprint, and the fingerprint walks the FILE: every
// section name of both files, and every level of every suffix. That is the part
// whose cost grows with the number of per-zone and per-area settings a player has
// authored, which is exactly the growth this harness has to keep flat.
//
// The shim's profile API re-parses the whole file per call, as the real one does,
// and counts every call, so the numbers below are API calls avoided rather than
// an impression of speed.
#include "../src/Environment/LocationTuning.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using renderer::LocationContext;
namespace lt = renderer::locationtuning;

static int failures = 0;

// The absolute millisecond budgets below are about a frame, so they are only
// meaningful on an optimised build. Under a sanitizer everything is tens of times
// slower and a budget would fail for the instrumentation, not for the code - so
// the structural assertions (did the skip fire, how many loads, does the cost stay
// flat as the settings grow) still run, and only the wall-clock ones step aside.
// gcc spells the flag __SANITIZE_ADDRESS__, clang __SANITIZER_ADDRESS__; either is
// enough, and a build can also say so outright with CROSSING_NO_TIMING_BUDGETS.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZER_ADDRESS__) || \
    defined(__SANITIZER_MEMORY__) || defined(CROSSING_NO_TIMING_BUDGETS)
static constexpr bool kTimingBudgetsMeaningful = false;
#else
static constexpr bool kTimingBudgetsMeaningful = true;
#endif

static void Require(bool ok, const std::string& message)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message.c_str());
    if (!ok) ++failures;
}

// ---------------------------------------------------------------------------
// The workload. GraphicsEffects.ini ships 13 sections and 206 keys; a full
// re-apply reads them through three different doors - the runtime read
// (ReadInt/ReadString), the F7 editor read (ReadEditorInt, which walks the
// location levels) and the presence probe the water colours use
// (HasScopedValue). Reproducing all three is what makes the sweep below the same
// shape as the eight ReloadTuning calls plus tuningoverlay::Configure.
struct TuningKey { std::wstring section, key; };

// The shim writes narrow bytes on purpose (Linux wide streams are unreliable
// under a POSIX locale) and the authored content is ASCII, so a widen/narrow
// round trip is exact here.
static std::string Narrow(const wchar_t* text)
{
    std::string out;
    for (const wchar_t* p = text; *p; ++p) out.push_back(static_cast<char>(*p));
    return out;
}

static const std::vector<std::pair<const wchar_t*, int>>& ShippedSections()
{
    // Section name and key count, as shipped in GraphicsEffects.ini.
    static const std::vector<std::pair<const wchar_t*, int>> sections = {
        {L"Atmosphere", 47}, {L"Water", 46}, {L"VolumetricClouds", 44}, {L"Wind", 12},
        {L"WeatherVisuals", 15}, {L"LocalFog", 7}, {L"DynamicLighting", 5},
        {L"ShadowDiagnostics", 5}, {L"PostProcess", 6},
        {L"AmbientOcclusion", 5}, {L"NativeShadows", 3},
        {L"DistanceFog", 3}, {L"Underwater", 8},
    };
    return sections;
}

static std::vector<TuningKey> BuildShippedIni(const fs::path& dir)
{
    std::vector<TuningKey> keys;
    std::ostringstream ini;
    int n = 0;
    for (const auto& [section, count] : ShippedSections()) {
        ini << "[" << Narrow(section) << "]\n";
        for (int i = 0; i < count; ++i) {
            const std::wstring key = L"Key" + std::to_wstring(++n);
            ini << Narrow(key.c_str()) << "=" << (n % 97) << "\n";
            keys.push_back({section, key});
        }
    }
    std::ofstream out(dir / "GraphicsEffects.ini", std::ios::binary | std::ios::trunc);
    out << ini.str();
    // The fallback file watereffect::ReadTuning consults when a key is absent.
    std::ofstream main(dir / "ModernWoWRenderer.ini", std::ios::binary | std::ios::trunc);
    main << "[Water]\nEnabled=1\n[Volume]\nEnabled=1\n";
    return keys;
}

// A LocationGraphics.ini the way a long session leaves it: every area the player
// ever tuned, each with `sectionsPerArea` scoped sections of `keysPerSection`
// keys. `areas` x `sectionsPerArea` is the number of sections in the file, which
// is what the fingerprint's section walk is priced against.
static void BuildScopedIni(const fs::path& dir, int zones, int areasPerZone,
                           int sectionsPerArea, int keysPerSection)
{
    static const wchar_t* suffixes[] = {L"Water", L"Atmosphere", L"VolumetricClouds",
                                        L"Wind", L"WeatherVisuals",
                                        L"LocalFog", L"AmbientOcclusion"};
    std::ostringstream ini;
    ini << "[Global.Water]\nFoamEnabled=0\n";
    for (int z = 1; z <= zones; ++z) {
        const int zoneId = z * 7;
        for (int a = 1; a <= areasPerZone; ++a) {
            const int areaId = z * 100 + a;
            for (int s = 0; s < sectionsPerArea; ++s) {
                const std::string name = Narrow(suffixes[s % (sizeof suffixes / sizeof *suffixes)]);
                ini << "[Map." << z << ".Zone." << zoneId << ".Area." << areaId << "." << name << "]\n";
                for (int k = 1; k <= keysPerSection; ++k)
                    ini << "Key" << (k + s) << "=" << (z + a + k) % 89 << "\n";
            }
            // Zone-level scope too, so the walk has all three levels to visit.
            if (a == 1) {
                for (int s = 0; s < sectionsPerArea; ++s) {
                    const std::string name = Narrow(suffixes[s % (sizeof suffixes / sizeof *suffixes)]);
                    ini << "[Map." << z << ".Zone." << zoneId << "." << name << "]\n";
                    for (int k = 1; k <= keysPerSection; ++k)
                        ini << "Key" << (k + s) << "=" << (z + k) % 71 << "\n";
                }
            }
        }
    }
    std::ofstream out(dir / "LocationGraphics.ini", std::ios::binary | std::ios::trunc);
    out << ini.str();
}

// ---------------------------------------------------------------------------
// One crossing, timed and counted the way HookedPresent performs it.
struct CrossingCost {
    double syncMs = 0, decideMs = 0, sweepMs = 0, markMs = 0;
    unsigned long long stats = 0, parses = 0, nameLists = 0, reads = 0;
    bool skipped = false;
    double Total() const { return syncMs + decideMs + sweepMs + markMs; }
};

static CrossingCost Cross(const std::wstring& base, const std::vector<TuningKey>& keys,
                          const LocationContext& place, const std::wstring& graphics)
{
    // Let the identity throttle expire so the crossing pays for its own stat, as
    // a real one does: crossings are seconds apart, and the 10 ms window in
    // ProfileIdentity only ever collapses the reads INSIDE one apply.
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    auto& api = shiminix::Stats();
    api.Reset();
    const uint64_t readsBefore = lt::profileReads;
    const uint64_t parsesBefore = lt::profileParses;
    const auto clock = [] { return std::chrono::steady_clock::now(); };

    CrossingCost cost;
    auto t = clock();
    lt::Sync(base, place, place.valid);
    cost.syncMs = std::chrono::duration<double, std::milli>(clock() - t).count();

    t = clock();
    const bool stale = !lt::AppliedTuningStillInForce(place, place.valid);
    cost.decideMs = std::chrono::duration<double, std::milli>(clock() - t).count();
    cost.skipped = !stale;

    if (stale) {
        t = clock();
        // The reload sweep: every shipped key through the runtime read, the F7
        // editor read and - for the water keys - the presence probe.
        for (const auto& entry : keys) {
            lt::ReadInt(entry.section.c_str(), entry.key.c_str(), 0, graphics.c_str());
            lt::ReadEditorInt(entry.section.c_str(), entry.key.c_str(), 0, graphics.c_str());
            if (entry.section == L"Water")
                lt::HasScopedValue(entry.section.c_str(), entry.key.c_str());
        }
        cost.sweepMs = std::chrono::duration<double, std::milli>(clock() - t).count();

        t = clock();
        lt::MarkTuningApplied(place, place.valid);
        cost.markMs = std::chrono::duration<double, std::milli>(clock() - t).count();
    }

    cost.stats = api.stats;
    cost.parses = api.fileOpens;
    cost.nameLists = api.nameLists;
    cost.reads = lt::profileReads - readsBefore;
    (void)parsesBefore;
    return cost;
}

static void Report(const char* label, const CrossingCost& c)
{
    std::printf("      %-34s %8.3f ms  (sync %.3f decide %.3f sweep %.3f mark %.3f)  "
                "stats=%llu parses=%llu namelists=%llu reads=%llu%s\n",
                label, c.Total(), c.syncMs, c.decideMs, c.sweepMs, c.markMs,
                c.stats, c.parses, c.nameLists, c.reads, c.skipped ? "  [SKIPPED]" : "");
}

struct Scale { const char* name; int zones, areasPerZone, sectionsPerArea, keysPerSection; };

// ---------------------------------------------------------------------------
// The cache no longer asks the profile API for a section: it reads the file once
// and parses it itself. That is only safe if the parser answers exactly what
// GetPrivateProfileStringW answers, so every rule it has to mirror is pinned here
// against the shim's independent implementation of the API - and then pinned again
// as a differential over the whole shipped key set.
static void TestParserSemantics(const fs::path& dir)
{
    std::printf("\n  parser semantics (cached parse vs the profile API)\n");
    const fs::path file = dir / "ParserSemantics.ini";
    const std::wstring path = file.wstring();

    // One file exercising every rule at once.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "Orphan=1\r\n"                       // before any section: ignored
               "; a comment\r\n"
               "# another comment\r\n"
               "[Spaced]\r\n"
               "Note = hello  world \r\n"           // edge blanks off, inner ones kept
               "Quoted = \"0xAABBCC\"\r\n"         // a matching pair comes off
               "SpacedQuote = \"  kept  \"\r\n"    // blanks INSIDE quotes stay
               "Single = 'abc'\r\n"                  // single quotes come off too
               "Mismatched = \"abc'\r\n"            // no matching pair: left alone
               "WrongOrder = 'abc\"\r\n"            // wrong order: left alone
               "Nested = \"'x'\"\r\n"              // only the outermost pair
               "Trailing=value   \r\n"               // trailing blanks off
               "Tabs=\tvalue\t\r\n"                // tabs are blanks too
               "Semicolon=; not a comment\r\n"       // only a LINE starting with ';' is
               "Blank=\r\n"                          // absent
               "BlankSpaces=   \r\n"                 // absent
               "Dup=1\r\n"
               "Dup=2\r\n"                           // first occurrence wins
               "  TrimmedKey  =  7  \r\n"            // key and value both trimmed
               "NoEqualsLine\r\n"
               "[MiXeD]\r\n"
               "CaseKey=5\r\n"
               "[Empty]\r\n"
               "[Dup]\r\n"                           // a second header merges
               "Dup=3\r\n"
               "OnlyHere=9\r\n";
    }

    // Every answer the cached parse gives must equal the API's, byte for byte.
    const std::pair<const wchar_t*, const wchar_t*> probes[] = {
        {L"Spaced", L"Note"}, {L"Spaced", L"Quoted"}, {L"Spaced", L"SpacedQuote"},
        {L"Spaced", L"Single"}, {L"Spaced", L"Mismatched"}, {L"Spaced", L"WrongOrder"},
        {L"Spaced", L"Nested"}, {L"Spaced", L"Trailing"}, {L"Spaced", L"Tabs"},
        {L"Spaced", L"Semicolon"}, {L"Spaced", L"Blank"}, {L"Spaced", L"BlankSpaces"},
        {L"Spaced", L"Dup"}, {L"Spaced", L"TrimmedKey"},
        {L"Spaced", L"NoEqualsLine"}, {L"Spaced", L"Orphan"}, {L"Spaced", L"Missing"},
        {L"mixed", L"casekey"}, {L"MIXED", L"CaseKey"}, {L"Empty", L"Anything"},
        {L"Dup", L"Dup"}, {L"Dup", L"OnlyHere"}, {L"NoSuchSection", L"Note"},
    };
    bool same = true;
    std::string firstDiff;
    // The fallback is the empty string every production caller passes: ReadInt and
    // ReadString both ask for L"" and apply their own default to an empty answer.
    // That is what lets Blank and BlankSpaces sit in this list at all - the API
    // returns an empty string for a key that is present with no value, and this
    // module reads such a key as absent and answers the fallback, so the two agree
    // for every caller here. With a non-empty fallback they would not, which is
    // why the rule is also asserted by name below.
    for (const auto& [section, key] : probes) {
        wchar_t cached[128]{}, api[128]{};
        lt::InvalidateScopedKeys();
        lt::ReadProfileString(section, key, L"", cached, 128, path.c_str());
        GetPrivateProfileStringW(section, key, L"", api, 128, path.c_str());
        if (std::wstring(cached) != std::wstring(api)) {
            same = false;
            firstDiff = Narrow(section) + "/" + Narrow(key) + ": cached=[" +
                        Narrow(cached) + "] api=[" + Narrow(api) + "]";
            break;
        }
    }
    Require(same, same ? "the cached parse answers every probe exactly as the profile API does"
                       : ("parser/API mismatch: " + firstDiff).c_str());

    // And the rules named outright, so a mismatch says which rule moved.
    lt::InvalidateScopedKeys();
    auto Value = [&](const wchar_t* section, const wchar_t* key) {
        wchar_t out[128]{};
        lt::ReadProfileString(section, key, L"<none>", out, 128, path.c_str());
        return std::wstring(out);
    };
    // The blanks a value was padded with are not part of it - the API strips them,
    // so a file laid out in columns reads the same as one that was not. The blanks
    // INSIDE the value are part of it and stay.
    Require(Value(L"Spaced", L"Note") == L"hello  world",
            "a value's surrounding blanks come off and its inner ones stay");
    Require(Value(L"Spaced", L"Trailing") == L"value" && Value(L"Spaced", L"Tabs") == L"value",
            "trailing blanks and tabs come off too");
    Require(Value(L"Spaced", L"Quoted") == L"0xAABBCC" && Value(L"Spaced", L"Single") == L"abc",
            "one matching pair of surrounding quotes comes off");
    Require(Value(L"Spaced", L"SpacedQuote") == L"  kept  ",
            "blanks inside quotes are kept");
    Require(Value(L"Spaced", L"Nested") == L"'x'", "only the outermost pair of quotes comes off");
    Require(Value(L"Spaced", L"Mismatched") == L"\"abc'" && Value(L"Spaced", L"WrongOrder") == L"'abc\"",
            "quotes that do not match are left alone");
    Require(Value(L"Spaced", L"Semicolon") == L"; not a comment",
            "a ';' only starts a comment at the beginning of a line");
    Require(Value(L"Spaced", L"Blank") == L"<none>" && Value(L"Spaced", L"BlankSpaces") == L"<none>",
            "a blank value is absent, so the fallback answers");
    Require(Value(L"Spaced", L"Dup") == L"1", "a duplicate key keeps its first value");
    Require(Value(L"Spaced", L"TrimmedKey") == L"7", "the key and the value are both trimmed");
    Require(Value(L"Spaced", L"NoEqualsLine") == L"<none>", "a line without '=' is not a key");
    Require(Value(L"Spaced", L"Orphan") == L"<none>", "a line before any section is ignored");
    Require(Value(L"mixed", L"CASEKEY") == L"5", "sections and keys match case-insensitively");
    Require(Value(L"Empty", L"Anything") == L"<none>", "an empty section answers the fallback");
    Require(Value(L"Dup", L"OnlyHere") == L"9", "a repeated section header merges");

    // A Unicode file is what WritePrivateProfileStringW produces once a file has a
    // BOM, so the decode has to handle it rather than read it as ANSI bytes.
    {
        const fs::path wide = dir / "ParserSemanticsWide.ini";
        std::string bytes;
        bytes.push_back(char(0xFF)); bytes.push_back(char(0xFE));
        const std::wstring text = L"[Water]\r\nColour=0x123456\r\n";
        for (wchar_t c : text) {
            const auto unit = static_cast<unsigned short>(c);
            bytes.push_back(static_cast<char>(unit & 0xFF));
            bytes.push_back(static_cast<char>((unit >> 8) & 0xFF));
        }
        {
            std::ofstream out(wide, std::ios::binary | std::ios::trunc);
            out << bytes;
        }
        lt::InvalidateScopedKeys();
        wchar_t cached[64]{}, api[64]{};
        lt::ReadProfileString(L"Water", L"Colour", L"<none>", cached, 64, wide.wstring().c_str());
        GetPrivateProfileStringW(L"Water", L"Colour", L"<none>", api, 64, wide.wstring().c_str());
        const bool wideOk = std::wstring(cached) == L"0x123456" && std::wstring(cached) == std::wstring(api);
        if (!wideOk)
            std::printf("      (cached=[%s] api=[%s])\n", Narrow(cached).c_str(), Narrow(api).c_str());
        Require(wideOk, "a UTF-16 settings file is decoded, not read as bytes");
    }

    // A file that does not exist is an empty answer, not an error - which is what a
    // fresh install with no LocationGraphics.ini yet has to look like.
    lt::InvalidateScopedKeys();
    {
        const std::wstring missing = (dir / "ParserSemanticsMissing.ini").wstring();
        wchar_t cached[64]{};
        lt::ReadProfileString(L"Water", L"Colour", L"<none>", cached, 64, missing.c_str());
        Require(std::wstring(cached) == L"<none>", "a missing settings file answers the fallback");
        Require(lt::SectionNamesOf(missing) != nullptr,
                "and its section listing is provable, so the crossing skip still works");
    }
}

int main()
{
    const fs::path dir = fs::temp_directory_path() / "locationtuning_crossing_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // g_basePath always ends in a separator; locationtuning concatenates names.
    const std::wstring base = (dir / "").wstring();
    const std::vector<TuningKey> keys = BuildShippedIni(dir);
    const std::wstring graphics = (dir / "GraphicsEffects.ini").wstring();
    const fs::path scoped = dir / "LocationGraphics.ini";

    std::printf("crossing harness: %zu shipped keys over %zu sections\n",
                keys.size(), ShippedSections().size());

    TestParserSemantics(dir);

    // Correctness first: the numbers below mean nothing if the reads are wrong.
    BuildScopedIni(dir, 40, 12, 3, 2);
    lt::InvalidateScopedKeys();
    lt::Sync(base, {39, 39 * 7, 39 * 100 + 11, true}, true);
    Require(lt::ReadInt(L"Water", L"Key1", -1, graphics.c_str()) == static_cast<int>((39 + 11 + 1) % 89),
            "an area override wins over the shipped file");
    lt::Sync(base, {39, 39 * 7, 0, true}, true);
    Require(lt::ReadInt(L"Water", L"Key1", -1, graphics.c_str()) == static_cast<int>((39 + 1) % 71),
            "a zone-level place reads the zone scope, not the area one");
    lt::Sync(base, {2, 2 * 7, 2 * 100 + 5, true}, true);
    Require(lt::ReadInt(L"Water", L"Key1", -1, graphics.c_str()) == static_cast<int>((2 + 5 + 1) % 89),
            "a different zone reads its own scope");

    // The skip must not swallow a real difference: a hand-written scope in a
    // section no read has touched has to force a re-apply.
    lt::Sync(base, {39, 39 * 7, 39 * 100 + 11, true}, true);
    lt::MarkTuningApplied({39, 39 * 7, 39 * 100 + 11, true}, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    Require(lt::AppliedTuningStillInForce({39, 39 * 7, 39 * 100 + 11, true}, true),
            "the same place with both files untouched is still in force");
    {
        std::ofstream patch(scoped, std::ios::binary | std::ios::app);
        patch << "[Map.39.Zone.273.Area.3911.Wind]\nKey9=77\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Require(!lt::AppliedTuningStillInForce({39, 39 * 7, 39 * 100 + 11, true}, true),
            "a hand edit in a section nothing had read still forces a re-apply");
    lt::MarkTuningApplied({39, 39 * 7, 39 * 100 + 11, true}, true);
    Require(lt::ReadInt(L"Wind", L"Key9", -1, graphics.c_str()) == 77,
            "and the newly written override is what the next read answers");

    // A place whose own scope differs must never be skipped either.
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    Require(!lt::AppliedTuningStillInForce({2, 2 * 7, 2 * 100 + 5, true}, true),
            "a genuinely different place is not skipped");

    // ---- the measurement: how a crossing scales with authored settings ----
    const Scale scales[] = {
        {"fresh install (no scoped file)", 0, 0, 0, 0},
        {"a few tuned areas", 4, 3, 2, 3},
        {"a long session", 40, 12, 3, 2},
        {"many settings per area", 40, 12, 8, 12},
        {"very many settings per area", 60, 20, 8, 20},
    };

    struct Row { const char* name; double flap = 0, genuine = 0, cold = 0;
                 unsigned long long flapStats = 0, genuineLoads = 0;
                 bool flapSkipped = false, genuineApplied = false; };
    std::vector<Row> rows;

    // The flap this client actually produces walking a border, from a player's
    // session log: `Area=0` and `Area=223` trading places. Neither area has a
    // scope of its own, so both resolve to the same Map+Zone settings and the
    // crossing has nothing to re-read - which is the case the skip exists for.
    const LocationContext flapA{39, 39 * 7, 0, true};
    const LocationContext flapB{39, 39 * 7, 223, true};
    // A place with its own authored overrides, so a crossing into it genuinely
    // changes what every subsystem reads.
    const LocationContext tuned{2, 2 * 7, 2 * 100 + 5, true};

    for (const auto& scale : scales) {
        fs::remove(scoped, ec);
        if (scale.zones) BuildScopedIni(dir, scale.zones, scale.areasPerZone,
                                        scale.sectionsPerArea, scale.keysPerSection);
        const long long bytes = scale.zones ? static_cast<long long>(fs::file_size(scoped, ec)) : 0;
        std::printf("\n  %s: LocationGraphics.ini = %lld bytes\n", scale.name, bytes);

        // Cold: nothing has been read or applied in this file's identity yet.
        lt::InvalidateScopedKeys();
        lt::Sync(base, {}, false);
        lt::appliedFingerprint = 0;
        const CrossingCost cold = Cross(base, keys, tuned, graphics);
        Report("cold first apply", cold);

        // Walking onto the border: a real change of settings, so a real apply.
        const CrossingCost onto = Cross(base, keys, flapA, graphics);
        Report("walk onto the border", onto);

        // Now the flap itself, both directions and then back again. The place
        // changes on every one of these; nothing any subsystem reads does.
        const CrossingCost flap1 = Cross(base, keys, flapB, graphics);
        Report("border flap Area=0 -> Area=223", flap1);
        const CrossingCost flap2 = Cross(base, keys, flapA, graphics);
        Report("border flap Area=223 -> Area=0", flap2);
        const CrossingCost flap3 = Cross(base, keys, flapB, graphics);
        Report("border flap Area=0 -> Area=223", flap3);

        // A real crossing back into a place with its own authored overrides.
        const CrossingCost genuine = Cross(base, keys, tuned, graphics);
        Report("genuine crossing (new settings)", genuine);

        const double worstFlap = std::max({flap1.Total(), flap2.Total(), flap3.Total()});
        const unsigned long long worstFlapStats = std::max({flap1.stats, flap2.stats, flap3.stats});
        rows.push_back({scale.name, worstFlap, genuine.Total(), cold.Total(), worstFlapStats,
                        genuine.parses, flap1.skipped && flap2.skipped && flap3.skipped,
                        // With no scoped file at all, every place resolves to the
                        // shipped settings, so a crossing back is correctly a
                        // no-op: only ask for a re-apply where `tuned` has
                        // something of its own to change.
                        scale.zones >= 2 ? !genuine.skipped : genuine.skipped});
    }

    // ---- budgets ----
    // A frame is 16.7 ms at 60 fps. The flap is the common case - it is what this
    // client produces on every other sample of a walk along a border - so it has
    // to stay inside a fraction of one frame no matter how much the player has
    // authored. A genuine crossing re-reads everything, so it gets a wider
    // budget, but it must still not be the multi-hundred-ms hitch a player
    // reports: that is a visible freeze, not a slow frame.
    std::printf("\n  budgets\n");
    bool flat = true;
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].flap > rows[0].flap * 6.0 + 1.0) flat = false;
    }
    for (const auto& row : rows) {
        std::printf("      %-34s flap %7.3f ms   genuine %7.3f ms   cold %7.3f ms\n",
                    row.name, row.flap, row.genuine, row.cold);
        // Measured on this harness: a flap is 0.05-0.10 ms, a genuine crossing
        // 0.6-0.9 ms and a cold apply 0.8-23 ms at every scale up to a
        // two-megabyte settings file. The budgets are wide enough for a slow
        // shared runner and narrow enough that a return to one whole-file parse
        // per section - 22 ms for the flap and 570 ms for the crossing at a
        // 587 KB file - cannot pass.
        if (kTimingBudgetsMeaningful) {
            Require(row.flap < 2.0, (std::string("border flap costs nothing worth feeling: ") + row.name).c_str());
            Require(row.genuine < 15.0, (std::string("genuine crossing stays inside one frame: ") + row.name).c_str());
            Require(row.cold < 100.0, (std::string("cold first apply stays well under a visible hitch: ") + row.name).c_str());
        }
        // The skip has to FIRE, not merely be cheap when it does. A section-name
        // listing with a fixed buffer silently turned it off above about a
        // thousand authored scopes, and a crossing then paid the full re-apply
        // every time while looking exactly as correct as one that skipped.
        Require(row.flapSkipped, (std::string("the border flap is skipped, not re-applied: ") + row.name).c_str());
        Require(row.genuineApplied, (std::string("a crossing that changes settings is still applied: ") + row.name).c_str());
    }
    // Relative, so it holds under a sanitizer too: this is the assertion that
    // actually protects a player who authors a lot of per-area settings.
    Require(flat, "the flap cost does not grow with the number of authored per-zone/per-area settings");

    // And the flap must not touch the disk per key: it is a decision, not a read.
    Require(rows.back().flapStats <= 8,
            "the flap's decision costs a bounded handful of stats, not one per setting");
    // A crossing that does re-apply reads each file once, not once per section.
    Require(rows.back().genuineLoads <= 4,
            "a re-apply loads each settings file once, not once per section it asks about");

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL CROSSING CHECKS PASSED\n", failures);
    return failures ? 1 : 0;
}
