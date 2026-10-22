#include "EnvironmentProfileManager.h"
#include "LocationTuning.h"
#include "MenuDiagnostics.h"
#include "../Diagnostics/FileSink.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
namespace renderer {
EnvironmentProfileManager& EnvironmentProfileManager::Instance() {static EnvironmentProfileManager manager;return manager;}
// The location line is written on every CONFIRMED place, which is not the same
// thing as a settled one: this client's place flaps across a border while the
// player walks it, and the settle logic in HookedPresent deliberately applies
// nothing on most of those alternations. This used to open the log, write one
// line and close it again on every one of them - and stat the file for rotation
// first - so the crossings that re-applied nothing still paid two opens in the
// game folder, on the frame the player was walking on. It now buffers into the
// shared sink and goes out in the one open Present already makes.
void EnvironmentProfileManager::Log(const std::string& message) const {if(!basePath.empty())renderer::filesink::AppendLine(basePath+L"ModernWoWRenderer.log","[Environment] "+message);}
void EnvironmentProfileManager::Configure(const std::wstring& base) {
    basePath=base;std::string error;
    menudiagnostics::Configure(base);
    if(!database.Load(base+L"data\\areas.txt",error))Log(error+"; location validation unavailable");
    else Log("Area database loaded: "+std::to_string(database.AreaCount()));
    Reload();
    const auto& client=provider.Client();
    const std::string clientIdentity=
        client.sha256State==WoWClientContext::Sha256State::Computed?"SHA256="+client.sha256:
        client.sha256State==WoWClientContext::Sha256State::Failed?"SHA256 unavailable (hash computation failed)":
        "SHA256 not computed (identified by code signature)";
    Log("Client "+clientIdentity+" timestamp="+std::to_string(client.timestamp));
    WriteMenuReport("configured");
}
void EnvironmentProfileManager::Reload() {
    // F12 must also recover after installing/fixing the location database.
    std::string databaseError;
    if(!database.Load(basePath+L"data\\areas.txt",databaseError))menudiagnostics::Write("DATABASE reload failed: "+databaseError);
    // The provider is configured FIRST and from its own INI section. It used to sit below
    // the EnvironmentProfiles.ini parse, so a client folder without that file - which the
    // install instructions do not require, and which the profile parser rejects outright
    // for one bad key - returned here and never configured the provider. Every zone then
    // read "Location unavailable" with the ids at 0 / 0 / 0, and a typo in an unrelated
    // [Zone.1] value cost the player their location system. Location is not a profile
    // setting; it must come up even when the profile file is missing or unusable.
    provider.Configure(basePath+L"EnvironmentProfiles.ini");
    Log(provider.Status());
    if(!provider.Available())Log("Location unavailable: "+std::string(LocationFailureText(provider.Failure()))+LocationFailureHint(provider.Failure(),database.Authoritative()));
    std::ifstream input{std::filesystem::path(basePath+L"EnvironmentProfiles.ini")};std::ostringstream text;text<<input.rdbuf();
    EnvironmentConfiguration next;std::string error;
    if(!input||!ParseEnvironmentConfiguration(text.str(),next,error)) {Log("Profile reload rejected; retaining previous config: "+(input?error:"EnvironmentProfiles.ini missing/unreadable"));menudiagnostics::Write("CONFIG reload rejected: "+(input?error:"EnvironmentProfiles.ini missing/unreadable"));debounce.Reset();lastPoll=0;Retarget();WriteMenuReport("reload");return;}
    config=std::move(next);
    debounce.Reset();lastPoll=0;
    Retarget();
    WriteMenuReport("reload");
}
void EnvironmentProfileManager::Retarget() {
    resolved=ResolveEnvironment(config,location);
    // The supplied task requires real-client verification before effects are
    // connected. Diagnostics and resolver still work while this gate is closed.
    target=provider.Verified()&&database.Authoritative()?resolved.state:ResolvedEnvironmentState{};
    from=current;blendStart=lastNow;progress=0;
    if(config.debug)Log("Profile resolved: "+resolved.profile+" source="+resolved.source);
}
void EnvironmentProfileManager::Update(uint64_t now,bool world,bool hold) {
    lastNow=now;
    // Enforce the provider's world-context precondition here as well as at the
    // HookedPresent call site. A focused no-depth frame is not a valid sample;
    // otherwise its deliberate invalid return can debounce into clearing the
    // last verified zone and disabling all scoped water settings. It must not
    // age a pending candidate either, so restart that short debounce on resume.
    if(!world||hold) debounce.Reset();
    if(world&&!hold&&(!lastPoll||now-lastPoll>=125)) {
        lastPoll=now;const auto sample=provider.Sample(database,world);
        // Invalid world context uses the same confirmation discipline: brief
        // particle/camera gaps cannot oscillate the environment.
        if(debounce.Confirm(sample,location,now,config.debounceMs)) {
            const auto revision=location.revision+1;location=sample;location.revision=revision;
            // The invalid branch is the line a supporter needs, so it names the cause and
            // the raw ids: "unrecognized executable" and "ids fail area database" are
            // two different failures with two different fixes, and both used to log as
            // one identical "Location unavailable; Default environment" line.
            Log(location.valid?"Location changed: Map="+std::to_string(location.mapId)+" Zone="+std::to_string(location.zoneId)+" Area="+std::to_string(location.areaId)
                :"Location unavailable: "+std::string(LocationFailureText(provider.Failure()))+" ("+provider.ValidationStatus()+"), ids "+LocationIdsText(provider.Sampled(),provider.RawSample().mapId,provider.RawSample().zoneId,provider.RawSample().areaId));Retarget();
            WriteMenuReport("location changed");
        }
    }
    progress=std::clamp(float(now-blendStart)/(config.transitionSeconds*1000),0.f,1.f);
    current=BlendEnvironment(from,target,progress);
}
WorldSceneObservation EnvironmentProfileManager::ObserveWorldScene(bool worldContext,bool knownWorldDrawSeen) const {
    // Unlike the debounced profile location, this is a render-safety sample: a
    // coherent all-zero tuple must close world-anchored effects immediately on
    // the login screen, not after the profile's user-facing debounce interval.
    if(!worldContext)
        return ClassifyWorldScene(provider.Verified(),false,0,0,0,knownWorldDrawSeen);
    provider.Sample(database,true);
    const auto& raw=provider.RawSample();
    return ClassifyWorldScene(provider.Verified(),provider.Sampled(),
        raw.mapId,raw.zoneId,raw.areaId,knownWorldDrawSeen);
}
std::vector<std::string> EnvironmentProfileManager::DebugLines() const {
    std::vector<std::string> lines={"LOCATION / ENVIRONMENT",provider.Status()};
    if(location.valid) {
        auto name=[&](uint32_t id) {const auto a=database.Area(id);return a?a->name:std::string("Unknown custom area");};
        const auto map=database.Map(location.mapId);
        lines.push_back("MapID: "+std::to_string(location.mapId)+"  "+(map?*map:"Unknown"));
        lines.push_back("ZoneID: "+std::to_string(location.zoneId)+"  "+name(location.zoneId));
        lines.push_back("AreaID: "+std::to_string(location.areaId)+"  "+name(location.areaId));
    } else {
        const auto& raw=provider.RawSample();
        const auto reason=provider.Failure();
        lines.push_back("Location unavailable ("+std::string(LocationFailureText(reason))+") - Environment: Default");
        const auto hint=LocationFailureHint(reason,database.Authoritative());
        // The hint carries a " - " separator for the log line it was written to sit on;
        // here it starts a line of its own, so the separator goes away.
        if(!hint.empty())lines.push_back("Try: "+hint.substr(hint.find_first_not_of(" -")));
        lines.push_back("Raw Map/Zone/Area: "+LocationIdsText(provider.Sampled(),raw.mapId,raw.zoneId,raw.areaId));
    }
    lines.push_back("Validation: "+provider.ValidationStatus());
    lines.push_back("Profile: "+resolved.profile+"  Source: "+resolved.source);
    lines.push_back("Transition: "+std::to_string(int(progress*100))+"%  "+(provider.Verified()&&database.Authoritative()?"Effects verified":"Effects locked: verify RVAs and database provenance"));
    lines.push_back("Create Area Override / Create Zone Override: experimental API (UI disabled)");
    return lines;
}
std::vector<std::string> EnvironmentProfileManager::CompactDebugLines() const {
 const auto& raw=location.valid?location:provider.RawSample();
 auto name=[&](uint32_t id){if(!id)return std::string("None (zone fallback)");const auto area=database.Area(id);return area?area->name:std::string("Unknown");};
 const auto map=database.Map(raw.mapId);
 std::vector<std::string> lines;
 if(location.valid)lines.push_back("Map "+std::to_string(raw.mapId)+" "+(map?*map:"Unknown")+"   |   Zone "+std::to_string(raw.zoneId)+" "+name(raw.zoneId)+"   |   Area "+std::to_string(raw.areaId)+" "+name(raw.areaId));
 else lines.push_back("Location unavailable: "+std::string(LocationFailureText(provider.Failure()))+"  |  Map/Zone/Area: "+LocationIdsText(provider.Sampled(),raw.mapId,raw.zoneId,raw.areaId));
 lines.push_back("Profile: "+resolved.profile+"  |  Source: "+resolved.source+"  |  Transition: "+std::to_string(int(progress*100))+"%");
 lines.push_back("Location: "+provider.ValidationStatus()+"  |  "+provider.Status());
 lines.push_back(provider.Verified()&&database.Authoritative()&&location.valid?"Automatic environment active":"Automatic environment: pending location / database verification");
 return lines;
}
std::string EnvironmentProfileManager::TuningBlockedReason() const {
    if(!provider.Verified())return provider.Status();
    if(!database.Authoritative())return database.AreaCount()?"Area database is diagnostic-only":"Missing/invalid data/areas.txt";
    if(!location.valid)return provider.ValidationStatus();
    return {};
}
void EnvironmentProfileManager::WriteMenuReport(const std::string& event) const {
    // The whole report goes out in one file open: this runs on every zone
    // change, and a dozen open/write/close cycles per border crossing is I/O
    // the player pays for with frame time.
    std::vector<std::string> lines;
    lines.push_back("REPORT "+event+" SHA256="+provider.Client().sha256+" areas="+std::to_string(database.AreaCount())+" authoritative="+std::to_string(database.Authoritative()));
    lines.push_back("Local settings: "+(TuningBlockedReason().empty()?std::string("editable"):TuningBlockedReason()));
    const auto path=basePath+L"EnvironmentProfiles.ini";
    auto utf8=[](const wchar_t* text) {
        const int size=WideCharToMultiByte(CP_UTF8,0,text,int(wcslen(text)),nullptr,0,nullptr,nullptr);
        std::string result(size,'\0');
        if(size)WideCharToMultiByte(CP_UTF8,0,text,int(wcslen(text)),result.data(),size,nullptr,nullptr);
        return result;
    };
    for(const wchar_t* key:{L"ExeSHA256",L"MapRva",L"ZoneRva",L"AreaRva",L"VerifiedInWorld"}) {
        std::wstring value; if(!renderer::locationtuning::ReadCachedString(path.c_str(),L"LocationProvider",key,value)||value.empty()) value=L"<missing>";
        lines.push_back("CONFIG "+utf8(key)+"="+utf8(value.c_str()));
    }
    for(const auto& line:CompactDebugLines())lines.push_back(line);
    menudiagnostics::WriteBatch(lines);
}
bool EnvironmentProfileManager::CreateOverride(bool area) {
    if(!location.valid)return false;
    const auto section=std::wstring(area?L"Area.":L"Zone.")+std::to_wstring(area?location.areaId:location.zoneId);
    const auto path=basePath+L"EnvironmentProfiles.ini";
    wchar_t existing[4096]{};if(GetPrivateProfileSectionW(section.c_str(),existing,4096,path.c_str()))return false;
    if(!WritePrivateProfileStringW(section.c_str(),L"Profile",L"Default",path.c_str()))return false;
    Reload();return true;
}
}
