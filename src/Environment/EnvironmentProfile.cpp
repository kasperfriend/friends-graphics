#include "EnvironmentProfile.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <set>
#include <charconv>
#include <vector>
namespace renderer {
namespace {
std::string Trim(std::string s) {auto a=s.find_first_not_of(" \t\r\n");return a==s.npos?"":s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
bool Number(const std::string& s,float& v) {try {size_t n=0;v=std::stof(s,&n);return n==s.size()&&std::isfinite(v);}catch(...) {return false;}}
bool Id(const std::string& s,uint32_t& id) {auto r=std::from_chars(s.data(),s.data()+s.size(),id);return !s.empty()&&r.ec==std::errc()&&r.ptr==s.data()+s.size();}
}
bool ParseEnvironmentConfiguration(const std::string& text,EnvironmentConfiguration& out,std::string& error) {
    EnvironmentConfiguration next;std::istringstream input(text);std::string line,section;EnvironmentProfile* profile=nullptr;
    std::set<std::string> sections,keys;size_t lineNo=0;
    auto fail=[&](const std::string& reason) {error="Line "+std::to_string(lineNo)+": "+reason;return false;};
    while(std::getline(input,line)) {
        ++lineNo;if(lineNo==1&&line.starts_with("\xef\xbb\xbf"))line.erase(0,3);
        line=Trim(line);if(line.empty()||line[0]==';'||line[0]=='#')continue;
        if(line[0]=='[') {
            if(line.back()!=']')return fail("Malformed section");
            section=line.substr(1,line.size()-2);profile=nullptr;keys.clear();
            if(!sections.insert(section).second)return fail("Duplicate section");
            if(section=="Default")profile=&next.defaults;
            else if(section.starts_with("Environment.")) {auto name=section.substr(12);if(name.empty()||name=="Default")return fail("Invalid profile name");profile=&next.named[name];}
            else if(section.starts_with("Map.")||section.starts_with("Zone.")||section.starts_with("Area.")) {
                auto dot=section.find('.');uint32_t id;if(!Id(section.substr(dot+1),id))return fail("Invalid location ID");
                profile=section.starts_with("Map.")?&next.maps[id]:section.starts_with("Zone.")?&next.zones[id]:&next.areas[id];
            } else if(section!="EnvironmentSystem"&&section!="LocationProvider")return fail("Unknown section");
            continue;
        }
        auto equals=line.find('=');if(equals==line.npos||section.empty())return fail("Expected section and key=value");
        auto key=Trim(line.substr(0,equals)),value=Trim(line.substr(equals+1));
        if(!keys.insert(key).second)return fail("Duplicate key");
        if(section=="LocationProvider")continue;
        if(profile) {
            if(key=="Name")continue;
            if(key=="Profile") {profile->profile=value;continue;}
            size_t index=0;for(;index<ParameterCount;++index)if(key==EnvironmentKeys[index])break;
            float number;
            if(index==ParameterCount||!Number(value,number)||number<EnvironmentMinimum[index]||number>EnvironmentMaximum[index])return fail("Unknown environment parameter or out-of-range value");
            profile->values[index]=number;
        } else {
            float number;if(!Number(value,number))return fail("Invalid numeric setting");
            if(key=="Enabled"||key=="Debug") {if(number!=0&&number!=1)return fail("Boolean must be 0 or 1");(key=="Enabled"?next.enabled:next.debug)=number!=0;}
            else if(key=="TransitionSeconds"&&number>=0.1f&&number<=30)next.transitionSeconds=number;
            else if(key=="AreaDebounceMs"&&number>=200&&number<=2000&&std::floor(number)==number)next.debounceMs=static_cast<uint32_t>(number);
            else return fail("Unknown/out-of-range system setting");
        }
    }
    // Named inheritance is supported, but cycles/missing names invalidate the
    // whole reload so a typo cannot partly replace the active configuration.
    auto check=[&](const EnvironmentProfile& p) {
        std::set<std::string> seen;std::string name=p.profile;
        while(!name.empty()&&name!="Default") {auto i=next.named.find(name);if(i==next.named.end()||!seen.insert(name).second)return false;name=i->second.profile;}return true;
    };
    if(!check(next.defaults))return fail("Invalid Default profile reference");
    for(const auto& p:next.named)if(!check(p.second))return fail("Missing/cyclic named profile");
    for(const auto* collection:{&next.maps,&next.zones,&next.areas})for(const auto& p:*collection)if(!check(p.second))return fail("Missing/cyclic location profile");
    out=std::move(next);error.clear();return true;
}
EnvironmentResolution ResolveEnvironment(const EnvironmentConfiguration& c,const LocationContext& location) {
    EnvironmentResolution result;
    auto apply=[&](const EnvironmentProfile& p) {
        std::vector<const EnvironmentProfile*> chain;std::string name=p.profile;
        std::set<std::string> seen;
        while(!name.empty()&&name!="Default"&&seen.insert(name).second) {auto i=c.named.find(name);if(i==c.named.end())break;chain.push_back(&i->second);name=i->second.profile;}
        for(auto i=chain.rbegin();i!=chain.rend();++i)for(size_t k=0;k<ParameterCount;++k)if((*i)->values[k])result.state.values[k]=*(*i)->values[k];
        for(size_t k=0;k<ParameterCount;++k)if(p.values[k])result.state.values[k]=*p.values[k];
        if(!p.profile.empty()&&p.profile!="Default")result.profile=p.profile;
    };
    if(!c.enabled)return result;
    apply(c.defaults);
    if(!location.valid)return result;
    auto layer=[&](const auto& entries,uint32_t id,const char* source) {auto i=entries.find(id);if(i!=entries.end()){apply(i->second);result.source=source;}};
    layer(c.maps,location.mapId,"Map");layer(c.zones,location.zoneId,"Zone");if(location.areaId)layer(c.areas,location.areaId,"Area");return result;
}
ResolvedEnvironmentState BlendEnvironment(const ResolvedEnvironmentState& a,const ResolvedEnvironmentState& b,float t) {
    ResolvedEnvironmentState result;t=std::clamp(t,0.f,1.f);t=t*t*(3-2*t);
    for(size_t k=0;k<ParameterCount;++k)result.values[k]=a.values[k]+(b.values[k]-a.values[k])*t;return result;
}
}
