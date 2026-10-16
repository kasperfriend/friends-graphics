#pragma once
// On-disk cache of compiled D3D9 shader bytecode.
//
// D3DCompile (fxc) is the renderer's one CPU cost that is paid on every
// launch for content that never changes; the water replacement stage alone
// is the largest single fxc cost in the proxy. Bytecode output is
// deterministic for a given (source, entry, profile) triple, so once a
// shader has been compiled on a machine it can be read back from disk on
// every later launch.
//
// The cache is a pure speedup, never a source of truth:
//   * a miss, a truncated file, a wrong size or a wrong shader magic all
//     fall back to a compile exactly as before;
//   * the driver's CreatePixelShader/CreateVertexShader stays the final
//     authority - GetShader recompiles once and retries when the driver
//     rejects a cached blob, so a stale or corrupt entry can at worst cost
//     one extra compile, never a missing effect;
//   * saves are atomic (unique temp file + rename), so an interrupted
//     write can never leave a torn entry behind.
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace renderer
{
    using Microsoft::WRL::ComPtr;

    namespace shaderbytecode
    {
        // D3D9 raw shader magic (not the DXBC container): "XPSF" / "XVSP".
        constexpr uint32_t kPixelMagic = 0x46535058;
        constexpr uint32_t kVertexMagic = 0x46535658;

        // FNV-1a 64 over profile + entry + source. The profile and entry
        // are part of the key so no two compile settings can share a file.
        inline uint64_t KeyHash(const char* source, const char* entry, const char* profile) noexcept
        {
            uint64_t h = 1469598103934665603ull;
            auto feed = [&](const char* s)
            {
                for (const char* p = s; *p; ++p) { h ^= static_cast<uint8_t>(*p); h *= 1099511628211ull; }
                h *= 1099511628211ull; // field separator
            };
            feed(profile);
            feed(entry);
            feed(source);
            return h;
        }

        inline std::wstring KeyPath(const std::wstring& dir, const char* source, const char* entry, const char* profile)
        {
            wchar_t hex[17]{};
            swprintf(hex, 17, L"%016llx", static_cast<unsigned long long>(KeyHash(source, entry, profile)));
            return dir + hex + ((profile && profile[0] == 'p') ? L".ps" : L".vs");
        }

        // The shared cache directory next to the DLL, always ending in a
        // separator (g_basePath normally does already).
        inline std::wstring Dir(const std::wstring& base)
        {
            if (base.empty()) return std::wstring();
            std::wstring dir = base;
            if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
            return dir + L"ShaderCache\\";
        }

#if defined(_MSC_VER)
        // The official D3D9 SDK has no "blob from memory" factory (the
        // ID3DBuffer helper only exists in Wine's headers), so the cache
        // implements the tiny ID3DBlob interface itself over an owned copy
        // of the cached file. Lifetime is plain COM: the ComPtr that Load
        // fills takes ownership via Attach and releases it away.
        class MemoryBlob final : public ID3DBlob
        {
        public:
            MemoryBlob(const void* data, size_t size)
                : m_data(static_cast<const uint8_t*>(data),
                         static_cast<const uint8_t*>(data) + static_cast<size_t>(size)),
                  m_refs(1)
            {
            }

            HRESULT QueryInterface(REFIID riid, void** ppvObject) override
            {
                if (!ppvObject) return E_POINTER;
                if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3DBlob))
                {
                    *ppvObject = static_cast<ID3DBlob*>(this);
                    AddRef();
                    return S_OK;
                }
                *ppvObject = nullptr;
                return E_NOINTERFACE;
            }

            ULONG AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }

            ULONG Release() override
            {
                const LONG refs = InterlockedDecrement(&m_refs);
                if (refs == 0) delete this;
                return static_cast<ULONG>(refs);
            }

            // Official ID3DBlob (= ID3D10Blob) accessors: no-arg, by-value.
            SIZE_T GetBufferSize() override
            {
                return static_cast<SIZE_T>(m_data.size());
            }

            LPVOID GetBufferPointer() override
            {
                return m_data.data();
            }

        private:
            std::vector<uint8_t> m_data;
            LONG m_refs;
        };
#endif

        // Reads cached bytecode into blob. False on any miss or validation
        // failure (no directory/file, too short, size or magic mismatch).
        inline bool Load(const std::wstring& dir, const char* source, const char* entry, const char* profile, ComPtr<ID3DBlob>& blob)
        {
            if (dir.empty() || !source || !entry || !profile) return false;
            const uint32_t magic = (profile[0] == 'p') ? kPixelMagic : kVertexMagic;
            std::error_code ec;
            const auto path = std::filesystem::path(KeyPath(dir, source, entry, profile));
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size < 16) return false;
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> bytes(size);
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
            if (!in) return false;
            // 8-byte header: u32 total file size, u32 shader magic. Both
            // must agree with the file before the bytecode after the header
            // is handed out (a truncated or profile-swapped entry misses).
            uint32_t declared = 0, fileMagic = 0;
            std::memcpy(&declared, bytes.data(), 4);
            std::memcpy(&fileMagic, bytes.data() + 4, 4);
            if (declared != size || fileMagic != magic) return false;
            const uint8_t* code = bytes.data() + 8;
            const uint32_t codeSize = static_cast<uint32_t>(size) - 8;
            ID3DBlob* raw = nullptr;
