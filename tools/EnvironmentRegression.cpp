#include "../src/Environment/EnvironmentProfileManager.h"
#include "../src/Environment/LocationTuning.h"
#include "../src/Core/DeviceHealth.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cmath>
using namespace renderer;
void Require(bool v,const char* message) {if(!v)throw std::runtime_error(message);std::cout<<"PASS "<<message<<'\n';}
bool Near(float a,float b){return std::abs(a-b)<.0001f;}
static uint32_t testMap=0,testZone=12,testArea=87;
// Called with the existing zone/subarea fixtures in place.
void TestGlobalOverrides(const std::wstring& base) {
 using namespace locationtuning;
 Sync(base,{571,65,4164,true},true);
 const auto path=baseFile;
 const auto runtime=[&]{return ReadInt(L"Atmosphere",L"ShaftPercent",0,path.c_str());};
 editGlobal=true;
 Require(ReadEditorInt(L"Atmosphere",L"ShaftPercent",0,path.c_str())==60,"untouched Global editor shows baseline, not current subarea");
 Require(runtime()==170&&!HasGlobalValue(L"Atmosphere",L"ShaftPercent"),"opening Global does not create overrides");
 Require(Save(L"Atmosphere",L"ShaftPercent",0)&&runtime()==0,"explicit zero Global edit overrides subarea");
 Require(GetPrivateProfileIntW(L"Atmosphere",L"ShaftPercent",0,path.c_str())==60,"Global override preserves baseline file");
 Require(ReadInt(L"Water",L"Enabled",1,path.c_str())==0&&!HasGlobalValue(L"Water",L"Enabled"),"Global edit leaves other keys untouched");
 editGlobal=false;editZone=true;
 Require(ReadEditorInt(L"Atmosphere",L"ShaftPercent",0,path.c_str())==90,"zone editor remains independent of Global mask");
 Require(Save(L"Atmosphere",L"ShaftPercent",95)&&runtime()==0,"zone edit cannot beat Global override");
 editZone=false;
 Require(ReadEditorInt(L"Atmosphere",L"ShaftPercent",0,path.c_str())==170,"area editor remains independent of Global mask");
 Require(Save(L"Atmosphere",L"ShaftPercent",175)&&runtime()==0,"area edit cannot beat Global override");
 Sync(base,{619,4494,4623,true},true);
 Require(runtime()==0,"Global override survives map changes");
 Sync(base,{},false);
 Require(runtime()==0&&ReadEditorInt(L"Atmosphere",L"ShaftPercent",60,path.c_str())==0,"Global override works without location verification");
 Require(Save(L"Atmosphere",L"ShaftPercent",5)&&runtime()==5,"unverified fallback edits existing Global override instead of masked baseline");
 editGlobal=true;
 Require(Save(L"Water",L"WaterColour",0x123456)&&HasScopedValue(L"Water",L"WaterColour"),"Global water colour is a deliberate override even without location");
 Require(ReadInt(L"Water",L"WaterColour",0,path.c_str())==0x123456,"Global water colour reads back");
 Require(Save(L"Atmosphere",L"ShaftPercent",60)&&runtime()==60,"Global edit equal to baseline still overrides location");
 Sync(base,{571,65,4164,true},true);
 Require(runtime()==60,"Global override persists across settings reload");
 Require(ClearGlobalValue(L"Atmosphere",L"ShaftPercent")&&runtime()==175,"clearing Global restores saved subarea without deleting it");
 Sync(base,{571,65,4165,true},true);
 Require(runtime()==95,"clearing Global preserves saved zone value");
 Require(HasGlobalValue(L"Water",L"WaterColour"),"clearing one Global key leaves another intact");
 Require(ClearGlobalValue(L"Water",L"WaterColour")&&!HasScopedValue(L"Water",L"WaterColour"),"clearing Global colour restores automatic zone colours");
 Require(ClearGlobalValue(L"Water",L"WaterColour"),"clearing absent override is harmless");
 editGlobal=false;editZone=false;
}
int main() try {
 const auto tuningDir=std::filesystem::absolute("build/location-tuning-test");std::filesystem::create_directories(tuningDir);
 const auto tuningBase=tuningDir.wstring()+L"\\";
 std::filesystem::remove(tuningDir/L"LocationGraphics.ini");
 {std::ofstream f(tuningDir/L"GraphicsEffects.ini");f<<"[Atmosphere]\nShaftPercent=60\n";}
 locationtuning::Sync(tuningBase,{571,65,4164,true},true);
 Require(locationtuning::Save(L"Atmosphere",L"ShaftPercent",170),"save exact location preset");
 Require(locationtuning::ReadInt(L"Atmosphere",L"ShaftPercent",0,locationtuning::baseFile.c_str())==170,"read saved local value");
 locationtuning::Sync(tuningBase,{619,4494,4623,true},true);
 Require(locationtuning::ReadInt(L"Atmosphere",L"ShaftPercent",0,locationtuning::baseFile.c_str())==60,"instance inherits baseline without outdoor preset leaking");
 Require(locationtuning::Save(L"Atmosphere",L"ShaftPercent",20),"save separate instance preset");
 locationtuning::Sync(tuningBase,{571,65,4164,true},true);
 Require(locationtuning::ReadInt(L"Atmosphere",L"ShaftPercent",0,locationtuning::baseFile.c_str())==170,"return restores original outdoor preset");
 locationtuning::editZone=true;
 Require(locationtuning::Save(L"Atmosphere",L"ShaftPercent",90),"save whole zone while inside a subarea");
 Require(locationtuning::ReadEditorInt(L"Atmosphere",L"ShaftPercent",0,locationtuning::baseFile.c_str())==90,"zone editor shows zone value instead of subarea override");
 Require(locationtuning::ReadInt(L"Atmosphere",L"ShaftPercent",0,locationtuning::baseFile.c_str())==170,"runtime keeps explicit subarea override above zone");
 locationtuning::Sync(tuningBase,{571,65,4165,true},true);
 Require(locationtuning::ReadInt(L"Atmosphere",L"ShaftPercent",0,locationtuning::baseFile.c_str())==90,"sibling subarea inherits whole-zone slider edit");
 locationtuning::editZone=false;
 Require(GetPrivateProfileIntW(L"Atmosphere",L"ShaftPercent",0,locationtuning::baseFile.c_str())==60,"local edits preserve shared graphics file");
 locationtuning::Sync(tuningBase,{530,10127,0,true},true);
 Require(locationtuning::Save(L"Water",L"Enabled",0),"zero area saves zone preset");
 locationtuning::Sync(tuningBase,{530,10127,123,true},true);
 Require(locationtuning::ReadInt(L"Water",L"Enabled",1,locationtuning::baseFile.c_str())==0,"subarea inherits zone override including zero values");
 locationtuning::Sync(tuningBase,{},true);
 Require(locationtuning::Save(L"Water",L"Enabled",1)&&
         locationtuning::ReadInt(L"Water",L"Enabled",0,locationtuning::baseFile.c_str())==1,
         "loading-screen edits save globally without an invalid location preset");
 locationtuning::Sync(tuningBase,{571,65,4164,true},false);
 Require(locationtuning::Save(L"Water",L"Enabled",0)&&
         locationtuning::ReadInt(L"Water",L"Enabled",1,locationtuning::baseFile.c_str())==0&&
         GetPrivateProfileIntW(L"Map.571.Zone.65.Area.4164.Water",L"Enabled",-1,locationtuning::localFile.c_str())==-1,
         "unverified database falls back globally without writing a location preset");
 TestGlobalOverrides(tuningBase);
 // Existing map/zone/subarea color overrides must never affect global grading.
 locationtuning::Sync(tuningBase,{571,65,4164,true},true);
 const wchar_t* colorKeys[]={L"Enabled",L"BrightnessPercent",L"ContrastPercent",L"GammaPercent",L"SharpnessPercent"};
 const int globalValues[]={1,-6,108,80,42};
 for(int k=0;k<5;++k){
  for(unsigned level=0;level<3;++level){const auto section=locationtuning::Scope(level)+L".PostProcess";WritePrivateProfileStringW(section.c_str(),colorKeys[k],L"35",locationtuning::localFile.c_str());}
  locationtuning::editZone=k%2==0;locationtuning::editGlobal=k%3==2;
  Require(locationtuning::Save(L"PostProcess",colorKeys[k],globalValues[k]),"save color globally in all three editor scopes");
  Require(int(locationtuning::ReadInt(L"PostProcess",colorKeys[k],0,locationtuning::baseFile.c_str()))==globalValues[k]&&int(locationtuning::ReadEditorInt(L"PostProcess",colorKeys[k],0,locationtuning::baseFile.c_str()))==globalValues[k],"runtime and editor ignore old color overrides");
  Require(GetPrivateProfileIntW(L"PostProcess",colorKeys[k],0,locationtuning::baseFile.c_str())==globalValues[k],"global color writes shared graphics file");
  Require(GetPrivateProfileIntW(L"Map.571.Zone.65.Area.4164.PostProcess",colorKeys[k],0,locationtuning::localFile.c_str())==35,"legacy local color values remain untouched");
 }
 locationtuning::Sync(tuningBase,{619,4494,4623,true},true);
 Require(int(locationtuning::ReadInt(L"PostProcess",L"BrightnessPercent",0,locationtuning::baseFile.c_str()))==-6,"color remains identical after location change");
 locationtuning::Sync(tuningBase,{},false);
 Require(locationtuning::Save(L"PostProcess",L"Enabled",0)&&locationtuning::ReadInt(L"PostProcess",L"Enabled",1,locationtuning::baseFile.c_str())==0,"global color remains editable without valid local preset");
 locationtuning::editZone=false;locationtuning::editGlobal=false;
 locationtuning::Sync(L"",{},false);
 EnvironmentConfiguration c;std::string error;
 const std::string text="[EnvironmentSystem]\nTransitionSeconds=4\nAreaDebounceMs=300\n[Default]\nFogDensityMultiplier=0.8\nWaterAgitationMultiplier=1.0\n[Environment.Forest]\nRayIntensityMultiplier=1.2\nLocalFogMultiplier=0.7\n[Environment.City]\nProfile=Forest\nLocalFogMultiplier=0.1\n[Map.0]\nFogDensityMultiplier=0.5\nWaterAgitationMultiplier=1.2\n[Zone.12]\nProfile=City\nFogDensityMultiplier=0.6\nWaterAgitationMultiplier=0.7\n[Area.87]\nRayIntensityMultiplier=0.3\nWaterAgitationMultiplier=0.3\n";
 Require(ParseEnvironmentConfiguration(text,c,error),"INI parser accepts complete profile");
 LocationContext l{0,12,87,true};auto r=ResolveEnvironment(c,l);
 Require(Near(r.state[FogDensity],.6f)&&Near(r.state[LocalFog],.1f)&&Near(r.state[Rays],.3f)&&Near(r.state[WaterAgitation],.3f)&&r.source=="Area","Default < Map < Zone < Area preserves area-specific sea state");
 l.areaId=12345;r=ResolveEnvironment(c,l);
 Require(r.source=="Zone"&&Near(r.state[Rays],1.2f)&&Near(r.state[WaterAgitation],.7f),"unknown custom area inherits zone water tuning");
 l.zoneId=12345;r=ResolveEnvironment(c,l);Require(r.source=="Map"&&Near(r.state[FogDensity],.5f)&&Near(r.state[WaterAgitation],1.2f),"unknown zone inherits map water tuning");
 l.valid=false;r=ResolveEnvironment(c,l);Require(r.source=="Default"&&Near(r.state[FogDensity],.8f)&&Near(r.state[WaterAgitation],1.f),"unavailable location uses neutral Default sea state");
 c.enabled=false;r=ResolveEnvironment(c,l);Require(Near(r.state[FogDensity],1),"disabled system is identity");c.enabled=true;
 for(const auto* bad:{"[Default]\nFogDensityMultiplier=nan\n","[Default]\nFogDensityMultiplier=5\n","[Environment.A]\nProfile=B\n[Environment.B]\nProfile=A\n","[Zone.12]\nProfile=Missing\n","[Map.bad]\n","[Default]\nFogDensityMultiplier=1\nFogDensityMultiplier=2\n"}) {
  Require(!ParseEnvironmentConfiguration(bad,c,error),"reject corrupt config transactionally");
 }
 Require(c.named.count("City")==1,"failed reload preserves previous config");
 ResolvedEnvironmentState a,b;b.values[Rays]=3;
 Require(Near(BlendEnvironment(a,b,0)[Rays],1)&&Near(BlendEnvironment(a,b,.5f)[Rays],2)&&Near(BlendEnvironment(a,b,1)[Rays],3),"smoothstep interpolation endpoints and midpoint");
 auto interrupted=BlendEnvironment(a,b,.5f);ResolvedEnvironmentState next;next.values[Rays]=0;
 Require(Near(BlendEnvironment(interrupted,next,0)[Rays],2),"interrupted transition starts at visible current state");
 LocationDebouncer debounce;LocationContext original{0,12,87,true},other{0,12,88,true},invalid;
 Require(!debounce.Confirm(other,original,1000,300)&&!debounce.Confirm(other,original,1125,300)&&!debounce.Confirm(other,original,1250,300)&&debounce.Confirm(other,original,1375,300),"area requires stable 300ms and three samples");
 debounce.Reset();Require(!debounce.Confirm(other,original,1000,300)&&!debounce.Confirm(original,original,1125,300)&&!debounce.Confirm(other,original,1250,300),"border flapping resets candidate timer");
 debounce.Reset();Require(!debounce.Confirm(invalid,original,1000,300)&&!debounce.Confirm(invalid,original,1125,300)&&!debounce.Confirm(original,original,1250,300),"one invalid/loading sample cannot switch profile");
 auto file=std::filesystem::temp_directory_path()/"modern-wow-environment-regression.txt";
 {std::ofstream f(file);f<<"EnvironmentDatabase 1\nM 0 \"Eastern Kingdoms\"\nA 12 0 0 65 \"Elwynn Forest\"\nA 87 0 12 0 \"Goldshire\"\n";}
 EnvironmentDatabase db;Require(db.Load(file.wstring(),error)&&db.AreaCount()==2&&db.Area(87)->parentId==12,"database parser and hierarchy");
 {std::ofstream f(file);f<<"EnvironmentDatabase 1\nM 0 \"Map\"\nA 12 0 87 0 \"Cycle\"\nA 87 0 12 0 \"Cycle\"\n";}
 Require(!db.Load(file.wstring(),error)&&db.Area(87)->name=="Goldshire","database rejects cycles and preserves previous state");std::filesystem::remove(file);
 WoWClientContext client;Require(client.Detect(),"detect x86 test executable");
 // Detect is header-only and never hashes the image (fast load); the provider
 // computes the fingerprint in Configure, so the test asks for it explicitly
 // before writing an INI that references client.sha256.
 client.EnsureSha256(L"");
 uint32_t value=0;
 Require(!client.Read(client.imageSize,&value,4)&&!client.Read(UINTPTR_MAX,&value,4),"bad RVAs fail without crash");
 WoWLocationProvider provider;provider.Configure(L"nonexistent-environment.ini");Require(!provider.Verified()&&!provider.Sample(db,true).valid,"unrecognized build never reads reference offsets");
 Require(provider.Failure()==LocationFailure::FingerprintMismatch&&!provider.Sampled(),
        "a refused sample keeps the configure-time cause instead of renaming it not configured");
 Require(provider.ValidationStatus().find("Unrecognized executable")!=std::string::npos,
        "the validation line names the real provider status after a refused sample");
 const auto providerFile=std::filesystem::temp_directory_path()/"modern-wow-location-regression.ini";
 auto writeProvider=[&](uintptr_t mapRva) {std::ofstream ini(providerFile);ini<<"[LocationProvider]\nExeSHA256="<<client.sha256<<"\nMapRva="<<mapRva<<"\nZoneRva="<<(reinterpret_cast<uintptr_t>(&testZone)-client.base)<<"\nAreaRva="<<(reinterpret_cast<uintptr_t>(&testArea)-client.base)<<"\nVerifiedInWorld=1\n";};
 writeProvider(reinterpret_cast<uintptr_t>(&testMap)-client.base);provider.Configure(providerFile.wstring());
 Require(provider.Failure()==LocationFailure::NoWorldContext,
        "a configured provider waiting on a world frame says so instead of not configured");
 Require(provider.Verified()&&provider.Sample(db,true).valid&&provider.Sample(db,true).areaId==87,"exact-hash provider reads validated live test-process image globals");
 Require(!provider.Sample(db,false).valid&&provider.Failure()==LocationFailure::NoWorldContext,"provider rejects absent world context instead of guessing from stale IDs");
 Require(ShouldHoldLocation(HasWorldContext(false,false),true),"HookedPresent holds a confirmed zone when a no-depth sample would be invalid");
 Require(!ShouldHoldLocation(HasWorldContext(false,false),false),"no confirmed location is never a reason to stop polling");
 Require(!ShouldHoldLocation(HasWorldContext(false,true),true),"a known-draw waves-only frame re-enables verified location sampling");
 testArea=12345;Require(provider.Sample(db,true).valid,"provider accepts unknown custom area with known zone and map");testArea=87;
 testArea=0;Require(provider.Sample(db,true).valid&&provider.Sample(db,true).areaId==0,"zero subarea preserves validated zone/map without inventing AreaID");testArea=87;
 writeProvider(client.imageSize+4);provider.Configure(providerFile.wstring());Require(!provider.Verified()&&!provider.Sample(db,true).valid,"matching hash with invalid configured RVA safely disables provider");
 Require(provider.Failure()==LocationFailure::UnreadableRvas,"unreadable RVAs keep their own cause through a refused sample");std::filesystem::remove(providerFile);
 // Exercise the actual manager gate, not only the ShouldHoldLocation helper:
 // focused frames with neither captured depth nor recognized world draws must
 // never feed the provider's deliberate invalid no-world sample into debounce.
 const auto managerDir=std::filesystem::temp_directory_path()/"modern-wow-location-manager-regression";
 std::filesystem::create_directories(managerDir/"data");
 const auto managerBase=managerDir.wstring()+L"\\";
 {std::ofstream areas(managerDir/"data"/"areas.txt");
  areas<<"EnvironmentDatabase 1\n"
         "# provenance=verified-live-client-tables\n"
         "M 0 \"Eastern Kingdoms\"\n"
         "A 12 0 0 65 \"Elwynn Forest\"\n"
         "A 87 0 12 0 \"Goldshire\"\n";}
 {std::ofstream ini(managerDir/"EnvironmentProfiles.ini");
  ini<<"[EnvironmentSystem]\nTransitionSeconds=4\nAreaDebounceMs=300\n"
        "[LocationProvider]\nExeSHA256="<<client.sha256
     <<"\nMapRva="<<(reinterpret_cast<uintptr_t>(&testMap)-client.base)
     <<"\nZoneRva="<<(reinterpret_cast<uintptr_t>(&testZone)-client.base)
     <<"\nAreaRva="<<(reinterpret_cast<uintptr_t>(&testArea)-client.base)
     <<"\nVerifiedInWorld=1\n[Default]\nWaterAgitationMultiplier=1\n"
        "[Zone.12]\nWaterAgitationMultiplier=.45\n";}
 auto& manager=EnvironmentProfileManager::Instance();manager.Configure(managerBase);
 for(uint64_t now:{1000,1125,1250,1375})manager.Update(now,true,false);
 Require(manager.Location().valid&&manager.Location().zoneId==12,"manager confirms a stable sampled zone before using its profiles");
 manager.Update(5375,true,false);
 Require(Near(manager.State()[WaterAgitation],.45f),"manager resolves the zone sea-state profile");
 for(uint64_t now:{5500,5625,5750,5875})manager.Update(now,false,false);
 Require(manager.Location().valid&&manager.Location().zoneId==12&&Near(manager.State()[WaterAgitation],.45f),"manager itself refuses invalid no-depth samples even if the caller forgets the hold flag");
 testZone=999;manager.Update(6000,true,false); // one invalid world sample starts, but cannot confirm, a new candidate
 manager.Update(6500,false,false);manager.Update(7000,false,false); // no-world time must not age it
 manager.Update(7125,true,false);manager.Update(7250,true,false);
 Require(manager.Location().valid&&manager.Location().zoneId==12,"a menu/loading gap cannot age a pending invalid candidate into clearing zone tuning");
 testZone=12;
 std::filesystem::remove_all(managerDir);
 const auto bundledDatabase=std::filesystem::path("data/areas.txt");
 EnvironmentDatabase bundled;Require(bundled.Load(bundledDatabase.wstring(),error)&&bundled.Authoritative()&&bundled.AreaCount()==2849,"release bundles verified Ascension location database");
 Require(bundled.Area(10221)&&bundled.Area(10221)->parentId==1519,"bundled database includes custom Stormwind area");
 // Point the real provider reader at this test process's known globals and
 // sweep every authoritative root tuple. This proves every profile key is
 // representable as a coherent Map/Zone/Area sample, not just parseable text.
 writeProvider(reinterpret_cast<uintptr_t>(&testMap)-client.base);provider.Configure(providerFile.wstring());
 Require(provider.Verified(),"the synthetic exact-hash provider is enabled for the root-zone read sweep");
 size_t readableRootZones=0;
 for(const auto& [id,area]:bundled.Areas())if(!area.parentId) {
  testMap=area.mapId;testZone=id;testArea=id;
  const auto sample=provider.Sample(bundled,true);
  if(!sample.valid||sample.mapId!=area.mapId||sample.zoneId!=id||sample.areaId!=id)
   throw std::runtime_error("the location provider could not read root zone "+std::to_string(id));
  ++readableRootZones;
 }
 Require(readableRootZones==414,"all 414 authoritative root-zone IDs pass the provider read sweep");
 testMap=0;testZone=12;testArea=9;
 Require(provider.Sample(bundled,true).valid,"stock 3.3.5a Northshire Valley (area 9) under Elwynn Forest (zone 12) passes hierarchy validation");
 testMap=1;testZone=14;testArea=363;
 Require(provider.Sample(bundled,true).valid,"stock 3.3.5a Valley of Trials (area 363) under Durotar (zone 14) passes hierarchy validation");
 testMap=0;testZone=12;testArea=87;std::filesystem::remove(providerFile);
 const auto actual=std::filesystem::path("build/client-research/areas.txt");
 if(std::filesystem::exists(actual)) {EnvironmentDatabase exported;Require(exported.Load(actual.wstring(),error)&&exported.AreaCount()==2849&&!exported.Authoritative(),"real Ascension custom database loads with diagnostic-only gate");}
 std::cout<<"Environment regression passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
