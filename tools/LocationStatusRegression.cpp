// Host-independent checks for the location diagnosis surface: the clause the F7 header
// shows when a location could not be resolved, the honest rendering of "never sampled",
// the INI value trimming that keeps a hand-edited [LocationProvider] section working, and
// the source-level rules that keep a missing or rejected EnvironmentProfiles.ini from
// switching the location provider off in every zone.
//
// The runtime side of this lives in src/Game/WoWLocationProvider.cpp, which needs
// windows.h and is therefore checked on the Windows runner (CityLocationRegression).
// Everything here is the part that can be reasoned about without a client.
#include "../src/Environment/LocationStatus.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace renderer;

namespace
{
    void Require(bool value, const char* name)
    {
        if (!value) throw std::runtime_error(name);
        std::printf("PASS %s\n", name);
    }

    // The pins below are statements about the shipped sources, so the test reads them the
    // way tools/WaterRegression.cpp does: relative to the working directory (the CI job
    // runs from the repository root) and, when the compiler was given an absolute source
    // path, relative to this file too. A "missing source" failure is never a diagnosis.
    std::string ReadSource(const char* path)
    {
        const std::string here(__FILE__);
        const size_t slash = here.find_last_of("/\\");
        std::vector<std::string> candidates{path};
        if (slash != std::string::npos) candidates.push_back(here.substr(0, slash + 1) + "../" + path);
        for (const std::string& candidate : candidates)
        {
            std::ifstream input{candidate};
            std::ostringstream buffer;
            buffer << input.rdbuf();
            if (!buffer.str().empty()) return buffer.str();
        }
        throw std::runtime_error(std::string("missing source: ") + path);
    }