#if defined(_MSC_VER)
            raw = new MemoryBlob(code, codeSize);
#else
            LPVOID buffer = nullptr;
            if (FAILED(ID3DBuffer::CreateID3DBlobFromMemory(code, codeSize, &buffer))) return false;
            raw = static_cast<ID3DBlob*>(buffer);
#endif
            blob.Attach(raw);
            return true;
        }

        // Writes blob next to the other entries. Skips the write when an
        // identical entry already loads (fxc output is deterministic, so a
        // valid existing file is the same generation); otherwise writes to
        // a unique temp file and renames it into place.
        inline void Save(const std::wstring& dir, const char* source, const char* entry, const char* profile, ID3DBlob* blob)
        {
            if (dir.empty() || !source || !entry || !profile || !blob) return;
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) return;
            ComPtr<ID3DBlob> existing;
            if (Load(dir, source, entry, profile, existing)) return;
            const auto path = std::filesystem::path(KeyPath(dir, source, entry, profile));
            const std::wstring tmpName = path.wstring() + L".tmp" + std::to_wstring(GetTickCount64());
            const auto tmp = std::filesystem::path(tmpName);
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            // Same layout Load validates: u32 total size, u32 shader magic,
            // then the raw bytecode (which already starts with its own
            // XPSF/XVSP tag).
            const uint32_t codeSize = static_cast<uint32_t>(blob->GetBufferSize());
            const uint32_t declared = static_cast<uint32_t>(8 + codeSize);
            const uint32_t fileMagic = (profile[0] == 'p') ? kPixelMagic : kVertexMagic;
            out.write(reinterpret_cast<const char*>(&declared), 4);
            out.write(reinterpret_cast<const char*>(&fileMagic), 4);
            out.write(reinterpret_cast<const char*>(blob->GetBufferPointer()), static_cast<std::streamsize>(codeSize));
            out.close();
            if (!out) { std::filesystem::remove(tmp, ec); return; }
            std::filesystem::rename(tmp, path, ec);
            if (ec) std::filesystem::remove(tmp, ec);
        }

        // Compile-or-load, ending in a driver-accepted shader.
        //   1. cached blob -> Create; driver accepts: done, zero fxc;
        //   2. miss -> D3DCompile -> Save -> Create;
        //   3. driver rejects a cached blob -> recompile once (deterministic
        //      output overwrites the stale entry) and Create again.
        // create(blob) must run the device's CreatePixelShader/
        // CreateVertexShader and return true on success. On failure with a
        // non-null error, it receives the fxc diagnostics (or the create
        // failure note) so callers keep their existing error logging.
        template <typename CreateFn>
        bool GetShader(const std::wstring& dir, const char* source, const char* entry, const char* profile, ComPtr<ID3DBlob>& bytecode, CreateFn create, std::string* error = nullptr)
        {
            if (bytecode) // compiled by an earlier Ensure
            {
                if (create(bytecode.Get())) return true;
                if (error) *error = "CreatePixelShader failed";
                return false;
            }
            bool fromCache = !dir.empty() && Load(dir, source, entry, profile, bytecode);
            if (!fromCache)
            {
                ComPtr<ID3DBlob> errors;
                const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile,
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.GetAddressOf());
                if (FAILED(hr) || !bytecode)
                {
                    if (error) *error = errors && errors->GetBufferPointer()
                        ? static_cast<const char*>(errors->GetBufferPointer())
                        : "D3DCompile failed";
                    return false;
                }
                Save(dir, source, entry, profile, bytecode.Get());
            }
            if (create(bytecode.Get())) return true;
            if (!fromCache)
            {
                if (error) *error = "CreatePixelShader failed";
                return false;
            }
            // A cached blob the driver refuses means the entry is stale or
            // corrupt for this machine: the fallback is exactly the old
            // behavior (compile), and the fresh bytecode replaces the entry.
            bytecode.Reset();
            ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile,
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.GetAddressOf());
            if (FAILED(hr) || !bytecode)
            {
                if (error) *error = errors && errors->GetBufferPointer()
                    ? static_cast<const char*>(errors->GetBufferPointer())
                    : "D3DCompile failed";
                return false;
            }
            Save(dir, source, entry, profile, bytecode.Get());
            if (create(bytecode.Get())) return true;
            if (error) *error = "CreatePixelShader failed";
            return false;
        }
    }
}
