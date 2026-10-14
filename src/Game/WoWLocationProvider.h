#pragma once
#include "WoWClientContext.h"
#include "../Environment/LocationContext.h"
#include "../Environment/LocationStatus.h"
namespace renderer {
class EnvironmentDatabase;
struct WoWLocationOffsets { uintptr_t map=0,zone=0,area=0; };
class WoWLocationProvider {
public:
    void Configure(const std::wstring& path);
    LocationContext Sample(const EnvironmentDatabase& database,bool worldContext) const;
    // Configured well enough to read the client at all; `Verified` additionally means the
    // addresses were confirmed in-world, which is what unlocks profile effects.
    bool Available() const {return available;}
    bool Verified() const {return available&&verified;}
    const std::string& Status() const {return status;}
    const WoWClientContext& Client() const {return client;}
    const LocationContext& RawSample() const {return rawSample;}
    // True once the three addresses have been read successfully at least once, so the
    // panel can tell "the client says 0 / 0 / 0" from "we never got to read".
    bool Sampled() const {return sampled;}
    // The machine-readable reason a sample was refused, for the F7 header line that has
    // room for a clause rather than a paragraph. Valid until the next Sample().
    LocationFailure Failure() const {return failure;}
    const std::string& ValidationStatus() const {return validation;}
private:
    mutable LocationContext rawSample;
    mutable std::string validation="Not sampled";
    mutable bool sampled=false;
    LocationFailure failure=LocationFailure::NotConfigured;
    WoWClientContext client;
    WoWLocationOffsets offsets;
    bool available=false,verified=false;
    std::string status="Location provider not configured";
};
}
