#pragma once
#include <windows.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include "LocationContext.h"
namespace renderer::locationtuning {
inline std::wstring baseFile, localFile;
inline LocationContext active;
inline bool editable=false;
inline bool editZone=false;
inline bool editGlobal=false;
inline bool IsGlobal(LPCWSTR section) { return section && _wcsicmp(section,L"PostProcess")==0; }
inline std::wstring Scope(unsigned level) {
    std::wstring result = L"Map." + std::to_wstring(active.mapId);
    if (level >= 1) result += L".Zone." + std::to_wstring(active.zoneId);
    if (level >= 2) result += L".Area." + std::to_wstring(active.areaId);
    return result;
}
// A scoped read used to walk the whole hierarchy per key, probing every level
// with its own GetPrivateProfileStringW before re-reading the level that hit -
// two calls per level, up to six, and then the global file on top. The F7 menu
// re-reads every one of its rows whenever the place changes
// (tuningoverlay::Configure -> ReadEditorInt, one row per control) and this
// client's place flaps across a boundary while the player walks it, so that
// probe is what a crossing spends its time in.
//
// That answer came from two caches: one of "which keys does this scope hold", one
// entry per scope, and the values behind it, one section per file. The second one
// is all the first ever needed - the key set of a scope IS the key set of the
// section the value cache holds, down to the same rule that `Key=` counts as
// absent - and keeping both is what a crossing paid for. The key cache had a 96
// entry ceiling, and a real session's place still flaps across more scopes than
// that inside a single apply: every level of every section the nine tuning reloads
// and the F7 menu's hundred-odd rows ask about. So the ceiling was reached, the
// whole memo was dropped in the middle of the apply, and every read that followed
// fell back to a GetPrivateProfileSectionW - a whole-file parse, one per scope per
// level. 46 to 78 ms per settled crossing, dozens of them in a session's log, and
// what a player feels as the stall.
//
// There is one cache now. It is revalidated against the file's own identity - its
// write time and size - on every read, so an edit to either file is visible to the
// very next read and the check costs microseconds, and it answers presence and
// value with the same lookup. A key counts as present only when it has a
// non-blank value: `Key=` and `Key=   ` are absent to the profile API.
inline std::wstring LowerKeyName(const wchar_t* key) {
    std::wstring text=key?key:L"";
    for(auto& c:text)if(c>=L'A'&&c<=L'Z')c=static_cast<wchar_t>(c+(L'a'-L'A'));
    return text;
}
// Section names come off a listing that is already lowercased, so the fingerprint
// has no reason to build the string LowerKeyName would return from scratch.
inline std::wstring& LowerInPlace(std::wstring& text) {
    for(auto& c:text)if(c>=L'A'&&c<=L'Z')c=static_cast<wchar_t>(c+(L'a'-L'A'));
    return text;
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
// read still checks the file's identity first, so an edit is visible to the
// next frame's reads - the guarantee the old comment made - at microseconds
// instead of milliseconds. The identity is re-stat'd at most once every 10 ms
// per file, not once per read: one settled crossing stats the same file once
// per read (hundreds of identity checks for one answer), and on a game folder
// a scanner is watching each stat is a millisecond the player feels on the
// crossing. 10 ms sits safely under the 15.625 ms system-timer step, so on the
// default 64 Hz timer the very next tick re-stats and no external edit can
// stay hidden for more than one tick (a reload cannot act on an edit before
// the next frame either, so the window hides nothing); on a high-resolution
// timer it is a plain 10 ms. Every write this project makes drops the values
// outright, and F12 drops everything, so no edit can stay hidden for more
// than one frame regardless.
// Blank values are absent, exactly as the profile API treats them.
struct ProfileValues {
    bool known=false;
    bool exists=false;
    ULONGLONG written=0;
    DWORD bytes=0;
    ULONGLONG lastStatMs=0;
    std::unordered_map<std::wstring,std::unordered_map<std::wstring,std::wstring>> sections;
};
inline std::unordered_map<std::wstring,ProfileValues>& ProfileCache() {
    static std::unordered_map<std::wstring,ProfileValues> files;
    return files;
}
inline bool ProfileIdentity(ProfileValues& file,const std::wstring& path) {
    // Trust the cached identity until 10 ms have passed (see the comment on
    // ProfileValues for why 10 ms hides no edit). A first-ever look or a file
    // we have not stat'd recently stats for real.
    const ULONGLONG now=GetTickCount64();
    if(file.known&&(now-file.lastStatMs)<10)return true;
    file.lastStatMs=now;
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
// ---------------------------------------------------------------------------
// Per-subsystem settings fingerprints.
//
// The file-level fingerprint below answers "did the scoped settings for this
// place change". A crossing between two places that genuinely differ in one
// key still re-applies every subsystem, and each re-apply pays its side
// effects - a log write, a temporal-history reset, a cloud-field thread
// check, a cache invalidation - on exactly the frame the player is walking
// the border on. Each ReloadTuning therefore fingerprints the raw values it
// reads and, when they are exactly what it applied before, skips the
// re-apply: the values the draw loop sees are unchanged by construction, so
// nothing about the picture changes. A 0 stored fingerprint means "never
// applied", which is why the guard also tests it.
struct SettingsFingerprint {
    uint64_t h=1469598103934665603ull;
    void mix(uint64_t v){h^=v+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);}
};
inline void MixInt(SettingsFingerprint& fp,int v){fp.mix(static_cast<uint64_t>(static_cast<uint32_t>(v)));}
inline void MixBool(SettingsFingerprint& fp,bool v){fp.mix(v?1ull:0ull);}
inline bool SameSettings(uint64_t& applied,SettingsFingerprint& fp) {
    const bool same=applied!=0&&applied==fp.h;
    if(!same)applied=fp.h;
    return same;
}
// The renderer's log grows across a long session, and a large file sitting in
// the game folder is what a scanner keeps re-scanning - deleting it
// mid-session is how players have found the crossing stall. Once past two
// megabytes the log is rotated to <name>.1: the oldest content is discarded,
// the recent content - the part anyone reading the log cares about - survives.
inline void RotateLogIfLarge(const std::wstring& path,size_t limitBytes=2*1024*1024) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)==0)return;
    if(static_cast<size_t>(info.nFileSizeLow)<limitBytes)return;
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path+L".1"),ec);
    std::filesystem::rename(std::filesystem::path(path),std::filesystem::path(path+L".1"),ec);
}
// What the cache actually did, for the line a report is read from: settings reads
// answered out of a section already in memory, and whole-file parses made to fill
// it. The reload line carries the difference per apply.
inline uint64_t profileReads=0;
inline uint64_t profileParses=0;
inline const std::unordered_map<std::wstring,std::wstring>* ProfileSection(ProfileValues& file,
    const std::wstring& path,LPCWSTR section) {
    const std::wstring name=LowerKeyName(section);
    const auto found=file.sections.find(name);
    if(found!=file.sections.end())return &found->second;
    // The profile API's own ceiling for a section string; a section that could
    // overflow this cannot come from this renderer at all.
    if(file.sections.size()>=4096)file.sections.clear();
    ++profileParses;
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
        ++profileReads;
        return static_cast<DWORD>(length);
    }
    ++profileParses;
    return GetPrivateProfileStringW(section,key,fallback,output,size,path);
}
// Every cached section of every file this module reads. A write invalidates them:
// see WriteProfile.
inline void InvalidateProfileValues() { ProfileCache().clear(); }
// What F12, an editor save and this module's own scoped writes mean: drop what was
// read. The file's identity is not a reliable signal right after a write of ours -
// NTFS updates a last-write time lazily, and a write of the same length does not
// change the size either - so a write drops rather than hopes. One cache, so one
// thing to drop.
inline void InvalidateScopedKeys() { InvalidateProfileValues(); }
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
// The section a scope names, through the same identity-checked cache every other
// read uses: one lookup answers both "does this scope hold the key" and "what does
// it say". nullptr still means "there is no scoped file to answer from", the one
// answer ReadAtLevel's walk treats as a reason to stop.
inline const std::unordered_map<std::wstring,std::wstring>* ScopedValuesFor(const std::wstring& scope) {
    if(localFile.empty())return nullptr;
    ProfileValues& file=ProfileCache()[localFile];
    ProfileIdentity(file,localFile);
    return ProfileSection(file,localFile,scope.c_str());
}
// Whether the file the scoped reads were taken from is still the file on disk: the
// question Sync asks before deciding that a place change needs nothing re-read.
// Nothing read yet answers false, which is the safe direction - the next settle
// applies instead of skipping.
inline bool ScopedFileUnchanged() {
    if(localFile.empty())return false;
    if(ProfileCache().find(localFile)==ProfileCache().end())return false;
    ProfileValues& file=ProfileCache()[localFile];
    return ProfileIdentity(file,localFile);
}
inline bool ScopedValuePresent(const std::wstring& scope,LPCWSTR key) {
    const auto* values=ScopedValuesFor(scope);
    return values&&values->find(LowerKeyName(key))!=values->end();
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
// ids: both files' own identity plus every scoped section that belongs to the
// place's three levels, keys and values. Two places with the same fingerprint
// answer all nine subsystems the same way, so the reload is skipped and the log
// says 0 us instead of a stalled second - and the nine subsystems read this place
// through nothing else: every one of them goes through ReadInt/ReadString/
// HasScopedValue, which resolve against these levels and these files alone.
//
// The sections are read from the FILE, never from what a previous read happened
// to cache. That distinction is what keeps this from trading a stall for a setting
// that quietly stops working: a place whose own scope was written by hand, or by
// the editor, in a section no read in this session ever touched has no cached
// section at all, so a fingerprint built from the cache would equal the last
// place's, the crossing would be skipped, and the override would never be read.
// The file's own section list answers that question honestly, one listing per file
// identity, and a listing that cannot be proven (truncated at the profile API's
// own buffer ceiling) refuses the skip instead of guessing.
//
inline void FingerprintMix(uint64_t& hash,const std::wstring& text) {
    for(const wchar_t character:text)hash=(hash^static_cast<uint64_t>(character))*1099511628211ull;
    hash*=1099511628211ull;
}
// What a file IS right now - the stat a read would make, not what an earlier read
// remembered of it.
inline void FileIdentity(const std::wstring& path,ULONGLONG& written,DWORD& bytes) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)) {written=0;bytes=0;return;}
    written=(static_cast<ULONGLONG>(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
    bytes=info.nFileSizeLow;
}
// Every section name a file holds, read from the file itself and kept per identity,
// so a section that has never been read still counts.
struct SectionListing {
    bool known=false;
    ULONGLONG written=0;
    DWORD bytes=0;
    std::vector<std::wstring> names;
};
inline std::unordered_map<std::wstring,SectionListing>& SectionNameCache() {
    static std::unordered_map<std::wstring,SectionListing> files;
    return files;
}
// nullptr means "this listing proves nothing" - the profile API truncated it at
// its own buffer ceiling - and the caller then refuses to skip rather than assume
// a file holds no more sections than fit.
inline const std::vector<std::wstring>* SectionNamesOf(const std::wstring& path,ULONGLONG written,DWORD bytes) {
    if(path.empty())return nullptr;
    SectionListing& listing=SectionNameCache()[path];
    if(!listing.known||listing.written!=written||listing.bytes!=bytes) {
        // The profile API's ceiling for a section-name string; a file this module
        // writes cannot come near it.
        std::vector<wchar_t> buffer(32768);
        const DWORD length=GetPrivateProfileSectionNamesW(buffer.data(),static_cast<DWORD>(buffer.size()),path.c_str());
        listing.known=false;listing.written=written;listing.bytes=bytes;listing.names.clear();
        if(length>=static_cast<DWORD>(buffer.size())-2)return nullptr;
        listing.known=true;
        DWORD at=0;
        while(at<length&&buffer[at]) {
            const std::wstring name(&buffer[at]);
            at+=static_cast<DWORD>(name.size())+1;
            listing.names.push_back(LowerKeyName(name.c_str()));
        }
    }
    return listing.known?&listing.names:nullptr;
}
// Which section a name belongs to. A section is named `Scope(level) + "." + suffix`
// by the scoped writes and by the suffix alone in the shipped file, so stripping a
// leading map/zone/area chain leaves the suffix both files agree on: `wind`,
// `water`, `postprocess` - never the ids of the place that wrote it.
inline std::wstring SectionSuffixOf(const std::wstring& name) {
    // Both files answer the same way: the listing of the scoped file is lowercased
    // already, the shipped one is not.
    std::wstring lowered(name);
    LowerInPlace(lowered);
    if(lowered.compare(0,4,L"map.")!=0)return lowered;
    const auto number=[&lowered](size_t& at) {
        const size_t begin=at;
        while(at<lowered.size()&&lowered[at]>=L'0'&&lowered[at]<=L'9')++at;
        return at>begin;
    };
    size_t at=4;
    if(!number(at)||at>=lowered.size()||lowered[at]!=L'.')return lowered;
    ++at;
    if(lowered.compare(at,5,L"zone.")==0) {
        at+=5;
        if(!number(at)||at>=lowered.size()||lowered[at]!=L'.')return lowered;
        ++at;
    }
    if(lowered.compare(at,5,L"area.")==0) {
        at+=5;
        if(!number(at)||at>=lowered.size()||lowered[at]!=L'.')return lowered;
        ++at;
    }
    return at<lowered.size()?lowered.substr(at):lowered;
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
// ids: both files' own identity plus every scoped section that belongs to the
// place's three levels, keys and values. Two places with the same fingerprint
// answer all nine subsystems the same way, so the reload is skipped and the log
// says 0 us instead of a stalled second - and the nine subsystems read this place
// through nothing else: every one of them goes through ReadInt/ReadString/
// HasScopedValue, which resolve against these levels and these files alone.
//
// What is compared per place is the ANSWER, not the file: for every suffix that
// either file names a section with, the value a scoped read of that section would
// resolve - the deepest level holding the key wins, exactly as ReadAtLevel walks
// it - plus whether a scope holds the key at all, which is the second question the
// same reads ask (the water colours use it so that a shipped per-zone colour never
// outranks a player's own scope). Which LEVEL answered is not part of the answer:
// two scopes that resolve to the same value answer every read the same way, and
// mixing the level in would refuse to skip the very `Area=0`/`Area=223` flap this
// exists for.
inline uint64_t ScopedFingerprint(const LocationContext& location) {
    // Scope() names a scope from the active place, and this is asked about the
    // place that settled: answer as that place and put the active one back.
    const LocationContext previous=active;
    active=location;
    uint64_t hash=1469598103934665603ull;
    bool provable=true;
    const std::vector<std::wstring>* localNames=nullptr;
    const std::vector<std::wstring>* baseNames=nullptr;
    for(const std::wstring& path:{baseFile,localFile}) {
        if(path.empty())continue;
        ULONGLONG written=0;DWORD bytes=0;
        FileIdentity(path,written,bytes);
        FingerprintMix(hash,std::to_wstring(written));
        FingerprintMix(hash,std::to_wstring(bytes));
        const std::vector<std::wstring>* names=SectionNamesOf(path,written,bytes);
        if(!names)provable=false;
        if(path==localFile)localNames=names; else baseNames=names;
    }
    if(provable&&!localFile.empty()) {
        std::vector<std::wstring> suffixes;
        for(const std::vector<std::wstring>* names:{localNames,baseNames}) {
            if(!names)continue;
            for(const std::wstring& name:*names) {
                std::wstring suffix=SectionSuffixOf(name);
                if(suffix.empty())continue;
                if(std::find(suffixes.begin(),suffixes.end(),suffix)==suffixes.end())
                    suffixes.push_back(std::move(suffix));
            }
        }
        std::sort(suffixes.begin(),suffixes.end());
        ProfileValues& file=ProfileCache()[localFile];
        ProfileIdentity(file,localFile);
        const int top=location.areaId?2:1;
        for(const std::wstring& suffix:suffixes) {
            // The walk ReadAtLevel makes, answered from the same cache it uses:
            // the deepest level holding a key decides its value, and whether any
            // scope holds it at all is mixed too.
            std::map<std::wstring,std::wstring> effective;
            std::set<std::wstring> present;
            for(int level=top;level>=0;--level) {
                const std::wstring section=LowerKeyName((Scope(level)+L"."+suffix).c_str());
                const auto* values=ProfileSection(file,localFile,section.c_str());
                if(!values)continue;
                for(const auto& [key,value]:*values) {
                    present.insert(key);
                    effective.try_emplace(key,value);
                }
            }
            FingerprintMix(hash,suffix);
            for(const auto& [key,value]:effective) {FingerprintMix(hash,key);FingerprintMix(hash,value);}
            if(!present.empty()) {
                FingerprintMix(hash,L"scoped");
                for(const std::wstring& key:present)FingerprintMix(hash,key);
            }
        }
    }
    active=previous;
    return provable?hash:0;
}
// The fingerprint the nine subsystems were last configured with, and whether that
// configuration is still the one this place would produce.
inline uint64_t appliedFingerprint=0;
inline bool appliedEditable=false;
inline bool AppliedTuningStillInForce(const LocationContext& location,bool verified) {
    if(!appliedFingerprint)return false;
    if(appliedEditable!=(location.valid&&verified))return false;
    if(!ScopedFileUnchanged())return false;
    if(!baseFile.empty()&&!ProfileIdentity(ProfileCache()[baseFile],baseFile))return false;
    const uint64_t current=ScopedFingerprint(location);
    return current!=0&&current==appliedFingerprint;
}
inline void MarkTuningApplied(const LocationContext& location,bool verified) {
    appliedEditable=location.valid&&verified;
    appliedFingerprint=ScopedFingerprint(location);
    if(!appliedFingerprint)appliedFingerprint=1; // 0 is "nothing applied yet"
}
inline DWORD ReadAtLevel(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD size,LPCWSTR path,int topLevel) {
    if(!IsGlobal(section)&&editable&&path&&baseFile==path) {
        const std::wstring wanted=LowerKeyName(key);
        for(int level=topLevel;level>=0;--level) {
            const auto scope=Scope(level)+L"."+section;
            const auto* values=ScopedValuesFor(scope);
            if(!values)break;
            if(values->find(wanted)==values->end())continue;
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
inline UINT ReadInt(LPCWSTR section, LPCWSTR key, INT fallback, LPCWSTR path) {
    wchar_t value[128]{}; ReadString(section, key, L"", value, 128, path);
    return value[0] ? static_cast<UINT>(wcstol(value, nullptr, 10)) : static_cast<UINT>(fallback);
}
// True for an explicit global or location override in LocationGraphics.ini.
// ReadString cannot answer this - it falls back to the
// global file - and the water colours need the distinction: a shipped per-zone
// colour replaces the global default, but never a player's own scoped override.
inline bool HasScopedValue(LPCWSTR section,LPCWSTR key) {
    if(HasGlobalValue(section,key))return true;
    if(!editable||localFile.empty())return false;
    const std::wstring wanted=LowerKeyName(key);
    for(int level=active.areaId?2:1;level>=0;--level) {
        const auto scope=Scope(level)+L"."+section;
        const auto* values=ScopedValuesFor(scope);
        if(!values)break;
        if(values->find(wanted)!=values->end())return true;
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

// Cached helpers for callers that still use the raw Win32 profile API on other
// files (ModernWoWRenderer.ini, the location provider's file): answered from the
// same identity-checked section cache as the scoped reads, so a file read here is
// kept in memory until it changes on disk, blank values included - they are
// absent, exactly as the profile API answers them.
inline bool ReadCachedString(LPCWSTR path, LPCWSTR section, LPCWSTR key, std::wstring& out) {
    if(!path||!section||!key)return false;
    wchar_t value[1024]{};
    if(ReadProfileString(section,key,L"",value,1024,path)==0)return false;
    out=value;
    return true;
}
inline UINT ReadCachedInt(LPCWSTR path, LPCWSTR section, LPCWSTR key, INT fallback) {
    std::wstring v;
    if(ReadCachedString(path,section,key,v))
        return static_cast<UINT>(wcstol(v.c_str(),nullptr,10));
    return static_cast<UINT>(fallback);
}
}
