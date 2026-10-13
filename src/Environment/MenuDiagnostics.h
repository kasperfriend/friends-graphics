#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
namespace renderer::menudiagnostics {
inline std::wstring file;
inline void Write(const std::string& message) {
    if(file.empty())return;
    std::error_code error;
    if(std::filesystem::file_size(file,error)>1024*1024&&!error) {
        auto previous=file+L".previous";
        std::filesystem::remove(previous,error);
        std::filesystem::rename(file,previous,error);
    }
    SYSTEMTIME t{};GetSystemTime(&t);
    std::ofstream out{std::filesystem::path(file),std::ios::app};
    out<<t.wYear<<'-'<<t.wMonth<<'-'<<t.wDay<<' '<<t.wHour<<':'<<t.wMinute<<':'<<t.wSecond<<" UTC "<<message<<'\n';
}
inline void Configure(const std::wstring& base) {
    file=base+L"MenuDiagnostics.log";
    Write("SESSION menu-fix-v2-cloud-light-v1 build=" __DATE__ " " __TIME__);
}
}
