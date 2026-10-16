#pragma once
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../Diagnostics/FileSink.h"
namespace renderer::menudiagnostics {
inline std::wstring file;
inline std::string TimeStamp() {
    SYSTEMTIME t{};GetSystemTime(&t);
    char prefix[32]{};
    snprintf(prefix,sizeof prefix,"%04d-%02d-%02d %02d:%02d:%02d UTC ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    return prefix;
}
// This report is written on EVERY confirmed place, and a confirmed place is not a
// settled one: walking a border flaps this client's idea of where the player is,
// and most of those alternations re-apply nothing at all. It used to stat the file
// for rotation and then open, write and close it on every one of them - a dozen
// lines' worth of I/O in the game folder, on the frame the player is walking on,
// for a crossing that changed nothing. It now buffers into the shared sink, so the
// report still lands in full and in order, in the one open Present already makes.
inline void Write(const std::string& message) {
    if(file.empty())return;
    renderer::filesink::AppendLine(file,TimeStamp()+message);
}
// One buffer for a whole report: the menu report is a dozen lines written in a
// single burst on every zone change, and one file open per line turned that burst
// into a dozen open/close round trips.
inline void WriteBatch(const std::vector<std::string>& messages) {
    if(file.empty()||messages.empty())return;
    const std::string stamp=TimeStamp();
    std::string batch;
    for(const auto& message:messages){batch+=stamp;batch+=message;batch+='\n';}
    renderer::filesink::Append(file,batch);
}
inline void Configure(const std::wstring& base) {
    file=base+L"MenuDiagnostics.log";
    // The README tells a supporter this file rotates at 1 MiB, and that the
    // rotated half is called MenuDiagnostics.log.previous - both are the sink's
    // business now, so both are declared here rather than left to its defaults.
    renderer::filesink::SetRotationLimit(file,1024*1024);
    renderer::filesink::SetRotationSuffix(file,L".previous");
    Write("SESSION menu-fix-v2-cloud-light-v1 build=" __DATE__ " " __TIME__);
}
}
