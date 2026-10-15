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
inline bool editGlobal=false;
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
// GetPrivateProfileSectionW and answer every later probe from memory.
// A key counts as present only when it has a non-blank value, which is exactly
// what the probe tested: `Key=` and `Key=   ` are absent to the profile API.
//
// The VALUES are memoized the same way, one section at a time and per file, and
// that is the other half of the crossing's cost: the nine tuning reloads and the
// F7 menu's rows between them read on the order of a hundred keys, and each
// GetPrivateProfileStringW parses the whole file - 125 to 200 ms per crossing in
// a real session, measured. Every read still checks the file's own identity (its
// write time and size) first, so an edit to either file is visible to the very
// next read, and the check costs microseconds.
inline std::unordered_map<std::wstring,std::unordered_set<std::wstring>>& ScopedKeys() {
    static std::unordered_map<std::wstring,std::unordered_set<std::wstring>> keys;
    return keys;
}
// InvalidateScopedKeys lives below, next to the value cache it also drops.
// The identity of the file the memo was filled from. A memo of a scoped section
// is a property of the FILE - which keys it has and what they hold - and not of
// the place that happens to be active, so the one thing that has to drop it is a
// change to the file itself: a different path, a hand edit, a save. A place
// change is not a file change, and clearing on one is what made a crossing
// re-read the whole scoped file, section by section, while the place flapped.
inline ULONGLONG memoWritten=0;
inline DWORD memoBytes=0;
inline void RememberScopedFile() {
    memoWritten=0;memoBytes=0;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if(!localFile.empty()&&GetFileAttributesExW(localFile.c_str(),GetFileExInfoStandard,&info)) {
        memoWritten=(static_cast<ULONGLONG>(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
        memoBytes=info.nFileSizeLow;
    }
}
inline bool ScopedFileUnchanged() {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if(!GetFileAttributesExW(localFile.c_str(),GetFileExInfoStandard,&info))return memoWritten==0&&memoBytes==0;
    const ULONGLONG written=(static_cast<ULONGLONG>(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
    return written==memoWritten&&info.nFileSizeLow==memoBytes;
}
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
    // The first section of a fresh memo: from here on, the file this was read
    // from is the memo's own identity, and Sync compares against it.
    if(cache.empty())RememberScopedFile();
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
// ---------------------------------------------------------------------------
// The values, read once per file identity.
//
// The key-set memo above answers "does this scope have this key" without touching
// the file; the value still came from a GetPrivateProfileStringW, and that call
// parses the whole file - measured, in a real session, at 125-200 ms for the nine
// tuning reloads and the F7 menu's 102 rows together, once per crossing, which is
// exactly the stall a player feels walking across a border. A profile file is a
// few kilobytes of key=value text: read a section ONCE, keep it, and answer from
// memory until the file's own identity (its write time and size) changes. Every
// read still stats the file first, so an edit is visible to the very next read -
// the guarantee the old comment made - at microseconds instead of milliseconds.
// Blank values are absent, exactly as the profile API treats them.
struct ProfileValues {
    bool known=false;
    bool exists=false;
    ULONGLONG written=0;
    DWORD bytes=0;
    std::unordered_map<std::wstring,std::unordered_map<std::wstring,std::wstring>> sections;
};
inline std::unordered_map<std::wstring,ProfileValues>& ProfileCache() {
    static std::unordered_map<std::wstring,ProfileValues> files;
    return files;
}
inline bool ProfileIdentity(ProfileValues& file,const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    const bool exists=GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)!=0;
    const ULONGLONG written=exists?((static_cast<ULONGLONG>(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime):0;
    const DWORD bytes=exists?info.nFileSizeLow:0;
    if(!file.known||file.exists!=exists||file.written!=written||file.bytes!=bytes) {
        file.known=true;file.exists=exists;file.written=written;file.bytes=bytes;
        file.sections.clear();
        return false;
    }
    return true;
}
inline const std::unordered_map<std::wstring,std::wstring>* ProfileSection(ProfileValues& file,
    const std::wstring& path,LPCWSTR section) {
    const std::wstring name=LowerKeyName(section);
    const auto found=file.sections.find(name);
    if(found!=file.sections.end())return &found->second;
    // The profile API's own ceiling for a section string; a section that could
    // overflow this cannot come from this renderer at all.
    if(file.sections.size()>=512)file.sections.clear();
    std::vector<wchar_t> buffer(32768);
    const DWORD length=GetPrivateProfileSectionW(section,buffer.data(),static_cast<DWORD>(buffer.size()),path.c_str());
    std::unordered_map<std::wstring,std::wstring> values;
    if(length)for(const wchar_t* line=buffer.data();*line;line+=std::wcslen(line)+1) {
        const wchar_t* equals=std::wcschr(line,L'=');
        if(!equals)continue;
        const wchar_t* value=equals+1;
        while(*value==L' '||*value==L'\t')++value;
        if(!*value)continue;
        // The profile API hands a value back without the quotes a writer may have
        // put around it; the cached read has to answer the same way.
        std::wstring text=value;
        if(text.size()>=2&&text.front()==L'"'&&text.back()==L'"')text=text.substr(1,text.size()-2);
        values[LowerKeyName(std::wstring(line,equals).c_str())]=std::move(text);
    }
    return &file.sections.emplace(name,std::move(values)).first->second;
}
// The profile string a caller would have read, from memory when the file has not
// changed: same section, same key, same fallback, same truncation.
inline DWORD ReadProfileString(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path) {
    if(!section||!key||!output||!size||!path)return GetPrivateProfileStringW(section,key,fallback,output,size,path);
    ProfileValues& file=ProfileCache()[std::wstring(path)];
    ProfileIdentity(file,path);
    if(const auto* values=ProfileSection(file,path,section)) {
        const auto found=values->find(LowerKeyName(key));
        const std::wstring text=found==values->end()?std::wstring(fallback?fallback:L""):found->second;
        const size_t length=text.size()<size-1?text.size():size-1;
        if(length)std::wmemcpy(output,text.c_str(),length);
        output[length]=0;
        return static_cast<DWORD>(length);
    }
    return GetPrivateProfileStringW(section,key,fallback,output,size,path);
}
// Every cached section of every file this module reads. A write invalidates them:
// see WriteProfile.
inline void InvalidateProfileValues() { ProfileCache().clear(); }
// What F12 and an editor save mean: drop the key memo AND the values behind it.
inline void InvalidateScopedKeys() { ScopedKeys().clear();InvalidateProfileValues(); }
// The file's own identity is NOT a reliable signal right after a write of ours:
// NTFS updates a file's last-write time lazily, so a stat taken straight after
// WritePrivateProfileStringW can still report the old time, and a value of the
// same length does not change the file's size either. A Save -> read-back round
// trip went stale exactly that way - Save wrote a toggle, the very next read was
// answered from the section the cache had taken before the write - and the water
// device regression is what caught it. So every write this module makes drops the
// values as well as the key memo, and the identity check stays for the writes
// that come from outside.
inline bool WriteProfile(LPCWSTR path,LPCWSTR section,LPCWSTR key,int value) {
    if(!path||!*path)return false;
    const bool written=WritePrivateProfileStringW(section,key,std::to_wstring(value).c_str(),path)!=0;
    if(written)InvalidateProfileValues();
    return written;
}
inline bool ScopedValuePresent(const std::wstring& scope,LPCWSTR key) {
    const auto* keys=ScopedKeysFor(scope);
    return keys&&keys->find(LowerKeyName(key))!=keys->end();
}
// The overlay's third scope: an explicit override that outranks every location.
// It lives in the same scoped file as the location scopes, so it is answered from
// the same key memo and dropped by the same writes - a global override can only
// exist in a file this module already reads once per identity.
inline std::wstring GlobalScope(LPCWSTR section) { return std::wstring(L"Global.")+section; }
inline bool HasGlobalValue(LPCWSTR section,LPCWSTR key) {
    if(localFile.empty())return false;
    return ScopedValuePresent(GlobalScope(section),key);
}
inline bool ClearGlobalValue(LPCWSTR section,LPCWSTR key) {
    if(localFile.empty())return false;
    // Deleting a key (a null value) is a write like any other, and it is the one
    // write WriteProfile cannot express: drop what both caches hold of the file so
    // the next read sees the override gone.
    const bool cleared=WritePrivateProfileStringW(GlobalScope(section).c_str(),key,nullptr,localFile.c_str())!=0;
    if(cleared)InvalidateScopedKeys();
    return cleared;
}
inline bool Sync(const std::wstring& base,const LocationContext& location,bool verified) {
    const bool changed=!active.SamePlace(location)||editable!=(location.valid&&verified)||baseFile!=base+L"GraphicsEffects.ini";
    const std::wstring nextBase=base+L"GraphicsEffects.ini",nextLocal=base+L"LocationGraphics.ini";
    // A different place does NOT invalidate what was read out of the scoped
    // file: which keys a scope has is a property of the file, not of the place
    // that is active, and this client's place flaps across a boundary while the
    // player walks it - `Area=0` and `Area=223` alternating in a real session,
    // each alternation dropping the memo and re-reading every section of the file
    // for the F7 menu and the nine tuning reloads behind it, one
    // GetPrivateProfileSectionW per scope per section. That is the second the
    // player felt on every crossing. What does invalidate it: a different scoped
    // file, the same file changed underneath the memo (a hand edit, another
    // tool), an editor save and F12.
    if(nextLocal!=localFile||!ScopedFileUnchanged())InvalidateScopedKeys();
    baseFile=nextBase;localFile=nextLocal;
    active=location;editable=location.valid&&verified;return changed;
}
// What the last apply was FOR, and whether the same thing is still in force.
// This client's place flaps across a boundary while the player walks it (`Area=0`
// and `Area=223` trading places, each alternation a settle), and a settle of a
// place whose tuning is already in force needs no re-read at all: the values can
// only differ if one of the two files changed in between, and both are checked
// here by their own identity - the same stat a read would make. This is the
// difference between a crossing that re-reads nine subsystems and one that does
// nothing, and it is what the log line of the next report is read for.
// A place does not have to be the SAME place to need no re-read: two areas answer
// a scoped read identically when neither of them overrides anything, which is the
// common case for the `Area=0`/`Area=223` flap this client produces walking a
// border. What decides it is therefore the VALUES a place resolves to, not its
// ids: the global file's identity plus every scoped section that belongs to the
// place's three levels, keys and values. Two places with the same fingerprint
// answer all nine subsystems the same way, so the reload is skipped and the log
// says 0 ms instead of a stalled second.
inline uint64_t FingerprintMix(uint64_t hash,const std::wstring& text) {
    for(const wchar_t character:text)hash=(hash^static_cast<uint64_t>(character))*1099511628211ull;
    return hash*1099511628211ull;
}
inline uint64_t ScopedFingerprint(const LocationContext& location) {
    uint64_t hash=1469598103934665603ull;
    auto& cache=ProfileCache();
    for(const std::wstring& path:{baseFile,localFile}) {
        auto found=cache.find(path);
        if(found==cache.end())continue;
        hash^=found->second.written;hash*=1099511628211ull;
        hash^=found->second.bytes;hash*=1099511628211ull;
    }
    const auto& sections=cache[localFile].sections;
    for(int level=0;level<3;++level) {
        const std::wstring prefix=LowerKeyName((Scope(level)+L".").c_str());
        for(const auto& [name,values]:sections) {
            if(name.compare(0,prefix.size(),prefix)!=0)continue;
            hash=FingerprintMix(hash,name);
            for(const auto& [key,value]:values) {hash=FingerprintMix(hash,key);hash=FingerprintMix(hash,value);}
        }
    }
    return hash;
}
inline uint64_t appliedFingerprint=0;
inline bool appliedEditable=false;
inline bool AppliedTuningStillInForce(const LocationContext& location,bool verified) {
    if(!appliedFingerprint)return false;
    if(appliedEditable!=(location.valid&&verified))return false;
    if(!ScopedFileUnchanged())return false;
    if(!baseFile.empty()&&!ProfileIdentity(ProfileCache()[baseFile],baseFile))return false;
    return ScopedFingerprint(location)==appliedFingerprint;
}
inline void MarkTuningApplied(const LocationContext& location,bool verified) {
    appliedEditable=location.valid&&verified;
    appliedFingerprint=ScopedFingerprint(location);
    if(!appliedFingerprint)appliedFingerprint=1; // 0 is "nothing applied yet"
}
inline DWORD ReadAtLevel(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path,int topLevel) {
    if(!IsGlobal(section)&&editable&&path&&baseFile==path) {
        for(int level=topLevel;level>=0;--level) {
            const auto scope=Scope(level)+L"."+section;
            if(!ScopedKeysFor(scope))break;
            if(!ScopedValuePresent(scope,key))continue;
            return ReadProfileString(scope.c_str(),key,fallback,output,size,localFile.c_str());
        }
    }
    return ReadProfileString(section,key,fallback,output,size,path);
}
inline DWORD ReadString(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path) {
    // Explicit global edits outrank every location, even without a verified location.
    if(!IsGlobal(section)&&path&&baseFile==path&&HasGlobalValue(section,key))
        return ReadProfileString(GlobalScope(section).c_str(),key,fallback,output,size,localFile.c_str());
    return ReadAtLevel(section,key,fallback,output,size,path,active.areaId?2:1);
}
inline UINT ReadEditorInt(LPCWSTR section,LPCWSTR key,INT fallback,LPCWSTR path) {
    // Location editors deliberately show their own layer, not the global mask.
    wchar_t value[128]{};
    if((editGlobal||!editable)&&!IsGlobal(section)&&path&&baseFile==path&&HasGlobalValue(section,key))
        ReadProfileString(GlobalScope(section).c_str(),key,L"",value,128,localFile.c_str());
    else if(editGlobal)ReadProfileString(section,key,L"",value,128,path);
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
        if(!ScopedKeysFor(scope))break;
        if(ScopedValuePresent(scope,key))return true;
    }
    return false;
}
inline bool Save(LPCWSTR section,LPCWSTR key,int value) {
    if(IsGlobal(section))return WriteProfile(baseFile.c_str(),section,key,value);
    // An explicit Global override (the overlay's third scope) outranks every
    // location, and it belongs to the scoped file - so it is the same write path
    // and the same invalidation as a location scope, not a third one.
    if(editGlobal||(!editable&&HasGlobalValue(section,key))) {
        const bool written=WriteProfile(localFile.c_str(),GlobalScope(section).c_str(),key,value);
        if(written)InvalidateScopedKeys();
        return written;
    }
    // Per-location overrides are only meaningful once this client's location
    // has been verified against the database. On every other 3.3.5 build
    // (including a stock client) there is no verified location to scope the
    // write to, and refusing the write made the whole F7 menu look alive -
    // rows highlighted under the cursor - while every toggle/slider snapped
    // straight back. Fall back to the plain global section in
    // GraphicsEffects.ini instead: ReadAtLevel already reads exactly that
    // value back whenever `editable` is false, so an edit now round-trips,
    // and a later verified session still leaves its scoped file untouched.
    if(!editable)return WriteProfile(baseFile.c_str(),section,key,value);
    const auto scope=Scope(editZone||!active.areaId?1:2)+L"."+section;
    // What the scoped cache holds is now one key out of date. An editor save is
    // a player action, not a per-frame event, so simply dropping it is cheaper
    // to reason about than patching the one section it wrote.
    const bool written=WriteProfile(localFile.c_str(),scope.c_str(),key,value);
    if(written)InvalidateScopedKeys();
    return written;
}
}
