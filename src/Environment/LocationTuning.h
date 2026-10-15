#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "LocationContext.h"
namespace renderer::locationtuning {
inline std::wstring baseFile, localFile;
inline LocationContext active;
inline bool editable=false;
inline bool editZone=false;
inline bool IsGlobal(LPCWSTR section) { return section && _wcsicmp(section,L"PostProcess")==0; }
inline std::wstring Scope(unsigned level) {
    std::wstring result=L"Map."+std::to_wstring(active.mapId);
    if(level>=1)result+=L".Zone."+std::to_wstring(active.zoneId);
    if(level>=2)result+=L".Area."+std::to_wstring(active.areaId);
    return result;
}
// A scoped read used to walk the whole hierarchy per key, probing every level
// with its own GetPrivateProfileStringW before re-reading the level that hit -
// two calls per level, up to six, and then the global file on top. The F7 menu
// re-reads every one of its rows whenever the place changes
// (tuningoverlay::Configure -> ReadEditorInt, one row per control) and this
// client's place flaps across a boundary while the player walks it, so that
// probe is what a crossing spends its time in. The key set of a scoped section
// is a property of the file, not of the key being asked for: read it once with
// GetPrivateProfileSectionW and answer every later probe from memory. Only the
// scoped file is memoized - the global read stays the same call it always was,
// so an edit to GraphicsEffects.ini is still visible to the very next read.
// A key counts as present only when it has a non-blank value, which is exactly
// what the probe tested: `Key=` and `Key=   ` are absent to the profile API.
inline std::unordered_map<std::wstring,std::unordered_set<std::wstring>>& ScopedKeys() {
    static std::unordered_map<std::wstring,std::unordered_set<std::wstring>> keys;
    return keys;
}
inline void InvalidateScopedKeys() { ScopedKeys().clear(); }
inline std::wstring LowerKeyName(const wchar_t* key) {
    std::wstring text=key?key:L"";
    for(auto& c:text)if(c>=L'A'&&c<=L'Z')c=static_cast<wchar_t>(c+(L'a'-L'A'));
    return text;
}
inline const std::unordered_set<std::wstring>* ScopedKeysFor(const std::wstring& scope) {
    if(localFile.empty())return nullptr;
    auto& cache=ScopedKeys();
    const auto found=cache.find(scope);
    if(found!=cache.end())return &found->second;
    if(cache.size()>=96)cache.clear();
    // Sized to the profile API's own ceiling for a section string (32767 wide
    // characters), so the key set of a section is never read in half: the editor
    // writes at most one value per control per scope, and a section that could
    // overflow this cannot come from this renderer at all.
    std::vector<wchar_t> buffer(32768);
    const DWORD length=GetPrivateProfileSectionW(scope.c_str(),buffer.data(),static_cast<DWORD>(buffer.size()),localFile.c_str());
    std::unordered_set<std::wstring> keys;
    if(length)for(const wchar_t* line=buffer.data();*line;line+=std::wcslen(line)+1) {
        const wchar_t* equals=std::wcschr(line,L'=');
        if(!equals)continue;
        const wchar_t* value=equals+1;
        while(*value==L' '||*value==L'\t')++value;
        if(!*value)continue;
        keys.emplace(LowerKeyName(std::wstring(line,equals).c_str()));
    }
    return &cache.emplace(scope,std::move(keys)).first->second;
}
inline bool ScopedValuePresent(const std::wstring& scope,LPCWSTR key) {
    const auto* keys=ScopedKeysFor(scope);
    return keys&&keys->find(LowerKeyName(key))!=keys->end();
}
inline bool Sync(const std::wstring& base,const LocationContext& location,bool verified) {
    const bool changed=!active.SamePlace(location)||editable!=(location.valid&&verified)||baseFile!=base+L"GraphicsEffects.ini";
    const std::wstring nextBase=base+L"GraphicsEffects.ini",nextLocal=base+L"LocationGraphics.ini";
    // A different place, a different file or a different verification state all
    // invalidate what was read out of the scoped file; so does an editor save,
    // and F12.
    if(changed||nextLocal!=localFile)InvalidateScopedKeys();
    baseFile=nextBase;localFile=nextLocal;
    active=location;editable=location.valid&&verified;return changed;
}
inline DWORD ReadAtLevel(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path,int topLevel) {
    if(!IsGlobal(section)&&editable&&path&&baseFile==path) {
        for(int level=topLevel;level>=0;--level) {
            const auto scope=Scope(level)+L"."+section;
            if(!ScopedKeysFor(scope))break;
            if(!ScopedValuePresent(scope,key))continue;
            return GetPrivateProfileStringW(scope.c_str(),key,fallback,output,size,localFile.c_str());
        }
    }
    return GetPrivateProfileStringW(section,key,fallback,output,size,path);
}
inline DWORD ReadString(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path) {
    return ReadAtLevel(section,key,fallback,output,size,path,active.areaId?2:1);
}
inline UINT ReadEditorInt(LPCWSTR section,LPCWSTR key,INT fallback,LPCWSTR path) {
    wchar_t value[128]{};ReadAtLevel(section,key,L"",value,128,path,editZone||!active.areaId?1:2);
    return value[0]?static_cast<UINT>(wcstol(value,nullptr,10)):static_cast<UINT>(fallback);
}
inline UINT ReadInt(LPCWSTR section,LPCWSTR key,INT fallback,LPCWSTR path) {
    wchar_t value[128]{};ReadString(section,key,L"",value,128,path);
    return value[0]?static_cast<UINT>(wcstol(value,nullptr,10)):static_cast<UINT>(fallback);
}
// True only for a value actually written into LocationGraphics.ini at some scope
// for the current place. ReadString cannot answer this - it falls back to the
// global file - and the water colours need the distinction: a shipped per-zone
// colour replaces the global default, but never a player's own scoped override.
inline bool HasScopedValue(LPCWSTR section,LPCWSTR key) {
    if(!editable||localFile.empty())return false;
    for(int level=active.areaId?2:1;level>=0;--level) {
        const auto scope=Scope(level)+L"."+section;
        if(!ScopedKeysFor(scope))break;
        if(ScopedValuePresent(scope,key))return true;
    }
    return false;
}
inline bool Save(LPCWSTR section,LPCWSTR key,int value) {
    if(IsGlobal(section))return !baseFile.empty()&&WritePrivateProfileStringW(section,key,std::to_wstring(value).c_str(),baseFile.c_str())!=0;
    // Per-location overrides are only meaningful once this client's location
    // has been verified against the database. On every other 3.3.5 build
    // (including a stock client) there is no verified location to scope the
    // write to, and refusing the write made the whole F7 menu look alive -
    // rows highlighted under the cursor - while every toggle/slider snapped
    // straight back. Fall back to the plain global section in
    // GraphicsEffects.ini instead: ReadAtLevel already reads exactly that
    // value back whenever `editable` is false, so an edit now round-trips,
    // and a later verified session still leaves its scoped file untouched.
    if(!editable)return !baseFile.empty()&&WritePrivateProfileStringW(section,key,std::to_wstring(value).c_str(),baseFile.c_str())!=0;
    const auto scope=Scope(editZone||!active.areaId?1:2)+L"."+section;
    const bool written=WritePrivateProfileStringW(scope.c_str(),key,std::to_wstring(value).c_str(),localFile.c_str())!=0;
    // What the scoped cache holds is now one key out of date. An editor save is
    // a player action, not a per-frame event, so simply dropping it is cheaper
    // to reason about than patching the one section it wrote.
    if(written)InvalidateScopedKeys();
    return written;
}
}
