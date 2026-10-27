#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
namespace renderer {
struct WoWClientContext {
    uintptr_t base=0;
    uint32_t imageSize=0,timestamp=0;
    // Full path of the main module; part of the on-disk hash cache key.
    std::wstring modulePath;
    // Set by EnsureSha256(); Detect() is header-only and never hashes the
    // whole executable.
    std::string sha256;
    enum class Sha256State { NotComputed, Computed, Failed };
    Sha256State sha256State=Sha256State::NotComputed;
    // Header detection: PE parse + identity (a few small reads, no file hashing).
    bool Detect();
    // Ensures sha256 is set. Hashing the whole executable (hundreds of MB)
    // is the single most expensive startup step, so the result is cached in
    // a small text file keyed by the module path + PE identity; a repeat
    // launch of the same client reads a few dozen bytes instead of hashing.
    void EnsureSha256(const std::wstring& cachePath);
    bool Read(uintptr_t rva,void* value,size_t size) const;
    // Read a client-owned pointer target after its RVA has been resolved.
    // This remains read-only and checks the containing committed region before
    // calling ReadProcessMemory; it never follows a pointer supplied by config.
    bool ReadAddress(uintptr_t address,void* value,size_t size) const;
};
}
