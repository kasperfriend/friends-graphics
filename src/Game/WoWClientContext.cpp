#include "WoWClientContext.h"
#include <bcrypt.h>
#include <array>
#include <limits>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>
#pragma comment(lib,"bcrypt.lib")
namespace renderer {
bool WoWClientContext::Read(uintptr_t rva,void* value,size_t size) const {
    if(!base||!size||rva>=imageSize||size>imageSize-rva||
       rva>std::numeric_limits<uintptr_t>::max()-base)return false;
    return ReadAddress(base+rva,value,size);
}
bool WoWClientContext::ReadAddress(uintptr_t address,void* value,size_t size) const {
    if(!base||!address||!value||!size||
       size>std::numeric_limits<uintptr_t>::max()-address)return false;
    MEMORY_BASIC_INFORMATION region{};
    if(VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region))!=sizeof(region)||
       region.State!=MEM_COMMIT||(region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const DWORD readable=PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
        PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
    if(!(region.Protect&readable))return false;
    const uintptr_t regionBase=reinterpret_cast<uintptr_t>(region.BaseAddress);
    if(address<regionBase)return false;
    const uintptr_t regionOffset=address-regionBase;
    if(regionOffset>=region.RegionSize||size>region.RegionSize-regionOffset)return false;
    SIZE_T copied=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),value,size,&copied)&&copied==size;
}
bool WoWClientContext::Detect() {
    base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));imageSize=4096;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS32 nt{};
    if(!Read(0,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||!Read(dos.e_lfanew,&nt,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC) {base=0;return false;}
    imageSize=nt.OptionalHeader.SizeOfImage;timestamp=nt.FileHeader.TimeDateStamp;
    wchar_t path[32768]{};
    if(GetModuleFileNameW(nullptr,path,32768))modulePath=path;
    return true;
}
void WoWClientContext::EnsureSha256(const std::wstring& cachePath) {
    if(!base||sha256State==Sha256State::Computed)return;
    // The key changes with any client swap: a rebuild moves the timestamp,
    // a different build changes the image size, a different install the path.
    const std::wstring key=std::to_wstring(timestamp)+L"|"+std::to_wstring(imageSize)+L"|"+modulePath;
    if(!cachePath.empty()) {
        std::wifstream in{std::filesystem::path(cachePath)};
        std::wstring cachedKey,cachedHash;
        if(std::getline(in,cachedKey)&&std::getline(in,cachedHash)&&cachedKey==key&&cachedHash.size()==64) {
            sha256=std::string(cachedHash.begin(),cachedHash.end());
            sha256State=Sha256State::Computed;
            return;
        }
    }
    std::ifstream file{std::filesystem::path(modulePath),std::ios::binary};
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) {sha256State=Sha256State::Failed;return;}
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0) {
        std::array<char,65536> buffer;bool ok=bool(file);
        while(file.read(buffer.data(),buffer.size())||file.gcount()) if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),static_cast<ULONG>(file.gcount()),0)<0)ok=false;
        if(file.bad())ok=false;
        std::array<unsigned char,32> digest{};
        if(ok&&BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0) {
            std::ostringstream result;result<<std::hex<<std::setfill('0');for(auto b:digest)result<<std::setw(2)<<unsigned(b);
            sha256=result.str();
            sha256State=Sha256State::Computed;
            std::wofstream out{std::filesystem::path(cachePath),std::ios::trunc};
            out<<key<<L'\n'<<std::wstring(sha256.begin(),sha256.end())<<L'\n';
        } else sha256State=Sha256State::Failed;
        BCryptDestroyHash(hash);
    } else sha256State=Sha256State::Failed;
    BCryptCloseAlgorithmProvider(algorithm,0);
}
}
