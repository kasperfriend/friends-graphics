#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
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
// that inside a single apply: every level of every section the eight tuning reloads
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
// What the cache actually did, for the line a report is read from: settings reads
// answered out of memory, and whole-file loads made to answer them. The reload
// line carries the difference per apply, and a healthy crossing says one load per
// file that changed and a few hundred reads - a load per SECTION is the regression
// this counter exists to catch (see ProfileValues).
inline uint64_t profileReads=0;
inline uint64_t profileParses=0;
// Blank values are absent, exactly as the profile API treats them.
// ---------------------------------------------------------------------------
// One read of the file per identity, not one per section.
//
// Caching the VALUES was only half of the fix. A cache entry still had to be
// filled, and filling one asked the profile API for a single section - which
// opens, reads and parses the whole file to find it. So a crossing that touched
// N sections paid N whole-file parses, and the cost of walking one border was
// the size of the settings file times the number of sections a crossing asks
// about. Measured on the real chain (66 sections per apply):
//
//     LocationGraphics.ini      one section miss     one cold crossing
//                1.7 KB              0.14 ms               3 ms
//               84 KB                1.37 ms              66 ms
//              587 KB               11.3 ms             691 ms
//              2.1 MB               46.6 ms            2653 ms
//
// That is the lag a player reports as "the more per-zone and per-area settings I
// author, the worse the crossing gets" - and it gets worse twice over, because
// the section-name listing below has a fixed buffer that a file this size
// overflows, and an overflowing listing turns the whole skip off (see
// ScopedFingerprint). Every F7 save invalidates the cache, so the next crossing
// pays the whole table again.
//
// So the file is read and decoded ONCE per identity, and what is kept is the
// text plus an index of where each section's lines start and end. A section is
// then parsed out of that text on first ask - a few hundred bytes, not the whole
// file - and stays materialised until the identity moves. The listing comes off
// the same index, with no buffer and therefore no ceiling.
//
// The text is kept rather than every section's map because this is a 32-bit
// process: a two-megabyte settings file materialised in full is tens of
// megabytes of small strings, while its text is two megabytes and the sections a
// crossing actually reads are a bounded few dozen.
struct SectionSpan {
    size_t begin=0,end=0;
    // Duplicate section headers are legal and merge, as they do for the profile
    // API; they chain here instead of costing a container per name.
    uint32_t next=0xffffffffu;
};
using SectionValues=std::unordered_map<std::wstring,std::wstring>;
struct ProfileValues {
    bool known=false;
    bool exists=false;
    ULONGLONG written=0;
    DWORD bytes=0;
    ULONGLONG lastStatMs=0;
    // What (written,bytes) holds. `loaded` is false for a file that has not been
    // read since its identity last moved.
    bool loaded=false;
    bool unreadable=false;
    std::wstring text;
    std::vector<SectionSpan> spans;
    std::unordered_map<std::wstring,uint32_t> firstSpanOf;
    std::vector<std::wstring> listing;
    bool listingBuilt=false;
    // Sections parsed out of `text` and kept, one entry per name ever asked for.
    std::unordered_map<std::wstring,SectionValues> sections;
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
        // The text and the index describe the OLD file; drop them with it.
        file.loaded=false;file.unreadable=false;file.text.clear();file.text.shrink_to_fit();
        file.spans.clear();file.firstSpanOf.clear();file.listing.clear();file.listingBuilt=false;
        return false;
    }
    return true;
}
inline bool IsProfileSpace(wchar_t c) { return c==L' '||c==L'\t'; }
// The blanks the profile API strips from the edges of a value read out of a file:
// space, tab and vertical tab. A newline is not one - it ends the line first.
inline bool IsProfileBlank(wchar_t c) { return c==L' '||c==L'\t'||c==L'\v'; }

