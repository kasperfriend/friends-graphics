#pragma once
#include <windows.h>
#include <string>
#include "LocationContext.h"
namespace renderer::locationtuning {
inline std::wstring baseFile, localFile;
inline LocationContext active;
inline bool editable=false;
inline bool editZone=false;
inline bool editGlobal=false;
inline std::wstring GlobalScope(LPCWSTR section) { return std::wstring(L"Global.")+section; }
inline bool HasGlobalValue(LPCWSTR section,LPCWSTR key) {
    if(localFile.empty())return false;
    wchar_t value[128]{};
    GetPrivateProfileStringW(GlobalScope(section).c_str(),key,L"",value,128,localFile.c_str());
    return value[0]!=0;
}
inline bool ClearGlobalValue(LPCWSTR section,LPCWSTR key) {
    return !localFile.empty()&&WritePrivateProfileStringW(GlobalScope(section).c_str(),key,nullptr,localFile.c_str())!=0;
}
inline bool IsGlobal(LPCWSTR section) { return section && _wcsicmp(section,L"PostProcess")==0; }
inline std::wstring Scope(unsigned level) {
    std::wstring result=L"Map."+std::to_wstring(active.mapId);
    if(level>=1)result+=L".Zone."+std::to_wstring(active.zoneId);
    if(level>=2)result+=L".Area."+std::to_wstring(active.areaId);
    return result;
}
inline bool Sync(const std::wstring& base,const LocationContext& location,bool verified) {
    const bool changed=!active.SamePlace(location)||editable!=(location.valid&&verified)||baseFile!=base+L"GraphicsEffects.ini";
    baseFile=base+L"GraphicsEffects.ini";localFile=base+L"LocationGraphics.ini";
    active=location;editable=location.valid&&verified;return changed;
}
inline DWORD ReadAtLevel(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path,int topLevel) {
    if(!IsGlobal(section)&&editable&&path&&baseFile==path) {
        for(int level=topLevel;level>=0;--level) {
            const auto scope=Scope(level)+L"."+section;
            wchar_t value[128]{};
            GetPrivateProfileStringW(scope.c_str(),key,L"",value,128,localFile.c_str());
            if(value[0])return GetPrivateProfileStringW(scope.c_str(),key,fallback,output,size,localFile.c_str());
        }
    }
    return GetPrivateProfileStringW(section,key,fallback,output,size,path);
}
inline DWORD ReadString(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path) {
    // Explicit global edits outrank every location, even without a verified location.
    if(!IsGlobal(section)&&path&&baseFile==path&&HasGlobalValue(section,key))
        return GetPrivateProfileStringW(GlobalScope(section).c_str(),key,fallback,output,size,localFile.c_str());
    return ReadAtLevel(section,key,fallback,output,size,path,active.areaId?2:1);
}
inline UINT ReadEditorInt(LPCWSTR section,LPCWSTR key,INT fallback,LPCWSTR path) {
    // Location editors deliberately show their own layer, not the global mask.
    wchar_t value[128]{};
    if((editGlobal||!editable)&&!IsGlobal(section)&&path&&baseFile==path&&HasGlobalValue(section,key))
        GetPrivateProfileStringW(GlobalScope(section).c_str(),key,L"",value,128,localFile.c_str());
    else if(editGlobal)GetPrivateProfileStringW(section,key,L"",value,128,path);
    else ReadAtLevel(section,key,L"",value,128,path,editZone||!active.areaId?1:2);
    return value[0]?static_cast<UINT>(wcstol(value,nullptr,10)):static_cast<UINT>(fallback);
}
inline UINT ReadInt(LPCWSTR section,LPCWSTR key,INT fallback,LPCWSTR path) {
    wchar_t value[128]{};ReadString(section,key,L"",value,128,path);
    return value[0]?static_cast<UINT>(wcstol(value,nullptr,10)):static_cast<UINT>(fallback);
}
// True for an explicit global or location override in LocationGraphics.ini.
// ReadString cannot answer this - it falls back to the
// global file - and the water colours need the distinction: a shipped per-zone
// colour replaces the global default, but never a player's own scoped override.
inline bool HasScopedValue(LPCWSTR section,LPCWSTR key) {
    if(HasGlobalValue(section,key))return true;
    if(!editable||localFile.empty())return false;
    for(int level=active.areaId?2:1;level>=0;--level) {
        const auto scope=Scope(level)+L"."+section;
        wchar_t value[128]{};GetPrivateProfileStringW(scope.c_str(),key,L"",value,128,localFile.c_str());
        if(value[0])return true;
    }
    return false;
}
inline bool Save(LPCWSTR section,LPCWSTR key,int value) {
    if(IsGlobal(section))return !baseFile.empty()&&WritePrivateProfileStringW(section,key,std::to_wstring(value).c_str(),baseFile.c_str())!=0;
    if(editGlobal||(!editable&&HasGlobalValue(section,key)))return !localFile.empty()&&WritePrivateProfileStringW(GlobalScope(section).c_str(),key,std::to_wstring(value).c_str(),localFile.c_str())!=0;
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
    return WritePrivateProfileStringW(scope.c_str(),key,std::to_wstring(value).c_str(),localFile.c_str())!=0;
}
}
