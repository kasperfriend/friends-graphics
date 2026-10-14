#pragma once
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <string>
namespace renderer {
    // Why the location is not usable, as a value rather than as prose.
    //
    // The F7 header renders exactly four environment lines (`for i < 4` in
    // TuningOverlay.h), so a fifth line that explains a failure is a line that is never
    // drawn, and a player reading "Location unavailable | Raw Map / Zone / Area:
    // 0 / 0 / 0" in a game they are standing in learns nothing: the ids look like the
    // world's own zeros and the cause is off in a log file. So the cause travels as an
    // enum from the provider, and the panel line carries it.
    //
    // The order of the causes is the order a reader should try them in: first the
    // provider was never told where to read, then the client is not a build this DLL has
    // researched, then the addresses it was given cannot be read, then the values read do
    // not survive the area database. The last two are the ones that look like a bug in the
    // renderer and are usually an installation problem, which is why both carry a hint.
    enum class LocationFailure : uint8_t {
        None = 0,            // a location was resolved
        NotConfigured,       // Configure() never ran: EnvironmentProfiles.ini missing or rejected
        UnsupportedExecutable, // the main module is not a PE this DLL can identify
        FingerprintMismatch, // a real executable, but not one whose location addresses are known
        UnreadableRvas,      // configured or bundled addresses are outside/inside-unreadable memory
        NoWorldContext,      // no render frame that proves the world is on screen
        ReadFailed,          // the two snapshots did not both read, or did not agree
        UnvalidatedIds,      // read fine; the area database cannot vouch for them
        HierarchyMismatch    // read fine; the ids contradict the map/zone/area hierarchy
    };

    // The panel text. The overlay draws this at eight pixels per glyph across a 700 px
    // panel without wrapping, and line 1 also has to carry the ids, so a clause has to
    // stay inside ~27 characters: "Location unavailable: " + clause + "  |  Map/Zone/Area: "
    // + ids is the whole line. Anything longer is silently chopped off the panel, which
    // is how a precise diagnosis becomes no diagnosis.
    inline const char* LocationFailureText(LocationFailure failure) noexcept
    {
        switch (failure) {
        case LocationFailure::None: return "resolved";
        case LocationFailure::NotConfigured: return "not configured";
        case LocationFailure::UnsupportedExecutable: return "client image unreadable";
        case LocationFailure::FingerprintMismatch: return "unrecognized executable";
        case LocationFailure::UnreadableRvas: return "RVAs unreadable";
        case LocationFailure::NoWorldContext: return "no world frame yet";
        case LocationFailure::ReadFailed: return "client read failed";
        case LocationFailure::UnvalidatedIds: return "ids fail area database";
        case LocationFailure::HierarchyMismatch: return "map/zone/area disagree";
        }
        return "unknown";
    }

    // What to DO about it, for the two causes that are an installation rather than a
    // research problem. Both of them make an otherwise working renderer report no
    // location in any zone, which is indistinguishable from "this build is unsupported"
    // unless the panel says which file is missing.
    inline std::string LocationFailureHint(LocationFailure failure, bool databaseLoaded)
    {
        if (failure == LocationFailure::NotConfigured)
            return " - EnvironmentProfiles.ini is missing or was rejected; copy it next to d3d9.dll (F12 retries)";
        if (failure == LocationFailure::UnvalidatedIds || failure == LocationFailure::HierarchyMismatch)
            return databaseLoaded ? std::string()
                : " - data\\areas.txt is missing or invalid; location ids cannot be checked without it";
        if (failure == LocationFailure::FingerprintMismatch)
            return " - this client build has no verified location addresses; see [LocationProvider]";
        return {};
    }

    // The ids a player is actually standing in, or the honest absence of them. Zero is a
    // real map id (Eastern Kingdoms) and a real "no subarea", so an un-sampled provider
    // must not print it: "0 / 0 / 0" reads as a game reporting the wrong place, when the
    // renderer never asked.
    inline std::string LocationIdsText(bool sampled, uint32_t mapId, uint32_t zoneId, uint32_t areaId)
    {
        if (!sampled) return "not sampled";
        return std::to_string(mapId) + " / " + std::to_string(zoneId) + " / " + std::to_string(areaId);
    }

    // `GetPrivateProfileStringW` returns the value exactly as written, spaces included.
    // That used to be fatal here: the shipped `[LocationProvider] ExeSHA256` compare is
    // byte-exact, so a pasted hash with one trailing space turned every zone into
    // "unrecognized executable", and the RVA parse rejected anything after the digits, so
    // `MapRva = 12340 ` disabled a correct configuration. Trim first, then parse, and
    // still refuse trailing junk so a typo cannot silently read the wrong address.
    inline std::string TrimIniValue(const std::wstring& value)
    {
        size_t begin = 0, end = value.size();
        auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (begin < end && space(value[begin])) ++begin;
        while (end > begin && space(value[end - 1])) --end;
        std::string out;
        out.reserve(end - begin);
        // Keys here are hashes, decimal/hex addresses and flags: all ASCII. Non-ASCII is
        // folded to '?' so a mojibake value fails the compare instead of matching it.
        for (size_t i = begin; i < end; ++i) out += value[i] > 0x7f ? '?' : char(value[i]);
        return out;
    }

    inline bool ParseIniUnsigned(const std::wstring& value, uint64_t& out)
    {
        const std::string text = TrimIniValue(value);
        if (text.empty()) return false;
        errno = 0;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
        if (errno || end == text.c_str() || !end || *end) return false;
        out = parsed;
        return true;
    }
}