// A value's edges, exactly as the profile API hands them back. Leading and
// trailing blanks in the file are stripped; if what is left is enclosed in one
// matching pair of single or double quotes that pair is discarded and the spaces
// inside it are kept; an unmatched pair, or a pair in the wrong order, is left
// alone. So a settings file laid out in columns for readability, one a player
// padded by hand, and one where a value genuinely needs its spaces all read the
// same way the API has always read them - the cached parse has to agree with the
// API to the character, because the two answer the same callers.
inline std::wstring ProfileValue(const std::wstring& text,size_t begin,size_t end) {
    while(begin<end&&IsProfileBlank(text[begin]))++begin;
    while(end>begin&&IsProfileBlank(text[end-1]))--end;
    if(end-begin>=2) {
        const wchar_t quote=text[begin];
        if((quote==L'"'||quote==L'\'')&&text[end-1]==quote) {++begin;--end;}
    }
    return text.substr(begin,end-begin);
}
// Decoded exactly as the profile API decodes: a Unicode BOM means UTF-16, a
// UTF-8 BOM is skipped, and anything else is the process ANSI codepage. Every
// value this renderer reads is digits or hex, but a section name a player typed
// is not ours to mangle.
inline void DecodeProfileBytes(const std::string& raw,std::wstring& out) {
    const auto byte=[&raw](size_t i){return static_cast<unsigned char>(raw[i]);};
    // One resize and indexed writes: a two-megabyte settings file is decoded on a
    // crossing, so the decode is not the place for a per-character capacity check.
    if(raw.size()>=2&&byte(0)==0xFF&&byte(1)==0xFE) {
        const size_t units=(raw.size()-2)/2;
        out.resize(units);
        for(size_t i=0;i<units;++i)
            out[i]=static_cast<wchar_t>(static_cast<uint16_t>(
                byte(2+2*i)|(static_cast<uint16_t>(byte(3+2*i))<<8)));
        return;
    }
    if(raw.size()>=2&&byte(0)==0xFE&&byte(1)==0xFF) {
        const size_t units=(raw.size()-2)/2;
        out.resize(units);
        for(size_t i=0;i<units;++i)
            out[i]=static_cast<wchar_t>(static_cast<uint16_t>(
                (static_cast<uint16_t>(byte(2+2*i))<<8)|byte(3+2*i)));
        return;
    }
    size_t start=0;
    if(raw.size()>=3&&byte(0)==0xEF&&byte(1)==0xBB&&byte(2)==0xBF)start=3;
#if defined(_WIN32)
    const int count=static_cast<int>(raw.size()-start);
    if(count<=0)return;
    out.resize(static_cast<size_t>(count));
    const int got=MultiByteToWideChar(CP_ACP,0,raw.data()+start,count,out.data(),count);
    out.resize(got>0?static_cast<size_t>(got):0);
#else
    out.resize(raw.size()-start);
    for(size_t i=start;i<raw.size();++i)out[i-start]=static_cast<wchar_t>(byte(i));
#endif
}
// Where each section's lines are, in one pass over the text. Keys and values are
// NOT parsed here: that is per-section work, done when a section is asked for,
// so a file with ten thousand scopes costs one pass and not ten thousand maps.
inline void IndexProfileText(ProfileValues& file) {
    file.spans.clear();file.firstSpanOf.clear();file.listingBuilt=false;file.listing.clear();
    // Duplicate section headers merge, as they do for the profile API, and the
    // first occurrence of a key still wins - so the chain runs in file order and
    // this tail map is what appends to it. It is indexing scaffolding only.
    std::unordered_map<std::wstring,uint32_t> tailOf;
    const std::wstring& text=file.text;
    size_t at=0;
    uint32_t current=0xffffffffu;
    while(at<text.size()) {
        const size_t newline=text.find(L'\n',at);
        const size_t stop=newline==std::wstring::npos?text.size():newline;
        const size_t nextLine=stop<text.size()?stop+1:text.size();
        size_t end=stop;
        if(end>at&&text[end-1]==L'\r')--end;
        size_t begin=at;
        while(begin<end&&IsProfileSpace(text[begin]))++begin;
        at=nextLine;
        if(begin>=end||text[begin]!=L'[')continue;
        size_t close=end;
        while(close>begin&&IsProfileSpace(text[close-1]))--close;
        if(close<=begin+1||text[close-1]!=L']')continue;
        // The section that was open ends where this header starts.
        if(current!=0xffffffffu)file.spans[current].end=begin;
        std::wstring name=text.substr(begin+1,close-begin-2);
        LowerInPlace(name);
        const uint32_t index=static_cast<uint32_t>(file.spans.size());
        file.spans.push_back(SectionSpan{});
        file.spans[index].begin=at;
        const auto slot=file.firstSpanOf.find(name);
        if(slot==file.firstSpanOf.end()) {
            tailOf.emplace(name,index);
            file.firstSpanOf.emplace(std::move(name),index);
        } else {
            const auto tail=tailOf.find(slot->first);
            file.spans[tail->second].next=index;
            tail->second=index;
        }
        current=index;
    }
    if(current!=0xffffffffu)file.spans[current].end=text.size();
    file.loaded=true;
}
// The whole file, once. `unreadable` is what makes the fingerprint refuse to
// skip: a file that exists but could not be read is not the same as an empty one.
inline void EnsureProfileLoaded(ProfileValues& file,const std::wstring& path) {
    if(file.loaded||file.unreadable)return;
    ProfileIdentity(file,path);
    file.text.clear();file.spans.clear();file.firstSpanOf.clear();
    file.listing.clear();file.listingBuilt=false;file.sections.clear();
    file.unreadable=false;
    ++profileParses;
    if(file.exists) {
        std::ifstream in(std::filesystem::path(path),std::ios::binary);
        if(!in) {file.unreadable=true;file.loaded=true;return;}
        std::string raw((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
        DecodeProfileBytes(raw,file.text);
    }
    IndexProfileText(file);
}
// One section's keys and values, parsed out of the text the load already holds.
// First occurrence of a duplicate key wins, a value is kept exactly as written
// (spaces included - see src/Environment/LocationStatus.h for why that matters),
// a value that is nothing but whitespace is absent, and one matching pair of
// surrounding quotes comes off, all exactly as the profile API answers.
inline void ParseSectionSpan(const std::wstring& text,const SectionSpan& span,SectionValues& values) {
    size_t at=span.begin;
    while(at<span.end&&at<text.size()) {
        const size_t newline=text.find(L'\n',at);
        const size_t stop=newline==std::wstring::npos||newline>=span.end?span.end:newline;
        size_t end=stop;
        if(end>at&&text[end-1]==L'\r')--end;
        size_t begin=at;
        at=stop<text.size()?stop+1:span.end;
        while(begin<end&&IsProfileSpace(text[begin]))++begin;
        if(begin>=end)continue;
        if(text[begin]==L';'||text[begin]==L'#')continue;
        if(text[begin]==L'[')break; // the next header: this span is over
        const size_t equals=text.find(L'=',begin);
        if(equals==std::wstring::npos||equals>=end)continue;
        size_t keyEnd=equals;
        while(keyEnd>begin&&IsProfileSpace(text[keyEnd-1]))--keyEnd;
        if(keyEnd<=begin)continue;
        std::wstring key=text.substr(begin,keyEnd-begin);
        LowerInPlace(key);
        if(values.find(key)!=values.end())continue; // first occurrence wins
        const std::wstring value=ProfileValue(text,equals+1,end);
        // Nothing but blanks, or nothing at all: the key is present but has no
        // value to give, so this reads as absent and a caller's own default
        // answers. Every caller in this renderer passes an empty default, which is
        // also what the API returns for such a key, so the two agree.
        if(value.empty())continue;
        values.emplace(std::move(key),std::move(value));
    }
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
// One section's values, materialised out of the text the load already holds and
// kept until the file's identity moves. A miss costs that section's own few
// hundred bytes of parsing - it used to cost a whole-file parse through the
// profile API, once per section, which is what made a crossing scale with the
// size of the settings file (see ProfileValues for the measured table).
inline const SectionValues* ProfileSection(ProfileValues& file,
    const std::wstring& path,LPCWSTR section) {
    const std::wstring name=LowerKeyName(section);
    const auto found=file.sections.find(name);
    if(found!=file.sections.end())return &found->second;
    EnsureProfileLoaded(file,path);
    // A file that could not be read is not an empty file: hand the question back
    // to the profile API, which is the only authority left.
    if(file.unreadable)return nullptr;
    // The renderer asks for a bounded few dozen sections, but a hand-authored file
    // can name arbitrarily many suffixes, so the materialised map still has a
    // ceiling. Dropping it is cheap now: refilling one entry parses that section
    // out of text already in memory instead of re-reading the file.
    if(file.sections.size()>=4096)file.sections.clear();
    SectionValues values;
    const auto slot=file.firstSpanOf.find(name);
    if(slot!=file.firstSpanOf.end())
        for(uint32_t at=slot->second;at!=0xffffffffu;at=file.spans[at].next)
            ParseSectionSpan(file.text,file.spans[at],values);
    return &file.sections.emplace(std::move(name),std::move(values)).first->second;
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
// ---------------------------------------------------------------------------
// What a crossing decides, remembered.
//
// The suffix list and the fingerprint below are both pure functions of the two
// files and the place, and a crossing asks for them more than once: every settle
// asks "is what is in force still right" and an apply then asks "remember what is
// in force now". Deriving the suffix list is a walk over EVERY section name in
// both files - ten thousand of them once a player has authored per-area settings
// across a few dozen zones - and the fingerprint itself re-walks the place's
// levels for each suffix. Neither answer changes while the files do not, so both
// are kept against the identities they were derived from. A file change moves an
// identity and both miss; a border flap between two places that resolve the same
// way answers from memory for free, which is the whole point of the skip.
struct SuffixList {
    bool known=false;
    ULONGLONG localWritten=0,baseWritten=0;
    DWORD localBytes=0,baseBytes=0;
    std::vector<std::wstring> suffixes;
};
inline SuffixList& ScopedSuffixes() { static SuffixList list; return list; }
struct FingerprintMemo {
    bool used=false;
    ULONGLONG localWritten=0,baseWritten=0;
    DWORD localBytes=0,baseBytes=0;
    uint32_t mapId=0,zoneId=0,areaId=0;
    uint64_t hash=0;
};
// A crossing flaps between two or three places, so a handful of slots is enough
// to hold a walk along a border; the clock evicts round-robin when it is not.
inline std::array<FingerprintMemo,8>& FingerprintMemos() {
    static std::array<FingerprintMemo,8> memos{};
    return memos;
}
inline uint32_t& FingerprintMemoClock() { static uint32_t clock=0; return clock; }
inline void ForgetScopedAnswers() {
    ScopedSuffixes()=SuffixList{};
    for(auto& memo:FingerprintMemos())memo=FingerprintMemo{};
}
// Everything this module has read or derived from the files. A write invalidates
// it: see WriteProfile.
inline void InvalidateProfileValues() { ProfileCache().clear(); ForgetScopedAnswers(); }
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
inline std::wstring InheritKey(LPCWSTR section,LPCWSTR key) {
    return LowerKeyName((std::wstring(section)+L"."+key).c_str());
}
inline bool HasInheritGlobalAtLevel(LPCWSTR section,LPCWSTR key,unsigned level) {
    if(localFile.empty()||!editable)return false;
    const std::wstring scope=Scope(level)+L".inheritglobal";
    const auto* values=ScopedValuesFor(scope);
    if(!values)return false;
    const auto found=values->find(InheritKey(section,key));
    if(found==values->end())return false;
    return found->second!=L"0"&&found->second!=L"false";
}
inline bool HasInheritGlobal(LPCWSTR section,LPCWSTR key) {
    if(localFile.empty()||!editable)return false;
    const unsigned top=editZone||!active.areaId?1:2;
    for(int level=top;level>=1;--level) {
        const std::wstring scope=Scope(level)+L".inheritglobal";
        const auto* values=ScopedValuesFor(scope);
        if(!values)continue;
        const auto found=values->find(InheritKey(section,key));
        if(found!=values->end())
            return found->second!=L"0"&&found->second!=L"false";
    }
    return false;
}
inline bool SetInheritGlobal(LPCWSTR section,LPCWSTR key,bool inherit) {
    if(localFile.empty()||!editable||IsGlobal(section))return false;
    const std::wstring scope=Scope(editZone||!active.areaId?1:2)+L".InheritGlobal";
    const std::wstring fullKey=std::wstring(section)+L"."+key;
    const bool written=inherit
        ? WritePrivateProfileStringW(scope.c_str(),fullKey.c_str(),L"1",localFile.c_str())!=0
        : WritePrivateProfileStringW(scope.c_str(),fullKey.c_str(),nullptr,localFile.c_str())!=0;
    if(written)InvalidateScopedKeys();
    return written;
}
inline bool Sync(const std::wstring& base,const LocationContext& location,bool verified) {
    const bool changed=!active.SamePlace(location)||editable!=(location.valid&&verified)||baseFile!=base+L"GraphicsEffects.ini";
    const std::wstring nextBase=base+L"GraphicsEffects.ini",nextLocal=base+L"LocationGraphics.ini";
    // A different place does NOT invalidate what was read out of the scoped
    // file: which keys a scope has is a property of the file, not of the place
    // that is active, and this client's place flaps across a boundary while the
    // player walks it - `Area=0` and `Area=223` alternating in a real session,
    // each alternation dropping the memo and re-reading every section of the file
    // for the F7 menu and the eight tuning reloads behind it, one
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
// difference between a crossing that re-reads eight subsystems and one that does
// nothing, and it is what the log line of the next report is read for.
// A place does not have to be the SAME place to need no re-read: two areas answer
// a scoped read identically when neither of them overrides anything, which is the
// common case for the `Area=0`/`Area=223` flap this client produces walking a
// border. What decides it is therefore the VALUES a place resolves to, not its
// ids: both files' own identity plus every scoped section that belongs to the
// place's three levels, keys and values. Two places with the same fingerprint
// answer all eight subsystems the same way, so the reload is skipped and the log
// says 0 us instead of a stalled second - and the eight subsystems read this place
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
// Every section name a file holds, read from the file itself and kept per identity,
// so a section that has never been read still counts. This listing is what makes
// the skip honest rather than merely fast, and it has no ceiling.
//
// It used to come from GetPrivateProfileSectionNamesW into a fixed 32768-character
// buffer. A LocationGraphics.ini holding about a thousand scopes overflows that -
// each scope is a section name of thirty-odd characters - and an overflow returned
// nullptr, nullptr made the fingerprint unprovable, and an unprovable fingerprint
// turned the skip OFF. So the more per-zone and per-area settings a player
// authored, the more certainly every crossing paid the full re-apply; and because
// an unprovable listing is not cached either, the whole file was re-listed on
// every ask, twice per crossing. Measured at 84 KB: 1.4 ms per listing, every
// time, forever. At 2.1 MB: 46 ms.
//
// The listing now comes off the same index the load builds, so it costs nothing
// beyond the one read, it cannot be truncated, and nullptr means only "this file
// could not be read at all" - on which the caller still refuses to skip.
inline const std::vector<std::wstring>* SectionNamesOf(const std::wstring& path) {
    if(path.empty())return nullptr;
    ProfileValues& file=ProfileCache()[path];
    ProfileIdentity(file,path);
    EnsureProfileLoaded(file,path);
    if(file.unreadable)return nullptr;
    if(!file.listingBuilt) {
        file.listing.clear();
        file.listing.reserve(file.firstSpanOf.size());
        for(const auto& entry:file.firstSpanOf)file.listing.push_back(entry.first);
        file.listingBuilt=true;
    }
    return &file.listing;
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
// difference between a crossing that re-reads eight subsystems and one that does
// nothing, and it is what the log line of the next report is read for.
// A place does not have to be the SAME place to need no re-read: two areas answer
// a scoped read identically when neither of them overrides anything, which is the
// common case for the `Area=0`/`Area=223` flap this client produces walking a
// border. What decides it is therefore the VALUES a place resolves to, not its
// ids: both files' own identity plus every scoped section that belongs to the
// place's three levels, keys and values. Two places with the same fingerprint
// answer all eight subsystems the same way, so the reload is skipped and the log
// says 0 us instead of a stalled second - and the eight subsystems read this place
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
// The distinct suffixes both files name a section with, derived once per pair of
// file identities. Deriving it is a walk over EVERY section name in both files -
// ten thousand of them once a player has authored per-area settings across a few
// dozen zones - and it depends on the files alone, never on the place. A crossing
// asks twice (decide, then remember) and a walk along a border asks on every
// settle, so the walk is kept against the identities it was derived from.
// nullptr means a listing could not be proven, and the caller refuses to skip.
inline const std::vector<std::wstring>* ScopedSuffixList(ULONGLONG localWritten,DWORD localBytes,
    ULONGLONG baseWritten,DWORD baseBytes) {
    SuffixList& list=ScopedSuffixes();
    if(list.known&&list.localWritten==localWritten&&list.localBytes==localBytes
        &&list.baseWritten==baseWritten&&list.baseBytes==baseBytes)
        return &list.suffixes;
    list=SuffixList{};
    for(const std::wstring& path:{localFile,baseFile}) {
        // A file this session has no path for contributes no suffixes; it is not a
        // reason to refuse the skip, which is what returning nullptr would mean.
        if(path.empty())continue;
        const std::vector<std::wstring>* names=SectionNamesOf(path);
        if(!names)return nullptr;
        for(const std::wstring& name:*names) {
            std::wstring suffix=SectionSuffixOf(name);
            if(suffix.empty())continue;
            // A linear dedupe over the handful of suffixes a renderer has, from a
            // listing that can be arbitrarily long.
            if(std::find(list.suffixes.begin(),list.suffixes.end(),suffix)==list.suffixes.end())
                list.suffixes.push_back(std::move(suffix));
        }
    }
    // The listing is unordered, so the suffixes are sorted: the fingerprint must
    // not depend on a hash table's iteration order.
    std::sort(list.suffixes.begin(),list.suffixes.end());
    list.localWritten=localWritten;list.localBytes=localBytes;
    list.baseWritten=baseWritten;list.baseBytes=baseBytes;
    list.known=true;
    return &list.suffixes;
}
inline uint64_t ScopedFingerprint(const LocationContext& location) {
    // Scope() names a scope from the active place, and this is asked about the
    // place that settled: answer as that place and put the active one back.
    const LocationContext previous=active;
    active=location;
    uint64_t hash=1469598103934665603ull;
    bool provable=true;
    ULONGLONG localWritten=0,baseWritten=0;
    DWORD localBytes=0,baseBytes=0;
    for(const std::wstring& path:{baseFile,localFile}) {
        if(path.empty())continue;
        ProfileValues& file=ProfileCache()[path];
        // The same throttled stat a read makes, so the fingerprint and the reads
        // can never disagree about which file they are looking at - and a crossing
        // pays two stats, not one per ask.
        ProfileIdentity(file,path);
        FingerprintMix(hash,std::to_wstring(file.written));
        FingerprintMix(hash,std::to_wstring(file.bytes));
        if(path==localFile){localWritten=file.written;localBytes=file.bytes;}
        else {baseWritten=file.written;baseBytes=file.bytes;}
    }
    // Both files' identities and the place are the whole input, so an answer
    // already derived from them is the answer. This is what makes a border flap
    // and a re-settle of the same place cost nothing at all: the walk below is
    // run once per file change per place, not once per settle.
    for(const FingerprintMemo& memo:FingerprintMemos()) {
        if(!memo.used)continue;
        if(memo.localWritten!=localWritten||memo.localBytes!=localBytes)continue;
        if(memo.baseWritten!=baseWritten||memo.baseBytes!=baseBytes)continue;
        if(memo.mapId!=location.mapId||memo.zoneId!=location.zoneId||memo.areaId!=location.areaId)continue;
        active=previous;
        return memo.hash;
    }
    if(provable&&!localFile.empty()) {
        const std::vector<std::wstring>* suffixes=
            ScopedSuffixList(localWritten,localBytes,baseWritten,baseBytes);
        if(!suffixes)provable=false;
        else {
            ProfileValues& file=ProfileCache()[localFile];
            ProfileIdentity(file,localFile);
            const int top=location.areaId?2:1;
            for(const std::wstring& suffix:*suffixes) {
                // The walk ReadAtLevel makes, answered from the same cache it uses:
                // the deepest level holding a key decides its value, and whether any
                // scope holds the key at all is mixed too.
                // Gathered first, walked second: the pointers below point INTO the
                // materialised section maps, and asking for another section is the
                // one thing that could drop them. Three levels is all a place has.
                const SectionValues* levels[3]={nullptr,nullptr,nullptr};
                int levelCount=0;
                for(int level=top;level>=0;--level) {
                    const std::wstring section=LowerKeyName((Scope(level)+L"."+suffix).c_str());
                    levels[levelCount++]=ProfileSection(file,localFile,section.c_str());
                }
                std::vector<std::pair<const std::wstring*,const std::wstring*>> resolved;
                std::vector<const std::wstring*> present;
                for(int index=0;index<levelCount;++index) {
                    const SectionValues* values=levels[index];
                    if(!values)continue;
                    for(const auto& [key,value]:*values) {
                        present.push_back(&key);
                        resolved.emplace_back(&key,&value);
                    }
                }
                // Sorted by key with the level order preserved inside a key, so the
                // first entry of each run is the deepest level that holds it - which
                // is exactly the one ReadAtLevel answers with. A sort and a run of
                // pointers, where this used to be a std::map and a std::set of
                // copied strings per suffix.
                std::stable_sort(resolved.begin(),resolved.end(),
                    [](const auto& a,const auto& b){return *a.first<*b.first;});
                std::sort(present.begin(),present.end(),
                    [](const auto* a,const auto* b){return *a<*b;});
                FingerprintMix(hash,suffix);
                const std::wstring* previousKey=nullptr;
                for(const auto& [key,value]:resolved) {
                    if(previousKey&&*previousKey==*key)continue;
                    previousKey=key;
                    FingerprintMix(hash,*key);FingerprintMix(hash,*value);
                }
                if(!present.empty()) {
                    FingerprintMix(hash,L"scoped");
                    previousKey=nullptr;
                    for(const std::wstring* key:present) {
                        if(previousKey&&*previousKey==*key)continue;
                        previousKey=key;
                        FingerprintMix(hash,*key);
                    }
                }
            }
            // Mix in inheritance flags so toggling "Inherit global" marks the tuning in force as changed.
            for(int level=top;level>=1;--level) {
                const std::wstring inheritSection=LowerKeyName((Scope(level)+L".inheritglobal").c_str());
                const SectionValues* inhValues=ProfileSection(file,localFile,inheritSection.c_str());
                if(inhValues&&!inhValues->empty()) {
                    std::vector<std::pair<const std::wstring*,const std::wstring*>> sortedInh;
                    for(const auto& [k,v]:*inhValues) sortedInh.emplace_back(&k,&v);
                    std::sort(sortedInh.begin(),sortedInh.end(),
                        [](const auto& a,const auto& b){return *a.first<*b.first;});
                    FingerprintMix(hash,inheritSection);
                    for(const auto& [k,v]:sortedInh) {
                        FingerprintMix(hash,*k);FingerprintMix(hash,*v);
                    }
                }
            }
        }
    }
    if(!provable) {active=previous;return 0;}
    // Remembered against the identities it was derived from; the clock evicts
    // round-robin, and any write or path change drops the whole set.
    {
        auto& memos=FingerprintMemos();
        uint32_t& clock=FingerprintMemoClock();
        FingerprintMemo& slot=memos[clock%static_cast<uint32_t>(memos.size())];
        ++clock;
        slot.used=true;
        slot.localWritten=localWritten;slot.localBytes=localBytes;
        slot.baseWritten=baseWritten;slot.baseBytes=baseBytes;
        slot.mapId=location.mapId;slot.zoneId=location.zoneId;slot.areaId=location.areaId;
        slot.hash=hash;
    }
    active=previous;
    return hash;
}
// The fingerprint the eight subsystems were last configured with, and whether that
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
    // In verified location mode, a setting only inherits global when explicitly opted in.
    if(!IsGlobal(section)&&path&&baseFile==path&&HasGlobalValue(section,key)&&(!editable||HasInheritGlobal(section,key)))
        return ReadProfileString(GlobalScope(section).c_str(),key,fallback,output,size,localFile.c_str());
    return ReadAtLevel(section,key,fallback,output,size,path,active.areaId?2:1);
}
inline UINT ReadEditorInt(LPCWSTR section,LPCWSTR key,INT fallback,LPCWSTR path) {
    // Location editors show their own layer, or the inherited global override when opted in.
    wchar_t value[128]{};
    if((editGlobal||!editable)&&!IsGlobal(section)&&path&&baseFile==path&&HasGlobalValue(section,key))
        ReadProfileString(GlobalScope(section).c_str(),key,L"",value,128,localFile.c_str());
    else if(editGlobal)ReadProfileString(section,key,L"",value,128,path);
    else if(!IsGlobal(section)&&editable&&path&&baseFile==path&&HasInheritGlobal(section,key)&&HasGlobalValue(section,key))
        ReadProfileString(GlobalScope(section).c_str(),key,L"",value,128,localFile.c_str());
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
    if(HasGlobalValue(section,key)&&(!editable||HasInheritGlobal(section,key)))return true;
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
