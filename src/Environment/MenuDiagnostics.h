#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdio>
#include <string>
namespace renderer::menudiagnostics {
inline std::wstring file;
inline std::string TimeStamp() {
    SYSTEMTIME t{};GetSystemTime(&t);
    char prefix[32]{};
    snprintf(prefix,sizeof prefix,"%04d-%02d-%02d %02d:%02d:%02d UTC ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    return prefix;
}
inline void RotateIfNeeded() {
    std::error_code error;
    if(std::filesystem::file_size(file,error)>1024*1024&&!error) {
        auto previous=file+L".previous";
        std::filesystem::remove(previous,error);
        std::filesystem::rename(file,previous,error);
    }
}
inline void Write(const std::string& message) {
    if(file.empty())return;
    RotateIfNeeded();
    std::ofstream out{std::filesystem::path(file),std::ios::app};
    out<<TimeStamp()<<message<<'\n';
}
// One open for a whole report: the menu report is a dozen lines written in a
// single burst on every zone change, and one file open per line turned that
// burst into a dozen open/close round trips.
inline void WriteBatch(const std::vector<std::string>& messages) {
    if(file.empty()||messages.empty())return;
    RotateIfNeeded();
    std::ofstream out{std::filesystem::path(file),std::ios::app};
    const std::string stamp=TimeStamp();
    for(const auto& message:messages)out<<stamp<<message<<'\n';
}
inline void Configure(const std::wstring& base) {
    file=base+L"MenuDiagnostics.log";
    Write("SESSION menu-fix-v2-cloud-light-v1 build=" __DATE__ " " __TIME__);
}
}
