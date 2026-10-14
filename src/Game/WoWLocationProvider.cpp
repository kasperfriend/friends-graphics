#include "WoWLocationProvider.h"
#include "../Environment/EnvironmentDatabase.h"
#include <limits>
namespace renderer {
void WoWLocationProvider::Configure(const std::wstring& path) {
    available=verified=false;offsets={};rawSample={};sampled=false;validation="Not sampled";
    failure=configureFailure=LocationFailure::NotConfigured;
    if(!client.base&&!client.Detect()) {status="Unsupported executable / PE headers";failure=configureFailure=LocationFailure::UnsupportedExecutable;return;}
    // Only an exact executable fingerprint opts into custom RVAs. The stock
    // 12340 RVAs are documentation, never an automatic unknown-build fallback.
    // Values are trimmed because the Win32 profile API returns them space-for-space as
    // written: a byte-exact compare against a hash carrying one trailing space fails,
    // and a failed compare means no location in any zone, forever, with no typo in sight.
    auto iniValue=[&](const wchar_t* key) {wchar_t value[128]{};GetPrivateProfileStringW(L"LocationProvider",key,L"",value,128,path.c_str());return std::wstring(value);};
    std::string fingerprint=TrimIniValue(iniValue(L"ExeSHA256"));
    for(char& c:fingerprint)if(c>='A'&&c<='F')c=char(c-'A'+'a');
    // A blank shipped configuration uses only this exact, live-verified build.
    // Explicit custom configurations keep their original verification rules.
    const bool bundled=fingerprint.empty()&&client.sha256=="e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6";
    if(!bundled&&(client.sha256.empty()||fingerprint!=client.sha256)) {status="Unrecognized executable fingerprint; Default environment";failure=configureFailure=LocationFailure::FingerprintMismatch;return;}
    auto read=[&](const wchar_t* key)->uintptr_t {uint64_t value=0;return ParseIniUnsigned(iniValue(key),value)&&value<=std::numeric_limits<uintptr_t>::max()?static_cast<uintptr_t>(value):0;};
    offsets=bundled?WoWLocationOffsets{0x7D088C,0x7D080C,0x7D0810}:WoWLocationOffsets{read(L"MapRva"),read(L"ZoneRva"),read(L"AreaRva")};
    uint32_t probe=0;
    if(!offsets.map||!offsets.zone||!offsets.area||offsets.map==offsets.zone||offsets.map==offsets.area||offsets.zone==offsets.area||!client.Read(offsets.map,&probe,4)||!client.Read(offsets.zone,&probe,4)||!client.Read(offsets.area,&probe,4)) {status="Invalid/unreadable location RVAs; Default environment";failure=configureFailure=LocationFailure::UnreadableRvas;return;}
    available=true;verified=bundled||GetPrivateProfileIntW(L"LocationProvider",L"VerifiedInWorld",0,path.c_str())!=0;
    status=bundled?"Supported Ascension build; bundled verified RVAs":verified?"Fingerprint matched; in-world verified RVAs":"Fingerprint matched; diagnostic RVAs (effects locked)";
    // Configured and readable: the only thing missing is a frame that proves the
    // world is on screen. Saying "not configured" here - the value this function
    // started from - was the other half of the always-not-configured report: any
    // session whose world-frame signal never fired (menu, loading, a depth path
    // the hook did not take) kept the startup wording instead of this one.
    failure=configureFailure=LocationFailure::NoWorldContext;
    validation="Configured; no world frame sampled yet";
}
LocationContext WoWLocationProvider::Sample(const EnvironmentDatabase& db,bool world) const {
    LocationContext a,b;
    rawSample={};sampled=false;
    // An unavailable provider keeps the cause Configure() recorded. Naming every
    // such refusal NotConfigured here made F7 read "not configured | not sampled"
    // for an unrecognized executable, unreadable RVAs and an unreadable client
    // image alike - three different fixes collapsed into one wrong instruction.
    if(!available) {validation="Provider unavailable: "+status;failure=configureFailure;return {};}
    if(!world) {validation="No render world context";failure=LocationFailure::NoWorldContext;return {};}
    auto read=[&](LocationContext& v) {return client.Read(offsets.map,&v.mapId,4)&&client.Read(offsets.zone,&v.zoneId,4)&&client.Read(offsets.area,&v.areaId,4);};
    if(!read(a)||!read(b)||a.mapId!=b.mapId||a.zoneId!=b.zoneId||a.areaId!=b.areaId) {validation="Read failed or incoherent snapshot";failure=LocationFailure::ReadFailed;return {};}
    // The read succeeded: these are the ids the client really holds even if the
    // database refuses to vouch for them, which is the case a user needs to see.
    rawSample=a;sampled=true;
    failure=LocationFailure::UnvalidatedIds;
    validation="IDs not validated against database";
    if(!db.Map(a.mapId)||!a.zoneId||a.areaId>1000000||a.zoneId>1000000)return {};
    const auto area=db.Area(a.areaId);const auto zone=db.Area(a.zoneId);
    if(!zone||(area&&area->mapId!=a.mapId)||(!area&&zone->mapId!=a.mapId)) {failure=LocationFailure::HierarchyMismatch;return {};}
    // Unknown custom subareas can still inherit a known zone/map. Known areas
    // must belong to the reported zone, catching most coherent-looking garbage.
    if(area) {
        uint32_t parent=area->id;
        while(parent&&parent!=a.zoneId) {const auto p=db.Area(parent);if(!p) {failure=LocationFailure::HierarchyMismatch;return {};}parent=p->parentId;}
        if(parent!=a.zoneId) {failure=LocationFailure::HierarchyMismatch;return {};}
    }
    a.valid=true;failure=LocationFailure::None;validation=area?"Database hierarchy valid":a.areaId?"Unknown custom area; known zone/map valid":"No subarea; known zone/map valid";return a;
}
}
