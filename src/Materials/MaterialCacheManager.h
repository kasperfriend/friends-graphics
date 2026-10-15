#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

namespace renderer
{
    struct GeneratedMaterial
    {
        uint64_t sourceHash = 0;
        uint64_t rgbaHash = 0;
        std::string fileName;
        std::string originalPath;
        std::string materialClass = "terrain";
        std::string materialProfile = "generic";
        std::wstring fullNormalPath;
        std::wstring fullHeightPath;
        float normalStrength = 0.35f;
        bool enabled = true;
        Microsoft::WRL::ComPtr<IDirect3DTexture9> normalTexture;
        Microsoft::WRL::ComPtr<IDirect3DTexture9> heightTexture;
    };

    class MaterialCacheManager
    {
    public:
        static MaterialCacheManager& Instance();

        void Configure(const std::wstring& basePath);
        void ReloadTuning(const std::wstring& basePath);
        void Present(IDirect3DDevice9* device);
        void Reset(IDirect3DDevice9* device);

        void OnSetTexture(DWORD stage, IDirect3DBaseTexture9* texture);

        GeneratedMaterial* FindMaterialForTexture(IDirect3DDevice9* device, IDirect3DBaseTexture9* texture);

        bool IsEnabled() const { return m_enabled; }
        bool IsActive() const { return m_enabled && m_active; }
        bool IsInternalPass() const { return m_internalPass; }
        float GetNormalStrength() const { return m_globalNormalStrength; }

        void ApplyNormalModulation(
            IDirect3DDevice9* device,
            D3DPRIMITIVETYPE primitiveType,
            INT baseVertexIndex,
            UINT minVertexIndex,
            UINT numVertices,
            UINT startIndex,
            UINT primCount,
            const std::function<HRESULT()>& drawCall);

        void ApplyObjectMaterial(
            IDirect3DDevice9* device,
            UINT primCount,
            const std::function<HRESULT()>& drawCall);

        void DrawDebugOverlay(IDirect3DDevice9* device);

    private:
        MaterialCacheManager() = default;

        bool LoadManifest(const std::wstring& manifestPath);
        bool EnsureMaterialShader(IDirect3DDevice9* device, int layerCount);
        bool EnsureObjectShader(IDirect3DDevice9* device);
        bool EnsureModulationShader(IDirect3DDevice9*) { return false; } // retired compatibility path
        bool EnsureNeutralNormalTexture(IDirect3DDevice9* device);
        void ApplyLayeredMaterial(
            IDirect3DDevice9* device,
            UINT primCount,
            const std::function<HRESULT()>& drawCall);

        bool m_enabled = false; // experimental; fail closed until explicitly enabled
        bool m_active = true;
        bool m_showStatus = false;
        bool m_toggleKeyDown = false;
        bool m_internalPass = false;
        float m_globalNormalStrength = 0.90f;
        float m_parallaxScale = 0.035f;
        float m_shadowStrength = 0.90f;

        std::wstring m_basePath;
        std::wstring m_cacheDir;
        std::wstring m_logPath;

        std::unordered_map<uint64_t, GeneratedMaterial> m_entries;
        IDirect3DBaseTexture9* m_boundTextures[16]{};

        // Shader and helper resources
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> m_materialPS[5];
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> m_modulationPS; // retired compatibility path
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> m_objectPS;
        IDirect3DDevice9* m_shaderDevice = nullptr;
        Microsoft::WRL::ComPtr<IDirect3DTexture9> m_neutralNormalTexture;
        IDirect3DDevice9* m_neutralDevice = nullptr;
        std::unordered_map<uint64_t, bool> m_disassembledShaders;

        // Statistics
        uint64_t m_hitCount = 0;
        uint64_t m_missCount = 0;
        uint64_t m_lastHitHash = 0;
        std::string m_lastHitName;
        uint32_t m_frameHits = 0;
        uint32_t m_frameMisses = 0;
    };
}
