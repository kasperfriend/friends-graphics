#include "WoWLocationProvider.h"
#include "../Environment/LocationTuning.h"
#include "../Environment/EnvironmentDatabase.h"
#include "../D3D9/CameraContinuity.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
namespace renderer {
namespace {
bool HasVerified12340CodeSignature(const WoWClientContext& client) {
    if(client.base!=0x00400000)return false;
    constexpr uint8_t mapSig[7]={0xA1,0x8C,0x08,0xBD,0x00,0x8B,0x0D};
    constexpr uint8_t areaSig[7]={0xA1,0x10,0x08,0xBD,0x00,0x8B,0x0D};
    constexpr uint8_t writeSig[11]={0x89,0x35,0x0C,0x08,0xBD,0x00,0xA3,0x10,0x08,0xBD,0x00};
    uint8_t buf[11]{};
    return client.Read(0x119640,buf,sizeof(mapSig))&&std::memcmp(buf,mapSig,sizeof(mapSig))==0&&
           client.Read(0x119CA0,buf,sizeof(areaSig))&&std::memcmp(buf,areaSig,sizeof(areaSig))==0&&
           client.Read(0x1204E4,buf,sizeof(writeSig))&&std::memcmp(buf,writeSig,sizeof(writeSig))==0;
}
bool IsKnownVerified12340Client(const WoWClientContext& client) {
    // The code signature is three small reads; the hash comparisons are free
    // only once the hash exists, and computing it is the expensive step.
    // Answer from the signature whenever it can.
    return HasVerified12340CodeSignature(client)||
           client.sha256=="e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6"||
           client.sha256=="f4b9f6fce448194638c5b1c751483090a48c597c6272237f31b3d151b50d3114"||
           client.sha256=="57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da"||
           client.sha256=="ed62bd406dbe92cabe4b38759c630df25a8d3b02a2646e1b77f62cb7be273c1d";
}

// WoW 3.3.5a (12340) active-camera pointer chain: *(WorldFrame global) ->
// CGWorldFrame, CGCamera at +0x7E20, with the existing position at +0x08.
// Candidate orientation fields are forward/right/up at +0x14/+0x20/+0x2C;
// the candidate row-major world->view global is VA 0x00ADF5E8. The new
// orientation/matrix offsets are not independently verified against Arden;
// they remain supported-client-gated and fail closed on the cross-checks.
constexpr uintptr_t kWorldFramePointerRva=0x77436C; // VA 0x00B7436C at image base 0x00400000
constexpr uintptr_t kWorldFrameCameraOffset=0x7E20;
constexpr uintptr_t kCameraPositionOffset=0x08;
constexpr uintptr_t kCameraPoseFieldsOffset=kCameraPositionOffset;
constexpr uintptr_t kCameraForwardOffset=0x14;
constexpr uintptr_t kCameraRightOffset=0x20;
constexpr uintptr_t kCameraUpOffset=0x2C;
constexpr std::size_t kCameraForwardFieldIndex=(kCameraForwardOffset-kCameraPoseFieldsOffset)/sizeof(float);
constexpr std::size_t kCameraRightFieldIndex=(kCameraRightOffset-kCameraPoseFieldsOffset)/sizeof(float);
constexpr std::size_t kCameraUpFieldIndex=(kCameraUpOffset-kCameraPoseFieldsOffset)/sizeof(float);
static_assert(kCameraForwardFieldIndex==3&&kCameraRightFieldIndex==6&&kCameraUpFieldIndex==9);
constexpr uintptr_t kWorldViewMatrixRva=0x6DF5E8;
constexpr uint32_t kMinimumClientPointer=0x10000;
constexpr float kCameraEyeSnapshotDistanceSquared=.25f;
constexpr float kCameraPoseFieldTolerance=.002f;
constexpr float kCameraViewSnapshotTolerance=.002f;
constexpr float kCameraViewEyeLeashYards=5.f;

bool IsPlausibleClientPointer(uint32_t pointer) {
    return pointer>=kMinimumClientPointer&&(pointer&3u)==0;
}

bool IsPlausibleWorldPosition(const float position[3]) {
    for(int axis=0;axis<3;++axis)
        if(!std::isfinite(position[axis])||std::fabs(position[axis])>1000000.f)return false;
    return true;
}
// Stock 3.3.5a (build 12340) parent zones for areas that Ascension promoted to
// standalone starter roots (10138..10146) or unparented caves in data/areas.txt.
uint32_t StockParentZone(uint32_t areaId) {
    switch(areaId) {
        case 10138: return 12;   // Northshire -> Elwynn Forest
        case 10139: return 1;    // Coldridge Valley -> Dun Morogh
        case 10140: return 85;   // Deathknell -> Tirisfal Glades
        case 10141: return 3430; // Sunstrider Isle -> Eversong Woods
        case 10142:
        case 3569:  return 3524; // Ammen Vale, Tides' Hollow -> Azuremyst Isle
        case 10143:
        case 257:
        case 258:
        case 262:   return 141;  // Shadowglen, Teldrassil caves -> Teldrassil
        case 10144:
        case 365:
        case 371:   return 14;   // Valley of Trials, Durotar caves -> Durotar
        case 10146: return 215;  // Red Cloud Mesa -> Mulgore
        case 3446:  return 1377; // Twilight's Run -> Silithus
        default:    return 0;
    }
}
}
void WoWLocationProvider::Configure(const std::wstring& path) {
    available=verified=cameraLayoutSupported=false;offsets={};rawSample={};sampled=false;validation="Not sampled";
    failure=configureFailure=LocationFailure::NotConfigured;
    if(!client.base&&!client.Detect()) {status="Unsupported executable / PE headers";failure=configureFailure=LocationFailure::UnsupportedExecutable;return;}
    // Only an exact executable fingerprint (or verified 12340 client signature)
    // opts into the verified 12340 RVAs; arbitrary unknown executables are rejected.
    // Values are trimmed because the Win32 profile API returns them space-for-space as
    // written: a byte-exact compare against a hash carrying one trailing space fails,
    // and a failed compare means no location in any zone, forever, with no typo in sight.
    auto iniValue=[&](const wchar_t* key) {std::wstring value;renderer::locationtuning::ReadCachedString(path.c_str(),L"LocationProvider",key,value);return value;};
    std::string fingerprint=TrimIniValue(iniValue(L"ExeSHA256"));
    for(char& c:fingerprint)if(c>='A'&&c<='F')c=char(c-'A'+'a');
    // Hashing the whole executable (hundreds of MB) is the most expensive
    // startup step, and most configurations never need the hash: a verified
    // 12340 code signature identifies the stock client with three small
    // reads, and a blank fingerprint has nothing to compare a hash against.
    // When the hash IS needed (modded client or explicit fingerprint) it is
    // cached on disk keyed by the module's PE identity, so a repeat launch of
    // the same client reads a few dozen bytes instead of re-hashing.
    if(!HasVerified12340CodeSignature(client)) {
        std::filesystem::path dir = std::filesystem::path{path}.parent_path();
        if(dir.empty())dir=L".";
        client.EnsureSha256((dir/L"ClientSha256.cache").wstring());
    }
    // A blank shipped configuration trusts the supported Ascension, ArdenWoW,
    // and stock 3.3.5a (build 12340) fingerprints. Explicit custom configurations
    // keep their rules; Arden's supplied fingerprint uses the shared 12340 location RVAs.
    const bool knownClient=IsKnownVerified12340Client(client);
    cameraLayoutSupported=knownClient&&client.base==0x00400000;
    const bool bundled=fingerprint.empty()&&knownClient;
    if(!bundled&&(client.sha256.empty()||fingerprint!=client.sha256)) {status="Unrecognized executable fingerprint; Default environment";failure=configureFailure=LocationFailure::FingerprintMismatch;return;}
    auto read=[&](const wchar_t* key)->uintptr_t {uint64_t value=0;return ParseIniUnsigned(iniValue(key),value)&&value<=std::numeric_limits<uintptr_t>::max()?static_cast<uintptr_t>(value):0;};
    const WoWLocationOffsets configuredOffsets{read(L"MapRva"),read(L"ZoneRva"),read(L"AreaRva")};
    const bool useDefaultOffsets=bundled||(knownClient&&!configuredOffsets.map&&!configuredOffsets.zone&&!configuredOffsets.area);
    offsets=useDefaultOffsets?WoWLocationOffsets{0x7D088C,0x7D080C,0x7D0810}:configuredOffsets;
    uint32_t probe=0;
    if(!offsets.map||!offsets.zone||!offsets.area||offsets.map==offsets.zone||offsets.map==offsets.area||offsets.zone==offsets.area||!client.Read(offsets.map,&probe,4)||!client.Read(offsets.zone,&probe,4)||!client.Read(offsets.area,&probe,4)) {status="Invalid/unreadable location RVAs; Default environment";failure=configureFailure=LocationFailure::UnreadableRvas;return;}
    const bool knownVerifiedOffsets=knownClient&&offsets.map==0x7D088C&&offsets.zone==0x7D080C&&offsets.area==0x7D0810;
    available=true;verified=bundled||knownVerifiedOffsets||renderer::locationtuning::ReadCachedInt(path.c_str(),L"LocationProvider",L"VerifiedInWorld",0)!=0;
    status=bundled?"Supported 3.3.5a build; bundled verified RVAs":verified?"Fingerprint matched; in-world verified RVAs":"Fingerprint matched; diagnostic RVAs (effects locked)";
    // Configured and readable: the only thing missing is a frame that proves the
    // world is on screen. Saying "not configured" here - the value this function
    // started from - was the other half of the always-not-configured report: any
    // session whose world-frame signal never fired (menu, loading, a depth path
    // the hook did not take) kept the startup wording instead of this one.
    failure=configureFailure=LocationFailure::NoWorldContext;
    validation="Configured; no world frame sampled yet";
}
bool WoWLocationProvider::ReadCameraPose(ClientCameraPoseSnapshot& pose) const {
    pose={};
    // The camera layout is supported only for the verified 12340 client family;
    // a configurable location RVA must never authorize unrelated pointer reads.
    if(!available||!verified||!cameraLayoutSupported)return false;

    uint32_t worldFrame=0,camera=0;
    if(!client.Read(kWorldFramePointerRva,&worldFrame,sizeof(worldFrame))||
       !IsPlausibleClientPointer(worldFrame)||
       worldFrame>std::numeric_limits<uintptr_t>::max()-kWorldFrameCameraOffset||
       !client.ReadAddress(uintptr_t(worldFrame)+kWorldFrameCameraOffset,&camera,sizeof(camera))||
       !IsPlausibleClientPointer(camera)||
       camera>std::numeric_limits<uintptr_t>::max()-kCameraPoseFieldsOffset)return false;

    float firstFields[12]{},secondFields[12]{};
    float firstView[16]{},secondView[16]{};
    const uintptr_t fieldsAddress=uintptr_t(camera)+kCameraPoseFieldsOffset;
    if(!client.ReadAddress(fieldsAddress,firstFields,sizeof(firstFields))||
       !client.Read(kWorldViewMatrixRva,firstView,sizeof(firstView))||
       !client.ReadAddress(fieldsAddress,secondFields,sizeof(secondFields))||
       !client.Read(kWorldViewMatrixRva,secondView,sizeof(secondView))||
       !IsPlausibleWorldPosition(firstFields)||!IsPlausibleWorldPosition(secondFields))return false;

    // The eye and all three orientation axes must come from a stable camera
    // object snapshot. A mixed eye/basis during a turn is not a usable pose.
    const float dx=secondFields[0]-firstFields[0];
    const float dy=secondFields[1]-firstFields[1];
    const float dz=secondFields[2]-firstFields[2];
    if(dx*dx+dy*dy+dz*dz>kCameraEyeSnapshotDistanceSquared)return false;
    for(int i=3;i<12;++i)
        if(!std::isfinite(firstFields[i])||!std::isfinite(secondFields[i])||
           std::fabs(secondFields[i]-firstFields[i])>kCameraPoseFieldTolerance)return false;
    for(int i=0;i<16;++i)
        if(!std::isfinite(firstView[i])||!std::isfinite(secondView[i])||
           std::fabs(secondView[i]-firstView[i])>kCameraViewSnapshotTolerance)return false;

    // The live world->view matrix is the orientation source, not D3DTS_VIEW
    // left over from a sky billboard. Require a standard affine view matrix and
    // cross-check its right/up/forward columns against the same active CGCamera
    // object and its translation against that object's current position.
    if(std::fabs(firstView[3])>.001f||std::fabs(firstView[7])>.001f||
       std::fabs(firstView[11])>.001f||std::fabs(firstView[15]-1.f)>.001f)return false;
    float basis[3][4]{};
    for(int row=0;row<3;++row){
        for(int col=0;col<3;++col)basis[row][col]=firstView[row*4+col];
        basis[row][3]=0.f;
    }
    if(!IsOrthonormalViewBasis(basis,.002f))return false;
    for(int axis=0;axis<3;++axis){
        if(std::fabs(firstFields[kCameraRightFieldIndex+axis]-firstView[axis*4+0])>kCameraPoseFieldTolerance||
           std::fabs(firstFields[kCameraUpFieldIndex+axis]-firstView[axis*4+1])>kCameraPoseFieldTolerance||
           std::fabs(firstFields[kCameraForwardFieldIndex+axis]-firstView[axis*4+2])>kCameraPoseFieldTolerance)return false;
    }
    if(!ViewTranslationMatchesCameraEye(firstView+12,basis,secondFields,
                                         kCameraViewEyeLeashYards))return false;

    // Do not publish a mixed pointer-chain snapshot if the client replaced its
    // active world frame/camera during the read.
    uint32_t worldFrameAfter=0,cameraAfter=0;
    if(!client.Read(kWorldFramePointerRva,&worldFrameAfter,sizeof(worldFrameAfter))||
       worldFrameAfter!=worldFrame||
       !client.ReadAddress(uintptr_t(worldFrameAfter)+kWorldFrameCameraOffset,
                           &cameraAfter,sizeof(cameraAfter))||cameraAfter!=camera)return false;

    std::copy(secondFields,secondFields+3,pose.position);
    for(int row=0;row<3;++row)
        std::copy(basis[row],basis[row]+4,pose.viewBasis[row]);
    return true;
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
    if(!a.zoneId&&a.areaId&&a.areaId<=1000000) {
        if(const auto* fromArea=db.Area(a.areaId);fromArea&&fromArea->mapId==a.mapId) {
            uint32_t root=fromArea->id;
            while(const auto* p=db.Area(root)) {if(!p->parentId)break;root=p->parentId;}
            a.zoneId=root;
        }
    }
    if(!db.Map(a.mapId)||!a.zoneId||a.areaId>1000000||a.zoneId>1000000)return {};
    const auto area=db.Area(a.areaId);const auto zone=db.Area(a.zoneId);
    if(!zone||(area&&area->mapId!=a.mapId)||(!area&&zone->mapId!=a.mapId)) {failure=LocationFailure::HierarchyMismatch;return {};}
    // Unknown custom subareas can still inherit a known zone/map. Known areas
    // must belong to the reported zone, catching most coherent-looking garbage.
    if(area) {
        uint32_t parent=area->id;
        while(parent&&parent!=a.zoneId) {const auto p=db.Area(parent);if(!p) {failure=LocationFailure::HierarchyMismatch;return {};}parent=p->parentId?p->parentId:StockParentZone(parent);}
        if(parent!=a.zoneId) {failure=LocationFailure::HierarchyMismatch;return {};}
    }
    a.valid=true;failure=LocationFailure::None;validation=area?"Database hierarchy valid":a.areaId?"Unknown custom area; known zone/map valid":"No subarea; known zone/map valid";return a;
}
}
