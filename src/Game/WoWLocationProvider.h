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
    // Reads the active 3.3.5a world camera only for a verified supported
    // client. The caller must still require current in-world/orientation
    // evidence and pass the eye through CameraContinuityGate before use.
    bool ReadCameraEye(float position[3]) const;
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
    // Set from Sample(), which is const: the refusal belongs to the reading, not to the
    // configuration, and the panel reads it whenever it draws. Same reason rawSample and
    // validation are already mutable.
    mutable LocationFailure failure=LocationFailure::NotConfigured;
    // The refusal Configure() itself reached, kept separately because Sample() runs
    // every poll and must not rename it: a client whose fingerprint matches nothing
    // is "unrecognized executable" forever, and letting the first world-frame sample
    // overwrite that with NotConfigured sent players to check a file that was never
    // the problem (and made the log's real cause unreachable from the panel).
    LocationFailure configureFailure=LocationFailure::NotConfigured;
    WoWClientContext client;
    WoWLocationOffsets offsets;
    bool available=false,verified=false,cameraLayoutSupported=false;
    std::string status="Location provider not configured";
};
}