    bool Contains(const std::string& text, const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    // Line 1 exactly as the manager composes it, so the width rule is tested rather than
    // assumed: the overlay draws this at eight pixels per glyph across a 700 px panel and
    // clips the tail without wrapping.
    std::string Headline(LocationFailure failure, bool sampled, uint32_t mapId, uint32_t zoneId, uint32_t areaId)
    {
        return "Location unavailable: " + std::string(LocationFailureText(failure)) +
            "  |  Map/Zone/Area: " + LocationIdsText(sampled, mapId, zoneId, areaId);
    }
}

int main() try
{
    // A refused sample must never look like the client reported zeros: map 0 is a real
    // continent (Eastern Kingdoms) and area 0 is a real "no subarea".
    Require(LocationIdsText(false, 0, 0, 0) == "not sampled", "an un-sampled provider does not print 0 / 0 / 0");
    Require(LocationIdsText(true, 0, 14, 0) == "0 / 14 / 0", "a sampled zero map and zero subarea print as real ids");

    // Every cause has to fit on the one line the header has, including the worst wording.
    bool fits = true, shortEnough = true;
    std::string widest;
    for (uint8_t code = 0; code <= 8; ++code) {
        const LocationFailure failure = static_cast<LocationFailure>(code);
        const std::string line = Headline(failure, false, 0, 0, 0);
        if (line.size() > widest.size()) widest = line;
        fits = fits && line.size() <= 82;
        shortEnough = shortEnough && std::string(LocationFailureText(failure)).size() <= 27;
    }
    Require(fits, "every location cause fits the F7 header line without clipping");
    Require(shortEnough, "every location cause stays a clause");
    std::printf("     widest header line is %zu chars: %s\n", widest.size(), widest.c_str());
    Require(std::string(LocationFailureText(LocationFailure::NotConfigured)) == "not configured", "unconfigured provider names itself plainly");
    Require(std::string(LocationFailureText(LocationFailure::FingerprintMismatch)).find("executable") != std::string::npos,
        "an unmatched client says the executable is unrecognized, not that the world is empty");
    Require(std::string(LocationFailureText(LocationFailure::None)) == "resolved", "the success cause is not a failure");

    // The two causes that are installation problems name the file, because they are
    // otherwise indistinguishable from "this client build is unsupported".
    Require(Contains(LocationFailureHint(LocationFailure::NotConfigured, true), "EnvironmentProfiles.ini"),
        "an unconfigured provider points at the profile file it needed");
    Require(Contains(LocationFailureHint(LocationFailure::UnvalidatedIds, false), "areas.txt"),
        "unvalidated ids point at the area database when it is absent");
    Require(LocationFailureHint(LocationFailure::UnvalidatedIds, true).empty(),
        "unvalidated ids with a loaded database are a research problem, not a missing file");
    Require(LocationFailureHint(LocationFailure::FingerprintMismatch, true).empty() ||
            Contains(LocationFailureHint(LocationFailure::FingerprintMismatch, true), "LocationProvider"),
        "a fingerprint mismatch points at the section that can accept it");

    // GetPrivateProfileStringW returns the value verbatim, so the provider has to trim
    // before comparing a hash or parsing an address - and still refuse trailing junk.
    Require(TrimIniValue(L" e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6 ") ==
            "e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6",
        "a pasted hash survives surrounding spaces");
    Require(TrimIniValue(L"\tvalue\r\n") == "value", "profile line endings are trimmed");
    Require(TrimIniValue(L"").empty(), "an absent key reads as no value");
    uint64_t parsed = 0;
    Require(ParseIniUnsigned(L" 0x7D088C ", parsed) && parsed == 0x7D088C, "a hex address with spaces parses");
    Require(ParseIniUnsigned(L"12340", parsed) && parsed == 12340, "a decimal address parses");
    Require(!ParseIniUnsigned(L"0x7D088C ; researched", parsed), "a trailing comment is not a silent misparse");
    Require(!ParseIniUnsigned(L"", parsed) && !ParseIniUnsigned(L"   ", parsed), "blank or whitespace-only values are rejected");
    Require(!ParseIniUnsigned(L"not-a-number", parsed), "a non-numeric value is rejected");
    Require(!ParseIniUnsigned(L"999999999999999999999999", parsed), "an out-of-range value is rejected rather than wrapped");

    // And now the reason this file exists. A rejected or absent profile file used to
    // return from Reload() before provider.Configure(), which left the provider
    // unconfigured for the whole session: every zone then read "Location unavailable"
    // with the ids at 0 / 0 / 0 - including for a player whose only crime was not copying
    // EnvironmentProfiles.ini, which the install instructions did not ask for.
    {
        const std::string manager = ReadSource("src/Environment/EnvironmentProfileManager.cpp");
        const auto configure = manager.find("provider.Configure(basePath+L\"EnvironmentProfiles.ini\")");
        const auto rejected = manager.find("Profile reload rejected");
        Require(configure != std::string::npos && rejected != std::string::npos && configure < rejected,
            "Reload configures the location provider before it can bail on the profile file");
        Require(Contains(manager, "if(!provider.Available())Log(\"Location unavailable: \""),
            "an unusable provider is logged with its cause, not only its status prose");
        Require(Contains(manager, "\"  |  Map/Zone/Area: \"+LocationIdsText(provider.Sampled(),raw.mapId,raw.zoneId,raw.areaId)"),
            "the F7 header line carries the cause and the not-sampled marker");
        Require(!Contains(manager, "Raw Map / Zone / Area: \"+std::to_string(raw.mapId)"),
            "the header no longer prints raw zero ids for a provider that never read");
    }
    {
        const std::string provider = ReadSource("src/Game/WoWLocationProvider.cpp");
        Require(Contains(provider, "TrimIniValue(iniValue(L\"ExeSHA256\"))"),
            "the expected executable hash is trimmed before the exact compare");
        Require(Contains(provider, "ParseIniUnsigned(iniValue(key),value)"),
            "location RVAs go through the trimming parser, not a raw wcstoull");
        Require(!Contains(provider, "wcstoull(value,&end,0)"), "the old whitespace-intolerant RVA parse is gone");
        int causes = 0;
        for (const char* needle : {"failure=configureFailure=LocationFailure::UnsupportedExecutable", "failure=configureFailure=LocationFailure::FingerprintMismatch",
                                  "failure=configureFailure=LocationFailure::UnreadableRvas", "failure=LocationFailure::ReadFailed"})
            if (Contains(provider, needle)) ++causes;
        Require(causes == 4, "every Configure/Sample refusal records which refusal it was");
        Require(Contains(provider, "rawSample=a;sampled=true;"), "raw ids are marked as read only after a successful read");
        // The always-"not configured" report: Sample() runs on every poll, and it used
        // to rename every Configure() refusal to NotConfigured on the first world frame,
        // so the panel (and the log line built from the same value) pointed at a missing
        // INI file when the real cause was an unrecognized executable or unreadable RVAs.
        Require(Contains(provider, "failure=configureFailure=LocationFailure::FingerprintMismatch") &&
                Contains(provider, "if(!available) {validation=\"Provider unavailable: \"+status;failure=configureFailure;return {};}"),
            "a refused sample keeps the configure-time cause instead of renaming it not configured");
        Require(!Contains(provider, "failure=available?LocationFailure::NoWorldContext:LocationFailure::NotConfigured"),
            "Sample no longer collapses every unavailable provider into not configured");
        Require(Contains(provider, "failure=configureFailure=LocationFailure::NoWorldContext;"),
            "a configured provider waiting on a world frame says so instead of not configured");
        Require(Contains(provider, "57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da") &&
                Contains(provider, "ed62bd406dbe92cabe4b38759c630df25a8d3b02a2646e1b77f62cb7be273c1d") &&
                Contains(provider, "HasVerified12340CodeSignature") &&
                Contains(provider, "StockParentZone"),
            "stock 3.3.5a and the supplied ArdenWoW fingerprint use the supported location provider");
        const std::string inspector=ReadSource("tools/EnvironmentDatabaseBuilder/inspect_client.py");
        const std::string tableVerifier=ReadSource("tools/EnvironmentDatabaseBuilder/verify_live_database.py");
        Require(Contains(inspector, "ed62bd406dbe92cabe4b38759c630df25a8d3b02a2646e1b77f62cb7be273c1d") &&
                Contains(tableVerifier, "if sha not in TABLE_LAYOUT_SHA256") &&
                Contains(tableVerifier, "--executable"),
            "Arden's location fingerprint is inspectable without claiming unverified DBC table globals");
    }
    {
        // The hold defended a confirmed zone against a frame with no world evidence. It
        // must not also become the reason a first location never arrives.
        const std::string health = ReadSource("src/Core/DeviceHealth.h");
        Require(Contains(health, "ShouldHoldLocation(bool worldFrame, bool haveConfirmedLocation)"),
            "the location hold takes whether there is anything to hold");
        Require(Contains(health, "return !worldFrame && haveConfirmedLocation;"),
            "a frame with no world evidence holds only an already-confirmed location");
        const std::string host = ReadSource("ModernWoWRenderer.cpp");
        Require(Contains(host, "renderer::ShouldHoldLocation(worldFrame, environment.Location().valid)"),
            "HookedPresent passes the confirmed-location state into the hold");
    }
    // A captured scene depth is not required to know the world was drawn: it is replaced
    // before some Present calls, so the terrain/liquid observation and the fog capture are
    // OR-ed at the call site. Pin that no single source can veto sampling on its own.
    {
        const std::string host = ReadSource("ModernWoWRenderer.cpp");
        Require(Contains(host, "EnvironmentFogCapture::Instance().HasDepth()||worldGeometrySeenThisFrame") ||
                Contains(host, "renderer::DepthCapture::Instance().HasDepth(), knownWorldDraw"),
            "the world frame is the fog capture OR a known terrain/liquid draw");
    }

    std::printf("ALL LOCATION STATUS TESTS PASSED\n");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "LOCATION STATUS FAILURE: %s\n", error.what());
    return 1;
}
