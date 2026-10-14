#include "../Environment/LocationTuning.h"
#include "MaterialCacheManager.h"
#include "TextureHashLookup.h"
#include "WicTextureLoader.h"
#include "../Core/FrameContext.h"
#include "../D3D9/CelestialTracker.h"
#include "../D3D9/ScopedRenderState.h"
#include "../D3D9/TrackedRenderState.h"

#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
    static void LogMessage(const std::wstring& logPath, const std::string& msg)
    {
        if (logPath.empty()) return;
        std::ofstream out(logPath, std::ios::app);
        if (out) out << msg << '\n';
    }

    MaterialCacheManager& MaterialCacheManager::Instance()
    {
        static MaterialCacheManager instance;
        return instance;
    }

    void MaterialCacheManager::Configure(const std::wstring& basePath)
    {
        m_basePath = basePath;
        m_logPath = basePath + L"MaterialCache.log";
        m_cacheDir = basePath + L"MaterialCache\\";

        // Initialize COM for WIC texture loader
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        ReloadTuning(basePath);

        std::wstring manifestPath = m_cacheDir + L"manifest.json";
        LoadManifest(manifestPath);
        LoadManifest(m_cacheDir + L"object_manifest.json");
        LoadManifest(m_cacheDir + L"foliage_manifest.json");

        LogMessage(m_logPath, "MaterialCacheManager configured. Total loaded material entries: " + std::to_string(m_entries.size()));
    }

    void MaterialCacheManager::ReloadTuning(const std::wstring& basePath)
    {
        std::wstring ini = basePath + L"GraphicsEffects.ini";
        m_enabled = renderer::locationtuning::ReadInt(L"AIMaterials", L"Enabled", 1, ini.c_str()) != 0;
        m_showStatus = renderer::locationtuning::ReadInt(L"AIMaterials", L"ShowStatus", 0, ini.c_str()) != 0;

        int strengthInt = renderer::locationtuning::ReadInt(L"AIMaterials", L"NormalStrengthPercent", 90, ini.c_str());
        m_globalNormalStrength = std::clamp(static_cast<float>(strengthInt) * 0.01f, 0.05f, 2.0f);

        int parallaxInt = renderer::locationtuning::ReadInt(L"AIMaterials", L"ParallaxDepthPercent", 35, ini.c_str());
        m_parallaxScale = std::clamp(static_cast<float>(parallaxInt) * 0.001f, 0.0f, 0.10f);

        int shadowInt = renderer::locationtuning::ReadInt(L"AIMaterials", L"SelfShadowStrength", 90, ini.c_str());
        m_shadowStrength = std::clamp(static_cast<float>(shadowInt) * 0.01f, 0.0f, 1.5f);

        LogMessage(m_logPath, "Tuning reloaded: enabled=" + std::to_string(m_enabled) +
                              " strength=" + std::to_string(m_globalNormalStrength) +
                              " parallax=" + std::to_string(m_parallaxScale) +
                              " shadow=" + std::to_string(m_shadowStrength));
    }

    bool MaterialCacheManager::LoadManifest(const std::wstring& manifestPath)
    {
        std::ifstream file(manifestPath);
        if (!file.is_open())
        {
            std::string p;
            for (wchar_t c : manifestPath) p += (c < 128) ? static_cast<char>(c) : '?';
            LogMessage(m_logPath, "Warning: Could not open manifest file: " + p);
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        // Lightweight parser for entries in manifest.json
        // Looks for blocks with "source_hash", "normal_path", "file_name", etc.
        size_t pos = 0;
        while ((pos = content.find("\"source_hash\"", pos)) != std::string::npos)
        {
            // Find key-values inside this entry block
            size_t blockStart = content.rfind('{', pos);
            size_t blockEnd = content.find('}', pos);
            if (blockStart == std::string::npos || blockEnd == std::string::npos)
            {
                pos += 13;
                continue;
            }

            std::string block = content.substr(blockStart, blockEnd - blockStart + 1);

            auto extractString = [&](const std::string& key) -> std::string {
                size_t kpos = block.find("\"" + key + "\"");
                if (kpos == std::string::npos) return "";
                size_t colon = block.find(':', kpos);
                if (colon == std::string::npos) return "";
                size_t q1 = block.find('\"', colon);
                if (q1 == std::string::npos) return "";
                size_t q2 = block.find('\"', q1 + 1);
                if (q2 == std::string::npos) return "";
                return block.substr(q1 + 1, q2 - q1 - 1);
            };

            auto extractFloat = [&](const std::string& key, float defaultVal) -> float {
                size_t kpos = block.find("\"" + key + "\"");
                if (kpos == std::string::npos) return defaultVal;
                size_t colon = block.find(':', kpos);
                if (colon == std::string::npos) return defaultVal;
                char* end = nullptr;
                float v = strtof(block.c_str() + colon + 1, &end);
                return (end && end != block.c_str() + colon + 1) ? v : defaultVal;
            };

            auto extractBool = [&](const std::string& key, bool defaultVal) -> bool {
                size_t kpos = block.find("\"" + key + "\"");
                if (kpos == std::string::npos) return defaultVal;
                size_t colon = block.find(':', kpos);
                if (colon == std::string::npos) return defaultVal;
                size_t value = block.find_first_not_of(" \t\r\n", colon + 1);
                if (value == std::string::npos) return defaultVal;
                if (block.compare(value, 4, "true") == 0) return true;
                if (block.compare(value, 5, "false") == 0) return false;
                return defaultVal;
            };

            std::string srcHashStr = extractString("source_hash");
            std::string rgbaHashStr = extractString("rgba_hash");
            std::string fileName = extractString("file_name");
            std::string origPath = extractString("source_path");
            if (origPath.empty()) origPath = extractString("original_path");
            std::string normalRel = extractString("normal_path");
            std::string heightRel = extractString("height_path");
            std::string materialClass = extractString("class");
            std::string materialProfile = extractString("profile");
            float strength = extractFloat("normal_strength", 0.35f);
            bool enabled = extractBool("enabled", true);

            if (!srcHashStr.empty())
            {
                uint64_t srcHash = strtoull(srcHashStr.c_str(), nullptr, 16);
                uint64_t rgbaHash = rgbaHashStr.empty() ? 0 : strtoull(rgbaHashStr.c_str(), nullptr, 16);

                std::wstring normalRelW(normalRel.begin(), normalRel.end());
                std::replace(normalRelW.begin(), normalRelW.end(), L'/', L'\\');
                // If normal_path was not set directly in manifest entry, build standard path:
                if (normalRelW.empty())
                {
                    std::wstring hW(srcHashStr.begin(), srcHashStr.end());
                    normalRelW = L"entries\\" + hW + L"\\normal.png";
                }
                std::wstring heightRelW(heightRel.begin(), heightRel.end());
                std::replace(heightRelW.begin(), heightRelW.end(), L'/', L'\\');
                if (heightRelW.empty())
                {
                    std::wstring hW(srcHashStr.begin(), srcHashStr.end());
                    heightRelW = L"entries\\" + hW + L"\\height.png";
                }

                GeneratedMaterial mat;
                mat.sourceHash = srcHash;
                mat.rgbaHash = rgbaHash;
                mat.fileName = fileName.empty() ? srcHashStr : fileName;
                mat.originalPath = origPath;
                mat.materialClass = materialClass.empty() ? "terrain" : materialClass;
                mat.materialProfile = materialProfile.empty() ? "generic" : materialProfile;
                mat.fullNormalPath = m_cacheDir + normalRelW;
                mat.fullHeightPath = m_cacheDir + heightRelW;
                mat.normalStrength = strength;
                mat.enabled = enabled;

                m_entries[srcHash] = mat;
                if (rgbaHash != 0)
                {
                    m_entries[rgbaHash] = mat;
                }
            }

            pos = blockEnd + 1;
        }

        return !m_entries.empty();
    }

    void MaterialCacheManager::Present(IDirect3DDevice9*)
    {
        if (!m_enabled) return;

        // F8 hotkey toggle
        bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId())
        {
            if (down && !m_toggleKeyDown)
            {
                m_active = !m_active;
                LogMessage(m_logPath, m_active ? "F8: AI Materials ON" : "F8: AI Materials OFF");
            }
        }
        m_toggleKeyDown = down;

        m_frameHits = 0;
        m_frameMisses = 0;
    }

    void MaterialCacheManager::Reset(IDirect3DDevice9*)
    {
        TextureHashLookup::Instance().Clear();
        for (auto& pair : m_entries)
        {
            pair.second.normalTexture.Reset();
            pair.second.heightTexture.Reset();
        }
        for (auto& shader : m_materialPS)
            shader.Reset();
        m_objectPS.Reset();
        m_shaderDevice = nullptr;
        m_neutralNormalTexture.Reset();
        m_neutralDevice = nullptr;
    }

    void MaterialCacheManager::OnSetTexture(DWORD stage, IDirect3DBaseTexture9* texture)
    {
        if (stage < 16)
        {
            m_boundTextures[stage] = texture;
        }
    }

    GeneratedMaterial* MaterialCacheManager::FindMaterialForTexture(
        IDirect3DDevice9* device,
        IDirect3DBaseTexture9* texture)
    {
        if (!m_enabled || !texture)
            return nullptr;

        uint64_t hash = TextureHashLookup::Instance().GetTextureHash(texture);
        if (!hash)
            return nullptr;

        auto it = m_entries.find(hash);
        if (it == m_entries.end())
        {
            ++m_missCount;
            ++m_frameMisses;
            return nullptr;
        }

        GeneratedMaterial& mat = it->second;
        if (!mat.enabled)
            return nullptr;

        // Lazy load GPU texture on demand
        if ((!mat.normalTexture || !mat.heightTexture) && device)
        {
            IDirect3DTexture9* rawNormal = nullptr;
            IDirect3DTexture9* rawHeight = nullptr;
            HRESULT normalHr = LoadTextureFromFileWic(device, mat.fullNormalPath, &rawNormal);
            HRESULT heightHr = LoadTextureFromFileWic(
                device, mat.fullHeightPath, &rawHeight, WicTextureUsage::LinearData);
            if (SUCCEEDED(normalHr) && rawNormal && SUCCEEDED(heightHr) && rawHeight)
            {
                mat.normalTexture.Attach(rawNormal);
                mat.heightTexture.Attach(rawHeight);
                LogMessage(m_logPath, "Loaded normal + height textures for " + mat.fileName + " (hash 0x" + std::to_string(mat.sourceHash) + ")");
            }
            else
            {
                if (rawNormal) rawNormal->Release();
                if (rawHeight) rawHeight->Release();
                std::string normalPathStr, heightPathStr;
                for (wchar_t c : mat.fullNormalPath) normalPathStr += (c < 128) ? static_cast<char>(c) : '?';
                for (wchar_t c : mat.fullHeightPath) heightPathStr += (c < 128) ? static_cast<char>(c) : '?';
                LogMessage(m_logPath, "Failed to load material textures: normal=" + normalPathStr + " height=" + heightPathStr);
                mat.enabled = false;
                return nullptr;
            }
        }

        ++m_hitCount;
        ++m_frameHits;
        m_lastHitHash = hash;
        m_lastHitName = mat.fileName;

        return &mat;
    }

    bool MaterialCacheManager::EnsureNeutralNormalTexture(IDirect3DDevice9* device)
    {
        if (m_neutralNormalTexture && m_neutralDevice == device)
            return true;

        m_neutralNormalTexture.Reset();
        m_neutralDevice = device;

        IDirect3DTexture9* tex = nullptr;
        HRESULT hr = device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
        if (FAILED(hr) || !tex)
        {
            return false;
        }

        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0)))
        {
            // Flat normal: Nx=0, Ny=0, Nz=1 -> RGB (128, 128, 255)
            // Neutral height: 0.5 -> Alpha 128
            // D3DCOLOR ARGB: A=128, R=128, G=128, B=255 -> 0x808080ff
            const uint32_t flatPixel = 0x808080ff;
            for (int y = 0; y < 2; ++y)
            {
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
                row[0] = flatPixel;
                row[1] = flatPixel;
            }
            tex->UnlockRect(0);
        }

        m_neutralNormalTexture.Attach(tex);
        return true;
    }

    static int TerrainLayerCount(uint64_t shaderHash)
    {
        // Confirmed Ascension 3.3.5 terrain receiver family. Each successive
        // shader adds one diffuse layer; the last sampler is its RGBA blend map.
        switch (shaderHash)
        {
        case 0x1259eadbfaafb43eull: return 1;
        case 0x8f5f50576e4cf36aull: return 2;
        case 0x36df746731dcdefdull: return 3;
        case 0x250ee869fc4be850ull: return 4;
        default: return 0;
        }
    }

    bool MaterialCacheManager::EnsureMaterialShader(IDirect3DDevice9* device, int layerCount)
    {
        if (!device || layerCount < 1 || layerCount > 4)
            return false;

        if (m_shaderDevice != device)
        {
            for (auto& shader : m_materialPS)
                shader.Reset();
            m_shaderDevice = device;
        }

        if (m_materialPS[layerCount])
            return true;

        std::ostringstream ss;
        for (int i = 0; i < layerCount; ++i)
            ss << "sampler2D diffuseMap" << i << " : register(s" << i << ");\n";
        ss <<
            "sampler2D blendMap : register(s4);\n"
            "sampler2D normalMap0 : register(s5);\n"
            "sampler2D normalMap1 : register(s6);\n"
            "sampler2D normalMap2 : register(s7);\n"
            "sampler2D normalMap3 : register(s8);\n"
            "sampler2D heightMap0 : register(s9);\n"
            "sampler2D heightMap1 : register(s10);\n"
            "sampler2D heightMap2 : register(s11);\n"
            "sampler2D heightMap3 : register(s12);\n"
            "float4 wowFogColor : register(c2);\n"
            "float4 lightDir : register(c8);\n"
            "float4 viewDir : register(c9);\n"
            "float4 params : register(c10);\n"
            "float4 layerCtrl : register(c11);\n"
            "float4 layerRelief : register(c12);\n"
            "struct PS_IN { float3 color0 : COLOR0; float4 color1 : COLOR1;\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "float2 uv" << i << " : TEXCOORD" << i << ";\n";
        ss << "float2 uvBlend : TEXCOORD" << layerCount << "; float fog : FOG; };\n";
        ss << "float4 main(PS_IN input) : COLOR0 {\n"
              "float2 dx0=ddx(input.uv0), dy0=ddy(input.uv0);\n";
        for (int i = 1; i < layerCount; ++i)
            ss << "float2 dx" << i << "=ddx(input.uv" << i << "), dy" << i << "=ddy(input.uv" << i << ");\n";
        ss <<
            "float mipDist=length(dx0)+length(dy0);\n"
            "float distFade=saturate(1.0f-mipDist*20.0f);\n"
            "float4 blend=tex2D(blendMap,input.uvBlend);\n";

        if (layerCount == 1)
            ss << "float4 weights=float4(1,0,0,0);\n";
        else if (layerCount == 2)
            ss << "float4 weights=float4(1-blend.r,blend.r,0,0);\n";
        else if (layerCount == 3)
            ss << "float4 weights=float4((1-blend.r)*(1-blend.g),blend.r*(1-blend.g),blend.g,0);\n";
        else
            ss << "float4 weights=float4((1-blend.r)*(1-blend.g)*(1-blend.b),blend.r*(1-blend.g)*(1-blend.b),blend.g*(1-blend.b),blend.b);\n";

        ss <<
            "float matWeight=saturate(dot(weights,layerCtrl));\n"
            "float reliefWeight=max(.20f,dot(weights,layerRelief));\n"
            "float3 V=normalize(viewDir.xyz);\n"
            "float viewSlope=min(length(V.xy)/max(V.z,0.20f),1.35f);\n"
            "float grazingFade=smoothstep(0.03f,0.12f,V.z);\n"
            "float materialDepth=viewDir.w*distFade*grazingFade*matWeight*reliefWeight;\n"
            "float2 maxOffset=(length(V.xy)>0.0001f)?-normalize(V.xy)*(materialDepth*viewSlope):float2(0,0);\n"
            "float maxShift=materialDepth*1.15f; float offsetLen=length(maxOffset);\n"
            "if(offsetLen>maxShift) maxOffset*=maxShift/offsetLen;\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "float2 finalUV" << i << "=input.uv" << i << ";\n";
        ss <<
            "if(materialDepth>0.0005f){ const int steps=12; float layerH=1.0f;\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "float2 curUV" << i << "=input.uv" << i << ", uvStep" << i << "=maxOffset/12.0f;\n";
        ss << "float mapH=0.0f;\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "mapH+=weights[" << i << "]*tex2Dgrad(heightMap" << i << ",curUV" << i << ",dx" << i << ",dy" << i << ").r;\n";
        ss << "[unroll(12)] for(int p=0;p<steps;++p){if(mapH>=layerH)break;layerH-=1.0f/12.0f;\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "curUV" << i << "+=uvStep" << i << ";\n";
        ss << "mapH=0.0f;\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "mapH+=weights[" << i << "]*tex2Dgrad(heightMap" << i << ",curUV" << i << ",dx" << i << ",dy" << i << ").r;\n";
        ss << "}\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "finalUV" << i << "=lerp(input.uv" << i << ",curUV" << i << ",layerCtrl[" << i << "]);\n";
        ss << "}\nfloat4 diffuse=float4(0,0,0,0);\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "diffuse+=weights[" << i << "]*tex2Dgrad(diffuseMap" << i << ",finalUV" << i << ",dx" << i << ",dy" << i << ");\n";
        ss << "float3 packedN=float3(0.5f,0.5f,1.0f); float surfaceH=1.0f;float surfaceRoughness=1.0f;\n"
              "if(matWeight>0.001f){packedN=float3(0,0,0);surfaceH=0.0f;surfaceRoughness=0.0f;\n";
        for (int i = 0; i < layerCount; ++i)
        {
            ss << "packedN+=weights[" << i << "]*lerp(float3(0.5f,0.5f,1.0f),tex2Dgrad(normalMap" << i << ",finalUV" << i << ",dx" << i << ",dy" << i << ").rgb,layerCtrl[" << i << "]);\n";
            ss << "surfaceH+=weights[" << i << "]*lerp(1.0f,tex2Dgrad(heightMap" << i << ",finalUV" << i << ",dx" << i << ",dy" << i << ").r,layerCtrl[" << i << "]);\n";
            ss << "surfaceRoughness+=weights[" << i << "]*lerp(1.0f,tex2Dgrad(heightMap" << i << ",finalUV" << i << ",dx" << i << ",dy" << i << ").g,layerCtrl[" << i << "]);\n";
        }
        ss << "}\nfloat viewZ=saturate(V.z);float oblique=1.0f-viewZ;\n"
              "float angleNormalBoost=lerp(1.0f,1.65f,smoothstep(0.15f,0.80f,oblique));\n"
              "float3 N=packedN*2.0f-1.0f;N.xy*=lightDir.w*angleNormalBoost*reliefWeight;N=normalize(N);float3 L=normalize(lightDir.xyz);\n"
              "float shadow=1.0f;float horizonOcclusion=0.0f;\n"
              "if(params.z>0.001f&&matWeight>0.001f&&L.z>0.05f){\n"
              "float lightXY=max(length(L.xy),0.05f);float2 rayDir=L.xy/lightXY;\n"
              "float shadowDepth=max(viewDir.w,0.040f)*distFade*matWeight;\n"
              "float rayStepLen=shadowDepth*1.8f/12.0f;float2 rayStep=rayDir*rayStepLen;float maxHorizon=-100.0f;\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "float2 shUV" << i << "=finalUV" << i << "+rayStep;\n";
        ss << "[unroll(12)]for(int s=0;s<12;++s){float sampleH=0.0f;\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "sampleH+=weights[" << i << "]*tex2Dgrad(heightMap" << i << ",shUV" << i << ",dx" << i << ",dy" << i << ").r;\n";
        ss << "float travel=rayStepLen*float(s+1);float horizon=(sampleH-surfaceH-0.012f)*shadowDepth/max(travel,0.00001f);\n"
              "maxHorizon=max(maxHorizon,horizon);\n";
        for (int i = 0; i < layerCount; ++i)
            ss << "shUV" << i << "+=rayStep;\n";
        ss << "}float lightSlope=L.z/lightXY;horizonOcclusion=smoothstep(lightSlope*0.78f,lightSlope*1.04f,maxHorizon);\n"
              "float shadowAmount=saturate(params.z/1.5f);shadow=1.0f-horizonOcclusion*shadowAmount*0.88f;}\n"
              "float ndotl=max(0.0f,dot(N,L));float baseNdotL=max(0.0f,L.z);\n"
              "float shapedHeight=smoothstep(0.08f,0.92f,surfaceH);\n"
              "float angleAO=params.w*lerp(1.0f,1.55f,smoothstep(0.15f,0.85f,oblique));\n"
              "float sourceLuma=dot(diffuse.rgb,float3(.2126f,.7152f,.0722f));\n"
              "float darkJointProtection=lerp(.35f,1.0f,smoothstep(.08f,.34f,sourceLuma));\n"
              "float ao=lerp(1.0f-angleAO*matWeight*darkJointProtection,1.0f,shapedHeight);\n"
              "float mod=max(params.x,saturate(0.5f+(ndotl*shadow-baseNdotL)*params.y*matWeight)*ao);\n"
              "float reliefMul=lerp(1.0f,2.0f*mod,distFade*matWeight);\n"
              "reliefMul*=lerp(1.0f,shadow,0.45f*distFade*matWeight);\n"
              "float detail=blend.a*0.3f+0.7f;\n"
              "float3 lit=diffuse.rgb*detail*input.color0*2.0f+diffuse.a*input.color1.rgb*blend.a;\n"
              "float3 wowColor=lerp(wowFogColor.rgb,lit,input.fog);\n"
              "float3 H=normalize(L+V);float gloss=1.0f-saturate(surfaceRoughness);\n"
              "float spec=pow(saturate(dot(N,H)),lerp(14.0f,96.0f,gloss))*gloss*gloss*0.04f*distFade*matWeight;\n"
              "return float4(wowColor*reliefMul+spec,1.0f);}\n";

        const std::string source = ss.str();

        Microsoft::WRL::ComPtr<ID3DBlob> code;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(
            source.data(), source.size(), nullptr, nullptr, nullptr,
            "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            code.GetAddressOf(), errors.GetAddressOf());

        if (FAILED(hr) || !code)
        {
            if (errors)
            {
                LogMessage(m_logPath, "Shader compilation error: " + std::string(static_cast<const char*>(errors->GetBufferPointer())));
            }
            return false;
        }

        hr = device->CreatePixelShader(
            static_cast<const DWORD*>(code->GetBufferPointer()),
            m_materialPS[layerCount].GetAddressOf());

        if (SUCCEEDED(hr))
            LogMessage(m_logPath, "Compiled layered terrain material shader: layers=" + std::to_string(layerCount));
        return SUCCEEDED(hr);
    }

    bool MaterialCacheManager::EnsureObjectShader(IDirect3DDevice9* device)
    {
        if (!device)
            return false;
        if (m_shaderDevice != device)
        {
            for (auto& shader : m_materialPS)
                shader.Reset();
            m_objectPS.Reset();
            m_shaderDevice = device;
        }
        if (m_objectPS)
            return true;

        static const char* source = R"HLSL(
sampler2D normalMap : register(s0);
sampler2D heightMap : register(s1);
sampler2D diffuseMap : register(s2);
float4 lightControl : register(c12);
float4 viewControl : register(c13);
float4 reliefControl : register(c14);
float4 surfaceControl : register(c15); // specular, seam start/end, grazing floor
float4 albedoControl : register(c16); // displaced diffuse ratio strength
struct PS_IN { float2 uv : TEXCOORD0; float2 pixel : VPOS; };
float4 main(PS_IN input) : COLOR0
{
    float2 dx=ddx(input.uv),dy=ddy(input.uv);
    float footprint=max(length(dx),length(dy));
    float distanceFade=saturate(1.0-footprint*reliefControl.w);
    float seamFade=1.0-smoothstep(surfaceControl.y,surfaceControl.z,footprint);
    float major=max(length(dx),length(dy)),minor=min(length(dx),length(dy));
    float anisotropy=minor/max(major,1e-6);
    float angleFade=lerp(surfaceControl.w,1.0,smoothstep(.04,.18,anisotropy));
    float4 packedBase=tex2Dgrad(heightMap,input.uv,dx,dy);
    float stoneWeight=lerp(1.0,packedBase.b,albedoControl.y);
    float materialDepth=lerp(albedoControl.z,1.0,stoneWeight);
    float materialResponse=lerp(albedoControl.w,1.0,stoneWeight);
    float2 lightUv=dx*lightControl.x+dy*lightControl.y;
    float lightLen=length(lightUv);
    lightUv=lightLen>1e-6?lightUv/lightLen:float2(.707,-.707);
    float2 centerDelta=float2(.5/viewControl.x-input.pixel.x,.5/viewControl.y-input.pixel.y);
    float2 viewUv=dx*centerDelta.x+dy*centerDelta.y;
    float viewLen=length(viewUv);
    viewUv=viewLen>1e-6?viewUv/viewLen:float2(0,0);
    float depth=viewControl.z*.42*distanceFade*angleFade*seamFade*materialDepth;
    float2 uv=input.uv;
    if(depth>.0002&&viewLen>1e-6){
        float2 stepUv=-viewUv*depth/6.0;float layer=1.0;
        [unroll(6)]for(int i=0;i<6;++i){
            float h=tex2Dgrad(heightMap,uv,dx,dy).r;
            if(h>=layer)break;uv+=stepUv;layer-=1.0/6.0;}}
    float surfaceH=tex2Dgrad(heightMap,uv,dx,dy).r;
    float roughness=tex2Dgrad(heightMap,uv,dx,dy).g;
    float3 N=tex2Dgrad(normalMap,uv,dx,dy).rgb*2.0-1.0;
    N.xy*=lightControl.w*materialResponse;N=normalize(N);
    float3 L=normalize(float3(lightUv,max(.28,abs(lightControl.z))));
    float horizon=-100.0;float rayDistance=max(depth,.018)*1.55;
    float2 rayStep=lightUv*rayDistance/8.0,shadowUv=uv+rayStep;
    [unroll(8)]for(int s=0;s<8;++s){
        float h=tex2Dgrad(heightMap,shadowUv,dx,dy).r;
        float travel=rayDistance*float(s+1)/8.0;
        horizon=max(horizon,(h-surfaceH-.018)*rayDistance/max(travel,1e-5));
        shadowUv+=rayStep;}
    float lightSlope=L.z/max(length(L.xy),.05);
    float occlusion=smoothstep(lightSlope*.72,lightSlope*1.02,horizon);
    float selfShadow=1.0-occlusion*saturate(viewControl.w/1.5)*.78*seamFade*materialResponse;
    float detailLight=dot(N,L)-L.z;
    float normalFactor=1.0+detailLight*reliefControl.y*distanceFade*seamFade;
    float3 baseAlbedo=tex2Dgrad(diffuseMap,input.uv,dx,dy).rgb;
    float baseLuma=dot(baseAlbedo,float3(.2126,.7152,.0722));
    // Preserve generated normal/parallax depth, but do not multiply a second
    // full cavity shadow into mortar already painted dark in the source.
    float darkJointProtection=lerp(.30,1.0,smoothstep(.09,.36,baseLuma));
    float cavity=lerp(1.0-reliefControl.z*darkJointProtection*distanceFade*seamFade*materialResponse,1.0,smoothstep(.08,.92,surfaceH));
    float3 V=normalize(float3(viewUv,max(.35,anisotropy)));
    float gloss=1.0-saturate(roughness),specPower=lerp(12.0,96.0,gloss);
    float spec=pow(saturate(dot(N,normalize(L+V))),specPower)*gloss*gloss*surfaceControl.x*distanceFade*seamFade*materialResponse;
    float3 shiftedAlbedo=tex2Dgrad(diffuseMap,uv,dx,dy).rgb;
    float3 albedoRatio=clamp((shiftedAlbedo+.025)/(baseAlbedo+.025),.34,2.35);
    float ratioWeight=albedoControl.x*distanceFade*angleFade*seamFade*materialResponse;
    albedoRatio=lerp(float3(1,1,1),albedoRatio,ratioWeight);
    float shade=max(reliefControl.x,normalFactor*cavity*selfShadow);
    float3 factor=albedoRatio*shade+spec;
    return float4(factor,1.0);
})HLSL";

        Microsoft::WRL::ComPtr<ID3DBlob> code;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        HRESULT hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","ps_3_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.GetAddressOf(),errors.GetAddressOf());
        if(FAILED(hr)||!code){
            if(errors)LogMessage(m_logPath,"Object material shader compilation error: "+
                std::string(static_cast<const char*>(errors->GetBufferPointer())));
            return false;}
        hr=device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),m_objectPS.GetAddressOf());
        if(SUCCEEDED(hr))LogMessage(m_logPath,"Compiled orientation-aware WMO/M2 object material shader");
        return SUCCEEDED(hr);
    }

    struct ObjectProfileSettings
    {
        float normal=1.0f,parallax=1.0f,shadow=1.0f,cavity=.08f;
        float specular=.04f,seamStart=.070f,seamEnd=.260f,minFactor=.55f;
        float distanceScale=7.0f,grazingFloor=.25f,albedoShift=.15f;
        float useMaterialMask=0,lowDepth=1.0f,lowResponse=1.0f;
    };

    static ObjectProfileSettings ObjectProfile(const std::string& name)
    {
        ObjectProfileSettings p;
        if(name=="stone")   {p.normal=2.0f;p.parallax=1.0f;p.shadow=.92f;p.cavity=.13f;p.specular=.025f;p.seamStart=.090f;p.seamEnd=.320f;p.minFactor=.48f;p.distanceScale=4.0f;p.grazingFloor=.48f;p.albedoShift=1.0f;}
        else if(name=="mixed"){p.normal=1.85f;p.parallax=1.0f;p.shadow=1.0f;p.cavity=.16f;p.specular=.025f;p.seamStart=.080f;p.seamEnd=.300f;p.minFactor=.42f;p.distanceScale=4.5f;p.grazingFloor=.42f;p.albedoShift=1.0f;p.useMaterialMask=1.0f;p.lowDepth=.40f;p.lowResponse=.52f;}
        else if(name=="wood"){p.normal=.72f;p.parallax=.40f;p.shadow=.55f;p.cavity=.08f;p.specular=.025f;p.seamStart=.055f;p.seamEnd=.200f;p.minFactor=.58f;p.distanceScale=7.0f;}
        else if(name=="bark"){p.normal=.58f;p.parallax=.22f;p.shadow=.42f;p.cavity=.10f;p.specular=.01f;p.seamStart=.035f;p.seamEnd=.130f;p.minFactor=.62f;p.distanceScale=8.0f;p.grazingFloor=.15f;}
        else if(name=="metal"){p.normal=.58f;p.parallax=.12f;p.shadow=.30f;p.cavity=.05f;p.specular=.22f;p.minFactor=.62f;p.distanceScale=6.0f;}
        else if(name=="plaster"){p.normal=.52f;p.parallax=.18f;p.shadow=.30f;p.cavity=.06f;p.specular=.025f;p.minFactor=.65f;p.distanceScale=7.0f;}
        else if(name=="roof"){p.normal=.72f;p.parallax=.35f;p.shadow=.50f;p.cavity=.09f;p.specular=.02f;p.minFactor=.58f;p.distanceScale=6.0f;}
        else if(name=="foliage"){p.normal=.38f;p.parallax=0;p.shadow=.16f;p.cavity=.05f;p.specular=.015f;p.seamStart=.035f;p.seamEnd=.140f;p.minFactor=.76f;p.distanceScale=8.0f;p.grazingFloor=.10f;p.albedoShift=0;}
        return p;
    }

    void MaterialCacheManager::ApplyObjectMaterial(
        IDirect3DDevice9* device,UINT primCount,const std::function<HRESULT()>& drawCall)
    {
        if(!m_enabled||!m_active||!device||!m_boundTextures[0])return;
        DWORD alphaBlend=FALSE,alphaTest=FALSE,zEnable=TRUE,zWrite=TRUE;
        DWORD srcBlend=D3DBLEND_ONE,dstBlend=D3DBLEND_ZERO;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE,&alphaBlend);
        device->GetRenderState(D3DRS_ALPHATESTENABLE,&alphaTest);
        device->GetRenderState(D3DRS_ZENABLE,&zEnable);
        device->GetRenderState(D3DRS_ZWRITEENABLE,&zWrite);
        device->GetRenderState(D3DRS_SRCBLEND,&srcBlend);
        device->GetRenderState(D3DRS_DESTBLEND,&dstBlend);

        GeneratedMaterial* material=FindMaterialForTexture(device,m_boundTextures[0]);
        if(!material||!material->normalTexture||!material->heightTexture||!zEnable)return;
        const bool replaceBlend=srcBlend==D3DBLEND_ONE&&dstBlend==D3DBLEND_ZERO;
        const bool conventionalAlpha=srcBlend==D3DBLEND_SRCALPHA&&dstBlend==D3DBLEND_INVSRCALPHA;
        if(alphaBlend&&(!zWrite||(!replaceBlend&&!conventionalAlpha))){
            static uint32_t skippedBlendLogs=0;
            if(skippedBlendLogs++<40)LogMessage(m_logPath,"[ObjectMaterialSkip] texture="+material->fileName+
                " alphaBlend="+std::to_string(alphaBlend)+" zWrite="+std::to_string(zWrite)+
                " src="+std::to_string(srcBlend)+" dst="+std::to_string(dstBlend));
            return;}
        if(!EnsureObjectShader(device))return;
        const bool foliage=material->materialClass=="foliage";
        if(alphaTest&&!foliage)return;
        const ObjectProfileSettings profile=ObjectProfile(material->materialProfile);
        D3DVIEWPORT9 viewport{};device->GetViewport(&viewport);
        if(!viewport.Width||!viewport.Height)return;

        const CelestialBody& sun=CelestialTracker::Instance().Sun();
        const CelestialBody& moon=CelestialTracker::Instance().Moon();
        const CelestialBody* body=sun.visible?&sun:(moon.visible?&moon:nullptr);
        float wx=.577f,wy=-.577f,wz=.577f;
        if(body){wx=body->worldSpaceDirection.x;wy=body->worldSpaceDirection.y;wz=body->worldSpaceDirection.z;}
        const FrameContext& frame=FrameContext::Current();
        float vx=wx,vy=wy,vz=wz;
        if(frame.cameraValid){
            vx=wx*frame.view.m[0][0]+wy*frame.view.m[1][0]+wz*frame.view.m[2][0];
            vy=wx*frame.view.m[0][1]+wy*frame.view.m[1][1]+wz*frame.view.m[2][1];
            vz=wx*frame.view.m[0][2]+wy*frame.view.m[1][2]+wz*frame.view.m[2][2];}
        float screenLen=std::sqrt(vx*vx+vy*vy+1e-6f);vx/=screenLen;vy=-vy/screenLen;
        float strength=std::clamp(material->normalStrength*(m_globalNormalStrength/.35f)*1.25f*profile.normal,.10f,2.0f);
        float c12[4]={vx,vy,vz,strength};
        float c13[4]={1.0f/float(viewport.Width),1.0f/float(viewport.Height),m_parallaxScale*profile.parallax,m_shadowStrength*profile.shadow};
        float c14[4]={profile.minFactor,1.0f,profile.cavity,profile.distanceScale};
        float c15[4]={profile.specular,profile.seamStart,profile.seamEnd,profile.grazingFloor};
        float c16[4]={profile.albedoShift,profile.useMaterialMask,profile.lowDepth,profile.lowResponse};

        DWORD zFunc=D3DCMP_LESSEQUAL,colorWrite=0xf;
        device->GetRenderState(D3DRS_ZFUNC,&zFunc);
        device->GetRenderState(D3DRS_COLORWRITEENABLE,&colorWrite);
        Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> tex0,tex1,tex2;
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> pixelShader;
        device->GetTexture(0,tex0.GetAddressOf());device->GetTexture(1,tex1.GetAddressOf());
        device->GetTexture(2,tex2.GetAddressOf());
        device->GetPixelShader(pixelShader.GetAddressOf());
        float oldC12[4]{},oldC13[4]{},oldC14[4]{},oldC15[4]{},oldC16[4]{};
        device->GetPixelShaderConstantF(12,oldC12,1);device->GetPixelShaderConstantF(13,oldC13,1);
        device->GetPixelShaderConstantF(14,oldC14,1);
        device->GetPixelShaderConstantF(15,oldC15,1);
        device->GetPixelShaderConstantF(16,oldC16,1);
        DWORD sampler[3][6]{};
        for(DWORD s=0;s<3;++s){
            device->GetSamplerState(s,D3DSAMP_ADDRESSU,&sampler[s][0]);device->GetSamplerState(s,D3DSAMP_ADDRESSV,&sampler[s][1]);
            device->GetSamplerState(s,D3DSAMP_MINFILTER,&sampler[s][2]);device->GetSamplerState(s,D3DSAMP_MAGFILTER,&sampler[s][3]);
            device->GetSamplerState(s,D3DSAMP_MIPFILTER,&sampler[s][4]);device->GetSamplerState(s,D3DSAMP_SRGBTEXTURE,&sampler[s][5]);}

        m_internalPass=true;
        device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);device->SetRenderState(D3DRS_ZFUNC,D3DCMP_EQUAL);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
        device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);device->SetRenderState(D3DRS_COLORWRITEENABLE,0x7);
        device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        for(DWORD s=0;s<3;++s){
            device->SetSamplerState(s,D3DSAMP_ADDRESSU,sampler[0][0]);device->SetSamplerState(s,D3DSAMP_ADDRESSV,sampler[0][1]);
            device->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);device->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
            device->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR);}
        device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);device->SetSamplerState(1,D3DSAMP_SRGBTEXTURE,FALSE);
        device->SetSamplerState(2,D3DSAMP_SRGBTEXTURE,sampler[0][5]);
        device->SetTexture(0,material->normalTexture.Get());device->SetTexture(1,material->heightTexture.Get());device->SetTexture(2,tex0.Get());
        device->SetPixelShader(m_objectPS.Get());device->SetPixelShaderConstantF(12,c12,1);
        device->SetPixelShaderConstantF(13,c13,1);device->SetPixelShaderConstantF(14,c14,1);
        device->SetPixelShaderConstantF(15,c15,1);device->SetPixelShaderConstantF(16,c16,1);
        drawCall();

        device->SetRenderState(D3DRS_ZWRITEENABLE,zWrite);device->SetRenderState(D3DRS_ZFUNC,zFunc);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,alphaBlend);device->SetRenderState(D3DRS_SRCBLEND,srcBlend);
        device->SetRenderState(D3DRS_DESTBLEND,dstBlend);device->SetRenderState(D3DRS_COLORWRITEENABLE,colorWrite);
        device->SetRenderState(D3DRS_ALPHATESTENABLE,alphaTest);
        device->SetTexture(0,tex0.Get());device->SetTexture(1,tex1.Get());device->SetTexture(2,tex2.Get());device->SetPixelShader(pixelShader.Get());
        device->SetPixelShaderConstantF(12,oldC12,1);device->SetPixelShaderConstantF(13,oldC13,1);
        device->SetPixelShaderConstantF(14,oldC14,1);
        device->SetPixelShaderConstantF(15,oldC15,1);
        device->SetPixelShaderConstantF(16,oldC16,1);
        for(DWORD s=0;s<3;++s){
            device->SetSamplerState(s,D3DSAMP_ADDRESSU,sampler[s][0]);device->SetSamplerState(s,D3DSAMP_ADDRESSV,sampler[s][1]);
            device->SetSamplerState(s,D3DSAMP_MINFILTER,sampler[s][2]);device->SetSamplerState(s,D3DSAMP_MAGFILTER,sampler[s][3]);
            device->SetSamplerState(s,D3DSAMP_MIPFILTER,sampler[s][4]);device->SetSamplerState(s,D3DSAMP_SRGBTEXTURE,sampler[s][5]);}
        m_internalPass=false;
        static uint32_t logged=0;
        if(logged++<40)LogMessage(m_logPath,"[ObjectMaterial] prims="+std::to_string(primCount)+" texture="+material->fileName);
    }

    void MaterialCacheManager::ApplyNormalModulation(
        IDirect3DDevice9* device,
        D3DPRIMITIVETYPE /*primitiveType*/,
        INT /*baseVertexIndex*/,
        UINT /*minVertexIndex*/,
        UINT /*numVertices*/,
        UINT /*startIndex*/,
        UINT primCount,
        const std::function<HRESULT()>& drawCall)
    {
        if (!m_enabled || !m_active || !device)
            return;

        ApplyLayeredMaterial(device, primCount, drawCall);
        return;

        // 1. Inspect bound textures on stages 0..3
        IDirect3DBaseTexture9* tex0 = m_boundTextures[0];
        IDirect3DBaseTexture9* tex1 = m_boundTextures[1];
        IDirect3DBaseTexture9* splatCandidate = m_boundTextures[2];

        GeneratedMaterial* mat0 = tex0 ? FindMaterialForTexture(device, tex0) : nullptr;
        GeneratedMaterial* mat1 = tex1 ? FindMaterialForTexture(device, tex1) : nullptr;

        // Check if neither layer has an AI material
        if ((!mat0 || !mat0->normalTexture || !mat0->heightTexture) &&
            (!mat1 || !mat1->normalTexture || !mat1->heightTexture))
        {
            return;
        }

        if (!EnsureModulationShader(device) || !EnsureNeutralNormalTexture(device))
            return;

        // Diagnostic log for the first 30 terrain draws to track multi-stage texture binding
        static uint32_t s_logCount = 0;
        if (s_logCount < 30)
        {
            ++s_logCount;
            std::string msg = "[TerrainDraw #" + std::to_string(s_logCount) + "] prims=" + std::to_string(primCount);
            if (mat0) msg += " Layer0=" + mat0->fileName;
            if (mat1) msg += " Layer1=" + mat1->fileName;
            if (splatCandidate) msg += " SplatBound=yes";
            LogMessage(m_logPath, msg);
        }

        // Tangent space light direction
        // In WoW world: X is North, Y is West, Z is Up
        // Terrain UVs: U corresponds to -Y, V corresponds to -X
        // Tangent frame: T = (0, -1, 0), B = (-1, 0, 0), N = (0, 0, 1)
        float sx = 0.577f, sy = -0.577f, sz = 0.577f;

        const CelestialBody& sun = CelestialTracker::Instance().Sun();
        const CelestialBody& moon = CelestialTracker::Instance().Moon();
        const CelestialBody* activeBody = sun.visible ? &sun : (moon.visible ? &moon : nullptr);

        if (activeBody)
        {
            sx = activeBody->worldSpaceDirection.x;
            sy = activeBody->worldSpaceDirection.y;
            sz = activeBody->worldSpaceDirection.z;
        }

        // Transform light into terrain tangent space
        float tx = -sy;
        float ty = -sx;
        float tz = std::max(0.15f, sz); // keep above horizon for terrain bump relief
        float invLen = 1.0f / std::sqrt(tx * tx + ty * ty + tz * tz + 1e-6f);
        tx *= invLen;
        ty *= invLen;
        tz *= invLen;

        // Compute tangent-space view direction towards camera from vertex shader view matrix
        float vx = 0.0f, vy = 0.0f, vz = 1.0f;
        float vsRow[3][4]{};
        if (SUCCEEDED(device->GetVertexShaderConstantF(0, vsRow[0], 3)))
        {
            float fx = vsRow[2][0];
            float fy = vsRow[2][1];
            float fz = vsRow[2][2];
            vx = fy;
            vy = fx;
            vz = std::max(0.2f, -fz);
            float invVLen = 1.0f / std::sqrt(vx * vx + vy * vy + vz * vz + 1e-6f);
            vx *= invVLen;
            vy *= invVLen;
            vz *= invVLen;
        }

        float matStrength = (mat0 && mat0->normalTexture && mat0->heightTexture)
            ? mat0->normalStrength
            : (mat1 ? mat1->normalStrength : 0.35f);
        float effectiveStrength = std::clamp(matStrength * (m_globalNormalStrength / 0.35f) * 1.5f, 0.2f, 2.5f);
        float diffuseScale = 1.0f + (m_globalNormalStrength - 0.35f) * 1.5f;

        float has0 = (mat0 && mat0->normalTexture && mat0->heightTexture) ? 1.0f : 0.0f;
        float has1 = (mat1 && mat1->normalTexture && mat1->heightTexture) ? 1.0f : 0.0f;
        float hasSplat = splatCandidate ? 1.0f : 0.0f;

        float lightConstants[4]     = { tx, ty, tz, effectiveStrength };
        float viewConstants[4]      = { vx, vy, vz, m_parallaxScale };
        float paramConstants[4]     = { 0.32f, diffuseScale, m_shadowStrength, 0.40f };
        float layerCtrlConstants[4] = { has0, has1, hasSplat, 0.0f };

        // Save original device states
        DWORD origZWrite = TRUE, origZFunc = D3DCMP_LESSEQUAL;
        DWORD origBlend = FALSE, origSrcBlend = D3DBLEND_ONE, origDestBlend = D3DBLEND_ZERO;
        DWORD origColorWrite = 0xf;
        DWORD origAlphaTest = FALSE;
        float origPSC0[4]{}, origPSC1[4]{}, origPSC2[4]{}, origPSC3[4]{};
        DWORD origAddrU0 = D3DTADDRESS_WRAP, origAddrV0 = D3DTADDRESS_WRAP;
        DWORD origAddrU1 = D3DTADDRESS_WRAP, origAddrV1 = D3DTADDRESS_WRAP;
        DWORD origAddrU2 = D3DTADDRESS_CLAMP, origAddrV2 = D3DTADDRESS_CLAMP;
        DWORD origAddrU3 = D3DTADDRESS_WRAP, origAddrV3 = D3DTADDRESS_WRAP;
        DWORD origAddrU4 = D3DTADDRESS_WRAP, origAddrV4 = D3DTADDRESS_WRAP;
        DWORD origMinFilter0 = D3DTEXF_LINEAR, origMagFilter0 = D3DTEXF_LINEAR, origMipFilter0 = D3DTEXF_LINEAR;
        DWORD origMinFilter1 = D3DTEXF_LINEAR, origMagFilter1 = D3DTEXF_LINEAR, origMipFilter1 = D3DTEXF_LINEAR;
        DWORD origMinFilter2 = D3DTEXF_LINEAR, origMagFilter2 = D3DTEXF_LINEAR, origMipFilter2 = D3DTEXF_LINEAR;
        DWORD origMinFilter3 = D3DTEXF_LINEAR, origMagFilter3 = D3DTEXF_LINEAR, origMipFilter3 = D3DTEXF_LINEAR;
        DWORD origMinFilter4 = D3DTEXF_LINEAR, origMagFilter4 = D3DTEXF_LINEAR, origMipFilter4 = D3DTEXF_LINEAR;
        Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> origTex0, origTex1, origTex2, origTex3, origTex4;
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> origPS;

        device->GetRenderState(D3DRS_ZWRITEENABLE, &origZWrite);
        device->GetRenderState(D3DRS_ZFUNC, &origZFunc);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &origBlend);
        device->GetRenderState(D3DRS_SRCBLEND, &origSrcBlend);
        device->GetRenderState(D3DRS_DESTBLEND, &origDestBlend);
        device->GetRenderState(D3DRS_COLORWRITEENABLE, &origColorWrite);
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &origAlphaTest);
        device->GetPixelShaderConstantF(0, origPSC0, 1);
        device->GetPixelShaderConstantF(1, origPSC1, 1);
        device->GetPixelShaderConstantF(2, origPSC2, 1);
        device->GetPixelShaderConstantF(3, origPSC3, 1);

        device->GetSamplerState(0, D3DSAMP_ADDRESSU, &origAddrU0);
        device->GetSamplerState(0, D3DSAMP_ADDRESSV, &origAddrV0);
        device->GetSamplerState(0, D3DSAMP_MINFILTER, &origMinFilter0);
        device->GetSamplerState(0, D3DSAMP_MAGFILTER, &origMagFilter0);
        device->GetSamplerState(0, D3DSAMP_MIPFILTER, &origMipFilter0);

        device->GetSamplerState(1, D3DSAMP_ADDRESSU, &origAddrU1);
        device->GetSamplerState(1, D3DSAMP_ADDRESSV, &origAddrV1);
        device->GetSamplerState(1, D3DSAMP_MINFILTER, &origMinFilter1);
        device->GetSamplerState(1, D3DSAMP_MAGFILTER, &origMagFilter1);
        device->GetSamplerState(1, D3DSAMP_MIPFILTER, &origMipFilter1);

        device->GetSamplerState(2, D3DSAMP_ADDRESSU, &origAddrU2);
        device->GetSamplerState(2, D3DSAMP_ADDRESSV, &origAddrV2);
        device->GetSamplerState(2, D3DSAMP_MINFILTER, &origMinFilter2);
        device->GetSamplerState(2, D3DSAMP_MAGFILTER, &origMagFilter2);
        device->GetSamplerState(2, D3DSAMP_MIPFILTER, &origMipFilter2);

        device->GetSamplerState(3, D3DSAMP_ADDRESSU, &origAddrU3);
        device->GetSamplerState(3, D3DSAMP_ADDRESSV, &origAddrV3);
        device->GetSamplerState(3, D3DSAMP_MINFILTER, &origMinFilter3);
        device->GetSamplerState(3, D3DSAMP_MAGFILTER, &origMagFilter3);
        device->GetSamplerState(3, D3DSAMP_MIPFILTER, &origMipFilter3);

        device->GetSamplerState(4, D3DSAMP_ADDRESSU, &origAddrU4);
        device->GetSamplerState(4, D3DSAMP_ADDRESSV, &origAddrV4);
        device->GetSamplerState(4, D3DSAMP_MINFILTER, &origMinFilter4);
        device->GetSamplerState(4, D3DSAMP_MAGFILTER, &origMagFilter4);
        device->GetSamplerState(4, D3DSAMP_MIPFILTER, &origMipFilter4);

        device->GetTexture(0, origTex0.GetAddressOf());
        device->GetTexture(1, origTex1.GetAddressOf());
        device->GetTexture(2, origTex2.GetAddressOf());
        device->GetTexture(3, origTex3.GetAddressOf());
        device->GetTexture(4, origTex4.GetAddressOf());
        device->GetPixelShader(origPS.GetAddressOf());

        if (origPS)
        {
            uint64_t currentPSHash = renderer::g_trackedState.psHash;
            if (m_disassembledShaders.find(currentPSHash) == m_disassembledShaders.end())
            {
                m_disassembledShaders[currentPSHash] = true;
                UINT funcSize = 0;
                if (SUCCEEDED(origPS->GetFunction(nullptr, &funcSize)) && funcSize > 0)
                {
                    std::vector<BYTE> func(funcSize);
                    if (SUCCEEDED(origPS->GetFunction(func.data(), &funcSize)))
                    {
                        Microsoft::WRL::ComPtr<ID3DBlob> disasm;
                        if (SUCCEEDED(D3DDisassemble(func.data(), funcSize, 0, nullptr, disasm.GetAddressOf())))
                        {
                            char hBuf[32];
                            sprintf_s(hBuf, "0x%llx", currentPSHash);
                            LogMessage(m_logPath, "--- WOW TERRAIN PIXEL SHADER DISASSEMBLY (Hash: " + std::string(hBuf) + ") ---");
                            LogMessage(m_logPath, static_cast<const char*>(disasm->GetBufferPointer()));
                            LogMessage(m_logPath, "--------------------------------------------------------");
                        }
                    }
                }
            }
        }

        // Apply modulation states (retired compatibility path).
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7); // RGB only
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

        device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

        device->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

        for (DWORD stage = 3; stage <= 4; ++stage)
        {
            device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        }

        IDirect3DTexture9* bindTex0 = (mat0 && mat0->normalTexture) ? mat0->normalTexture.Get() : m_neutralNormalTexture.Get();
        IDirect3DTexture9* bindTex1 = (mat1 && mat1->normalTexture) ? mat1->normalTexture.Get() : m_neutralNormalTexture.Get();
        IDirect3DBaseTexture9* bindSplat = splatCandidate ? splatCandidate : m_neutralNormalTexture.Get();
        IDirect3DTexture9* bindHeight0 = (mat0 && mat0->heightTexture) ? mat0->heightTexture.Get() : m_neutralNormalTexture.Get();
        IDirect3DTexture9* bindHeight1 = (mat1 && mat1->heightTexture) ? mat1->heightTexture.Get() : m_neutralNormalTexture.Get();

        device->SetTexture(0, bindTex0);
        device->SetTexture(1, bindTex1);
        device->SetTexture(2, bindSplat);
        device->SetTexture(3, bindHeight0);
        device->SetTexture(4, bindHeight1);

        device->SetPixelShader(m_modulationPS.Get());
        device->SetPixelShaderConstantF(0, lightConstants, 1);
        device->SetPixelShaderConstantF(1, viewConstants, 1);
        device->SetPixelShaderConstantF(2, paramConstants, 1);
        device->SetPixelShaderConstantF(3, layerCtrlConstants, 1);

        // Execute modulation draw
        drawCall();

        // Restore original states
        device->SetRenderState(D3DRS_ZWRITEENABLE, origZWrite);
        device->SetRenderState(D3DRS_ZFUNC, origZFunc);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, origBlend);
        device->SetRenderState(D3DRS_SRCBLEND, origSrcBlend);
        device->SetRenderState(D3DRS_DESTBLEND, origDestBlend);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, origColorWrite);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, origAlphaTest);
        device->SetPixelShaderConstantF(0, origPSC0, 1);
        device->SetPixelShaderConstantF(1, origPSC1, 1);
        device->SetPixelShaderConstantF(2, origPSC2, 1);
        device->SetPixelShaderConstantF(3, origPSC3, 1);

        device->SetSamplerState(0, D3DSAMP_ADDRESSU, origAddrU0);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, origAddrV0);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, origMinFilter0);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, origMagFilter0);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, origMipFilter0);

        device->SetSamplerState(1, D3DSAMP_ADDRESSU, origAddrU1);
        device->SetSamplerState(1, D3DSAMP_ADDRESSV, origAddrV1);
        device->SetSamplerState(1, D3DSAMP_MINFILTER, origMinFilter1);
        device->SetSamplerState(1, D3DSAMP_MAGFILTER, origMagFilter1);
        device->SetSamplerState(1, D3DSAMP_MIPFILTER, origMipFilter1);

        device->SetSamplerState(2, D3DSAMP_ADDRESSU, origAddrU2);
        device->SetSamplerState(2, D3DSAMP_ADDRESSV, origAddrV2);
        device->SetSamplerState(2, D3DSAMP_MINFILTER, origMinFilter2);
        device->SetSamplerState(2, D3DSAMP_MAGFILTER, origMagFilter2);
        device->SetSamplerState(2, D3DSAMP_MIPFILTER, origMipFilter2);

        device->SetSamplerState(3, D3DSAMP_ADDRESSU, origAddrU3);
        device->SetSamplerState(3, D3DSAMP_ADDRESSV, origAddrV3);
        device->SetSamplerState(3, D3DSAMP_MINFILTER, origMinFilter3);
        device->SetSamplerState(3, D3DSAMP_MAGFILTER, origMagFilter3);
        device->SetSamplerState(3, D3DSAMP_MIPFILTER, origMipFilter3);

        device->SetSamplerState(4, D3DSAMP_ADDRESSU, origAddrU4);
        device->SetSamplerState(4, D3DSAMP_ADDRESSV, origAddrV4);
        device->SetSamplerState(4, D3DSAMP_MINFILTER, origMinFilter4);
        device->SetSamplerState(4, D3DSAMP_MAGFILTER, origMagFilter4);
        device->SetSamplerState(4, D3DSAMP_MIPFILTER, origMipFilter4);

        device->SetTexture(0, origTex0.Get());
        device->SetTexture(1, origTex1.Get());
        device->SetTexture(2, origTex2.Get());
        device->SetTexture(3, origTex3.Get());
        device->SetTexture(4, origTex4.Get());
        device->SetPixelShader(origPS.Get());
    }


    void MaterialCacheManager::ApplyLayeredMaterial(
        IDirect3DDevice9* device,
        UINT primCount,
        const std::function<HRESULT()>& drawCall)
    {
        const int layerCount = TerrainLayerCount(renderer::g_trackedState.psHash);
        if (layerCount == 0)
            return;

        IDirect3DBaseTexture9* gameTextures[5]{};
        GeneratedMaterial* materials[4]{};
        bool anyMaterial = false;
        for (int i = 0; i <= layerCount; ++i)
            gameTextures[i] = m_boundTextures[i];
        if (!gameTextures[layerCount])
            return;

        for (int i = 0; i < layerCount; ++i)
        {
            materials[i] = gameTextures[i] ? FindMaterialForTexture(device, gameTextures[i]) : nullptr;
            anyMaterial = anyMaterial ||
                (materials[i] && materials[i]->normalTexture && materials[i]->heightTexture);
        }
        if (!anyMaterial || !EnsureMaterialShader(device, layerCount) || !EnsureNeutralNormalTexture(device))
            return;

        static uint32_t s_layerLogCount = 0;
        if (s_layerLogCount < 30)
        {
            ++s_layerLogCount;
            std::string msg = "[LayeredTerrain #" + std::to_string(s_layerLogCount) + "] prims=" +
                std::to_string(primCount) + " layers=" + std::to_string(layerCount);
            for (int i = 0; i < layerCount; ++i)
                if (materials[i]) msg += " L" + std::to_string(i) + "=" + materials[i]->fileName;
            LogMessage(m_logPath, msg);
        }

        float sx = 0.577f, sy = -0.577f, sz = 0.577f;
        const CelestialBody& sun = CelestialTracker::Instance().Sun();
        const CelestialBody& moon = CelestialTracker::Instance().Moon();
        const CelestialBody* activeBody = sun.visible ? &sun : (moon.visible ? &moon : nullptr);
        if (activeBody)
        {
            sx = activeBody->worldSpaceDirection.x;
            sy = activeBody->worldSpaceDirection.y;
            sz = activeBody->worldSpaceDirection.z;
        }

        float tx = -sy, ty = -sx, tz = std::max(0.15f, sz);
        float invLen = 1.0f / std::sqrt(tx * tx + ty * ty + tz * tz + 1e-6f);
        tx *= invLen; ty *= invLen; tz *= invLen;

        float vx = 0.0f, vy = 0.0f, vz = 1.0f;
        float vsRow[3][4]{};
        if (SUCCEEDED(device->GetVertexShaderConstantF(0, vsRow[0], 3)))
        {
            vx = vsRow[2][1];
            vy = vsRow[2][0];
            vz = std::max(0.2f, -vsRow[2][2]);
            float invVLen = 1.0f / std::sqrt(vx * vx + vy * vy + vz * vz + 1e-6f);
            vx *= invVLen; vy *= invVLen; vz *= invVLen;
        }

        float matStrength = 0.35f;
        for (int i = 0; i < layerCount; ++i)
            if (materials[i]) { matStrength = materials[i]->normalStrength; break; }
        const float effectiveStrength = std::clamp(
            matStrength * (m_globalNormalStrength / 0.35f) * 1.5f, 0.2f, 2.5f);
        const float diffuseScale = 1.15f + (m_globalNormalStrength - 0.35f) * 1.75f;

        float lightConstants[4] = { tx, ty, tz, effectiveStrength };
        float viewConstants[4] = { vx, vy, vz, m_parallaxScale };
        // Height-derived cavity is intentionally subtle and survives diffuse
        // light without turning the hand-painted WoW albedo into black mortar.
        float paramConstants[4] = { 0.24f, diffuseScale, m_shadowStrength, 0.22f };
        float layerCtrlConstants[4]{};
        float layerReliefConstants[4]{};
        for (int i = 0; i < layerCount; ++i)
        {
            layerCtrlConstants[i] = materials[i] && materials[i]->normalTexture && materials[i]->heightTexture
                ? 1.0f : 0.0f;
            if (layerCtrlConstants[i] > 0.0f)
                layerReliefConstants[i] = materials[i]->materialProfile == "stone" ? 1.28f
                    : (materials[i]->materialProfile == "mixed" ? 1.12f : 1.0f);
        }

        DWORD origZWrite = TRUE, origZFunc = D3DCMP_LESSEQUAL;
        DWORD origBlend = FALSE, origSrcBlend = D3DBLEND_ONE, origDestBlend = D3DBLEND_ZERO;
        DWORD origColorWrite = 0xf, origAlphaTest = FALSE;
        float origPSC8[4]{}, origPSC9[4]{}, origPSC10[4]{}, origPSC11[4]{}, origPSC12[4]{};
        DWORD origAddrU[13]{}, origAddrV[13]{}, origMinFilter[13]{}, origMagFilter[13]{}, origMipFilter[13]{};
        Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> origTex[13];
        Microsoft::WRL::ComPtr<IDirect3DPixelShader9> origPS;

        device->GetRenderState(D3DRS_ZWRITEENABLE, &origZWrite);
        device->GetRenderState(D3DRS_ZFUNC, &origZFunc);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &origBlend);
        device->GetRenderState(D3DRS_SRCBLEND, &origSrcBlend);
        device->GetRenderState(D3DRS_DESTBLEND, &origDestBlend);
        device->GetRenderState(D3DRS_COLORWRITEENABLE, &origColorWrite);
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &origAlphaTest);
        device->GetPixelShaderConstantF(8, origPSC8, 1);
        device->GetPixelShaderConstantF(9, origPSC9, 1);
        device->GetPixelShaderConstantF(10, origPSC10, 1);
        device->GetPixelShaderConstantF(11, origPSC11, 1);
        device->GetPixelShaderConstantF(12, origPSC12, 1);
        for (DWORD stage = 0; stage < 13; ++stage)
        {
            device->GetSamplerState(stage, D3DSAMP_ADDRESSU, &origAddrU[stage]);
            device->GetSamplerState(stage, D3DSAMP_ADDRESSV, &origAddrV[stage]);
            device->GetSamplerState(stage, D3DSAMP_MINFILTER, &origMinFilter[stage]);
            device->GetSamplerState(stage, D3DSAMP_MAGFILTER, &origMagFilter[stage]);
            device->GetSamplerState(stage, D3DSAMP_MIPFILTER, &origMipFilter[stage]);
            device->GetTexture(stage, origTex[stage].GetAddressOf());
        }
        device->GetPixelShader(origPS.GetAddressOf());

        m_internalPass = true;
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        for (DWORD stage = 0; stage < 13; ++stage)
        {
            const DWORD address = stage == 4 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
            device->SetSamplerState(stage, D3DSAMP_ADDRESSU, address);
            device->SetSamplerState(stage, D3DSAMP_ADDRESSV, address);
            device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        }

        for (int i = 0; i < layerCount; ++i)
            device->SetTexture(i, gameTextures[i]);
        device->SetTexture(4, gameTextures[layerCount]);
        for (int i = 0; i < 4; ++i)
        {
            IDirect3DTexture9* normal = materials[i] && materials[i]->normalTexture
                ? materials[i]->normalTexture.Get() : m_neutralNormalTexture.Get();
            IDirect3DTexture9* height = materials[i] && materials[i]->heightTexture
                ? materials[i]->heightTexture.Get() : m_neutralNormalTexture.Get();
            device->SetTexture(5 + i, normal);
            device->SetTexture(9 + i, height);
        }

        device->SetPixelShader(m_materialPS[layerCount].Get());
        device->SetPixelShaderConstantF(8, lightConstants, 1);
        device->SetPixelShaderConstantF(9, viewConstants, 1);
        device->SetPixelShaderConstantF(10, paramConstants, 1);
        device->SetPixelShaderConstantF(11, layerCtrlConstants, 1);
        device->SetPixelShaderConstantF(12, layerReliefConstants, 1);
        drawCall();

        device->SetRenderState(D3DRS_ZWRITEENABLE, origZWrite);
        device->SetRenderState(D3DRS_ZFUNC, origZFunc);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, origBlend);
        device->SetRenderState(D3DRS_SRCBLEND, origSrcBlend);
        device->SetRenderState(D3DRS_DESTBLEND, origDestBlend);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, origColorWrite);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, origAlphaTest);
        device->SetPixelShaderConstantF(8, origPSC8, 1);
        device->SetPixelShaderConstantF(9, origPSC9, 1);
        device->SetPixelShaderConstantF(10, origPSC10, 1);
        device->SetPixelShaderConstantF(11, origPSC11, 1);
        device->SetPixelShaderConstantF(12, origPSC12, 1);
        for (DWORD stage = 0; stage < 13; ++stage)
        {
            device->SetSamplerState(stage, D3DSAMP_ADDRESSU, origAddrU[stage]);
            device->SetSamplerState(stage, D3DSAMP_ADDRESSV, origAddrV[stage]);
            device->SetSamplerState(stage, D3DSAMP_MINFILTER, origMinFilter[stage]);
            device->SetSamplerState(stage, D3DSAMP_MAGFILTER, origMagFilter[stage]);
            device->SetSamplerState(stage, D3DSAMP_MIPFILTER, origMipFilter[stage]);
            device->SetTexture(stage, origTex[stage].Get());
        }
        device->SetPixelShader(origPS.Get());
        m_internalPass = false;
    }

    void MaterialCacheManager::DrawDebugOverlay(IDirect3DDevice9* device)
    {
        if (!m_enabled || !m_showStatus || !device)
            return;

        try
        {
            const char* label = m_active ? "AIMAT ON" : "AIMAT OFF";
            const char* letters = "WATERONFIHZXMAT BC";
            const char* glyphs[] = {
                "10001100011000110101101011101110001", // W
                "01110100011000111111100011000110001", // A
                "11111001000010000100001000010000100", // T
                "11111100001000011110100001000011111", // E
                "11110100011000111110101001001010001", // R
                "01110100011000110001100011000101110", // O
                "10001110011010110011100011000110001", // N
                "11111100001000011110100001000010000", // F
                "11111001000010000100001000010011111", // I
                "10001100011000111111100011000110001", // H
                "11111000010001000100010001000011111", // Z
                "10001100010101000100010101000110001", // X
                "10001110111010110001100011000110001", // M
                "01110100011000111111100011000110001", // A
                "11111001000010000100001000010000100", // T
                "00000000000000000000000000000000000", // ' '
                "11110100011000111110100011000111110", // B
                "01110100011000010000100001000101110"  // C
            };

            auto rect = [&](float x, float y, float width, float height, DWORD color) {
                y += 28.0f * 2.0f; // line 2 below Water/Haze status
                D3DRECT r{ static_cast<LONG>(x), static_cast<LONG>(y),
                           static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
                device->Clear(1, &r, D3DCLEAR_TARGET, color, 1.0f, 0);
            };

            rect(16, 40, 160, 25, 0xff101820);
            DWORD color = m_active ? 0xff44ddff : 0xff888888; // bright cyan when ON, gray when OFF

            for (unsigned i = 0; label[i]; ++i)
            {
                const char* found = strchr(letters, label[i]);
                if (!found) continue;
                auto glyph = glyphs[found - letters];
                for (int y = 0; y < 7; ++y)
                    for (int x = 0; x < 5; ++x)
                        if (glyph[y * 5 + x] == '1')
                            rect(22.0f + i * 12.0f + x * 2.0f, 46.0f + y * 2.0f, 2, 2, color);
            }
        }
        catch (...) {}
    }
}
