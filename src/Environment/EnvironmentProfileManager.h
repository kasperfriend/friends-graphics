#pragma once
#include "EnvironmentProfile.h"
#include "EnvironmentDatabase.h"
#include "../Core/DeviceHealth.h"
#include "../Game/WoWLocationProvider.h"
#include <vector>
namespace renderer {
class EnvironmentProfileManager {
public:
    static EnvironmentProfileManager& Instance();
    void Configure(const std::wstring& base);
    void Reload();
    // hold keeps the last confirmed location (focus or device gaps); blending still advances.
    void Update(uint64_t now,bool worldContext,bool holdLocation=false);
    // Fresh, non-debounced scene presence for rendering gates. Verified client
    // IDs distinguish a playable world from a login scene; unknown clients fall
    // back to an observed terrain/liquid draw without weakening location tuning.
    WorldSceneObservation ObserveWorldScene(bool worldContext,bool knownWorldDrawSeen) const;
    const ResolvedEnvironmentState& State() const {return current;}
    const LocationContext& Location() const {return location;}
    bool ReadCameraPose(ClientCameraPoseSnapshot& pose) const {return provider.ReadCameraPose(pose);}
    bool LocationTuningVerified() const {return provider.Verified()&&database.Authoritative();}
    std::string ZoneName() const {const auto* area=database.Area(location.zoneId);return area?area->name:std::to_string(location.zoneId);}
    std::string AreaName() const {const auto* area=database.Area(location.areaId);return area?area->name:std::to_string(location.areaId);}
    std::vector<std::string> DebugLines() const;
    std::vector<std::string> CompactDebugLines() const;
    std::string TuningBlockedReason() const;
    void WriteMenuReport(const std::string& event) const;
    bool CreateOverride(bool area);
private:
    void Retarget();
    void Log(const std::string& message) const;
    std::wstring basePath;
    EnvironmentDatabase database;
    WoWLocationProvider provider;
    EnvironmentConfiguration config;
    EnvironmentResolution resolved;
    ResolvedEnvironmentState current,from,target;
    LocationContext location;
    LocationDebouncer debounce;
    uint64_t lastPoll=0,blendStart=0,lastNow=0;
    float progress=1;
};
}
