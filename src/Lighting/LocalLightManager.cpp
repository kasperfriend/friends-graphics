#include "../Environment/LocationTuning.h"
#include "LocalLightManager.h"
#include "LocalLightSelection.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <wrl/client.h>
#include <cstring>
#include "../Core/FrameContext.h"
#include "../D3D9/TrackedRenderState.h"
#include "../D3D9/CameraContinuity.h"
#include "../D3D9/TextureHashLookup.h"
#include "TorchSignatures.h"

namespace renderer
{
    namespace
    {
        // Particle draws may be skipped briefly when a carried flame is close
        // to the character silhouette or crosses the frustum edge. Keep the
        // already-confirmed emitter stable across those gaps, then fade it out
        // slowly enough that unequipping a torch does not cause a hard pop.
        constexpr ULONGLONG kAttachedHoldMs = 1000;
        constexpr ULONGLONG kAttachedLifetimeMs = 3500;

        uint64_t StableManifestId(const std::string& record)
        {
            // Position-only IDs collide for intentionally layered emitters at
            // the same point. Hash the complete record so changes to radius,
            // color, intensity or source cannot inherit stale smoothing state.
            uint64_t hash = 14695981039346656037ull;
            for (unsigned char byte : record)
            {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
            return 0x4d414e4900000000ull ^ hash;
        }
    }

    LocalLightManager& LocalLightManager::Instance()
    {
        static LocalLightManager manager;
        return manager;
    }

    void LocalLightManager::Configure(const std::wstring& basePath)
    {
        m_basePath = basePath;
        m_iniPath = basePath + L"GraphicsEffects.ini";
        m_logPath = basePath + L"LocalLighting.log";
        ReloadTuning();
        LoadManifest();
        Log("LocalLightManager configured; native D3D9 world-light capture active");
    }

    void LocalLightManager::ReloadTuning()
    {
        m_enabled = renderer::locationtuning::ReadInt(L"DynamicLighting", L"Enabled", 1, m_iniPath.c_str()) != 0;
        m_intensityScale = std::clamp(int(renderer::locationtuning::ReadInt(L"DynamicLighting", L"IntensityPercent", 100, m_iniPath.c_str())), 0, 250) * 0.01f;
        m_rayScale = std::clamp(int(renderer::locationtuning::ReadInt(L"DynamicLighting", L"RayPercent", 75, m_iniPath.c_str())), 0, 200) * 0.01f;
        m_quality = std::clamp(int(renderer::locationtuning::ReadInt(L"DynamicLighting", L"Quality", 2, m_iniPath.c_str())), 0, 2);
        m_debugMode = std::clamp(int(renderer::locationtuning::ReadInt(L"DynamicLighting", L"DebugMode", 0, m_iniPath.c_str())), 0, 4);
    }

    void LocalLightManager::Reset()
    {
        m_nativeLights.clear();
        m_attached.clear();
        m_smoothed.clear();
        m_selected.clear();
        m_selectionSignature = 0;
    }

    void LocalLightManager::LoadManifest()
    {
        m_manifestLights.clear();
        const std::filesystem::path path = std::filesystem::path(m_basePath) / L"MaterialCache" / L"local_light_manifest.json";
        std::ifstream file(path);
        if (!file) { Log("Optional local light manifest not found; native D3D9 discovery remains active"); return; }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        size_t pos = 0;
        while ((pos = text.find("\"world_position\"", pos)) != std::string::npos)
        {
            const size_t begin = text.rfind('{', pos), end = text.find('}', pos);
            if (begin == std::string::npos || end == std::string::npos) break;
            const std::string block = text.substr(begin, end - begin + 1);
            auto array3 = [&](const char* key, Vec3 fallback) {
                const size_t k = block.find(std::string("\"") + key + "\"");
                if (k == std::string::npos) return fallback;
                const size_t b = block.find('[', k); if (b == std::string::npos) return fallback;
                std::stringstream stream(block.substr(b + 1)); Vec3 v = fallback; char comma = 0;
                if (!(stream >> v.x >> comma >> v.y >> comma >> v.z)) return fallback;
                return v;
            };
            auto number = [&](const char* key, float fallback) {
                const size_t k = block.find(std::string("\"") + key + "\"");
                if (k == std::string::npos) return fallback;
                const size_t c = block.find(':', k); if (c == std::string::npos) return fallback;
                char* tail = nullptr; const float value = std::strtof(block.c_str() + c + 1, &tail);
                return tail == block.c_str() + c + 1 ? fallback : value;
            };
            const Vec3 position = array3("world_position", {});
            const Vec3 direction = array3("direction", { 0,0,-1 });
            const Vec3 color = array3("color", { 1,.72f,.38f });
            const float radius = number("radius", 12.f);
            const float intensity = number("intensity", 1.f);
            const float confidence = number("confidence", 1.f);
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
                !std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) ||
                !std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z) ||
                !std::isfinite(radius) || !std::isfinite(intensity) || !std::isfinite(confidence) ||
                confidence < .75f)
            {
                pos = end + 1;
                continue;
            }
            LocalLightSource light;
            light.position = position;
            light.direction = Normalize(direction);
            light.color = color;
            light.radius = std::clamp(radius, 1.f, 100.f);
            light.intensity = std::clamp(intensity, .01f, 8.f);
            light.confidence = std::clamp(confidence, 0.f, 1.f);
            light.stableId = StableManifestId(block);
            light.flags = LocalLightPoint | LocalLightManifest | LocalLightVolumetric;
            m_manifestLights.push_back(light);
            pos = end + 1;
        }
        Log("Loaded authoritative world-space manifest lights: " + std::to_string(m_manifestLights.size()));
    }

    void LocalLightManager::OnSetLight(DWORD index, const D3DLIGHT9* light)
    {
        if (!light) return;
        NativeSlot& slot = m_nativeLights[index];
        bool stable = slot.valid;
        if (slot.valid)
        {
            const float dx = slot.light.Position.x - light->Position.x;
            const float dy = slot.light.Position.y - light->Position.y;
            const float dz = slot.light.Position.z - light->Position.z;
            // WoW frequently reuses a fixed-function light slot for short-lived
            // spell/particle draws.  A position jump starts a new observation;
            // only persistent world-space sources are admitted below.
            if (dx * dx + dy * dy + dz * dz > 0.25f)
            {
                slot.stableFrames = 0;
                ++slot.generation;
                stable = false;
            }
        }
        slot.light = *light;
        slot.valid = light->Type == D3DLIGHT_POINT || light->Type == D3DLIGHT_SPOT;
        slot.lastSeenTick = GetTickCount64();
        if (slot.valid && stable)
            slot.stableFrames = std::min<uint32_t>(slot.stableFrames + 1, 100000u);
    }

    void LocalLightManager::OnLightEnable(DWORD index, BOOL enabled)
    {
        NativeSlot& slot = m_nativeLights[index];
        slot.enabled = enabled != FALSE;
        if (slot.enabled)
        {
            slot.lastEnabledTick = GetTickCount64();
            slot.stableFrames = std::min<uint32_t>(slot.stableFrames + 1, 100000u);
        }
    }

    float LocalLightManager::EstimateRadius(const D3DLIGHT9& light)
    {
        if (std::isfinite(light.Range) && light.Range > 0.25f && light.Range < 500.0f)
            return light.Range;
        const float q = std::max(light.Attenuation2, 0.0001f);
        const float l = std::max(light.Attenuation1, 0.0f);
        const float c = std::max(light.Attenuation0, 0.01f) - 32.0f;
        const float disc = l * l - 4.0f * q * c;
        return std::clamp(disc > 0.0f ? (-l + std::sqrt(disc)) / (2.0f * q) : 12.0f, 2.0f, 80.0f);
    }

    void LocalLightManager::ObserveTorchDraw(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT start,UINT primitives,bool indexed,const void* verticesUP,UINT strideUP,const void* indicesUP,D3DFORMAT indexFormatUP,UINT vertexCountUP)
    {
        // Only verified flame material + verified rigid M2 skinning. Neither
        // arbitrary bright pixels nor character/weapon textures become lights.
        const auto& f=FrameContext::Current();
        // A torch is recovered from the draw's own view-space vertices, so its
        // world position is only as good as the pose converting it. With a
        // stale pose the emitter lands somewhere the player is not - and, since
        // this runs every frame, the light follows the error around. Wait for a
        // pose a draw actually carried the eye for.
        if(!m_enabled||!d||!f.cameraValid||!f.viewRawValid||!g_trackedState.zEnable||!g_trackedState.alphaBlend)return;
        // A one-frame-old pose places the torch a frame late, which is
        // invisible; a pose whose eye is a second old places it in the wrong
        // street. Only the latter is refused.
        if(f.cameraPoseStale&&!(f.cameraEyeAgeMs<=FrameContext::kCameraEyeHoldMs))return;
        bool rigid=std::find(std::begin(kRigidM2Shaders),std::end(kRigidM2Shaders),g_trackedState.vsHash)!=std::end(kRigidM2Shaders);
        bool unskinned=std::find(std::begin(kUnskinnedM2Shaders),std::end(kUnskinnedM2Shaders),g_trackedState.vsHash)!=std::end(kUnskinnedM2Shaders);
        if(!rigid&&!unskinned)return;
        using Microsoft::WRL::ComPtr;
        ComPtr<IDirect3DBaseTexture9> texture;if(FAILED(d->GetTexture(0,texture.GetAddressOf())))return;
        uint64_t hash=TextureHashLookup::Instance().GetTextureHash(texture.Get());
        if(std::find(std::begin(kTorchTextureHashes),std::end(kTorchTextureHashes),hash)==std::end(kTorchTextureHashes))return;
        UINT count=type==D3DPT_TRIANGLELIST?primitives*3:type==D3DPT_TRIANGLESTRIP?primitives+2:0;
        if(count<3||count>768)return;
        ComPtr<IDirect3DVertexDeclaration9> decl;D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1];UINT ne=std::size(elements);
        if(FAILED(d->GetVertexDeclaration(decl.GetAddressOf()))||!decl||FAILED(decl->GetDeclaration(elements,&ne)))return;
        int position=-1,bone=-1;
        for(UINT i=0;i<ne;++i){const auto& e=elements[i];if(e.Stream!=0)continue;
            if(e.Usage==D3DDECLUSAGE_POSITION&&e.Type==D3DDECLTYPE_FLOAT3)position=e.Offset;
            if(e.Usage==D3DDECLUSAGE_BLENDINDICES&&e.Type==D3DDECLTYPE_UBYTE4)bone=e.Offset;}
        if(position<0||(rigid&&bone<0))return;
        ComPtr<IDirect3DIndexBuffer9> ib;ComPtr<IDirect3DVertexBuffer9> vb;UINT offset=0,stride=strideUP;
        D3DVERTEXBUFFER_DESC vd{};
        if(!verticesUP){if(FAILED(d->GetStreamSource(0,vb.GetAddressOf(),&offset,&stride))||!vb||FAILED(vb->GetDesc(&vd)))return;}
        if(stride<UINT(position+12)||(rigid&&stride<UINT(bone+4)))return;
        const void* indices=indicesUP;D3DFORMAT indexFormat=indexFormatUP;void* lockedIndices=nullptr;
        if(indexed&&!indicesUP){D3DINDEXBUFFER_DESC id{};
            if(FAILED(d->GetIndices(ib.GetAddressOf()))||!ib||FAILED(ib->GetDesc(&id)))return;
            indexFormat=id.Format;UINT size=indexFormat==D3DFMT_INDEX16?2:4;
            if((uint64_t(start)+count)*size>id.Size)return;
            if(FAILED(ib->Lock(start*size,count*size,&lockedIndices,D3DLOCK_READONLY)))return;indices=lockedIndices;}
        // count is bounded to 768 above, so the index list and the emitter
        // clusters are stack-resident: no heap traffic on a matched draw.
        UINT selected[768];size_t selCount=0;UINT lo=~0u,hi=0;bool valid=true;
        for(UINT i=0;i<count;++i){int64_t v=int64_t(base)+(indexed?(indexFormat==D3DFMT_INDEX16?static_cast<const WORD*>(indices)[i]:static_cast<const DWORD*>(indices)[i]):start+i);
            if(v<0||(!verticesUP&&uint64_t(offset)+(uint64_t(v)+1)*stride>vd.Size)||(verticesUP&&vertexCountUP&&uint64_t(v)>=vertexCountUP)){valid=false;break;}
            selected[selCount++]=UINT(v);lo=std::min(lo,UINT(v));hi=std::max(hi,UINT(v));}
        if(lockedIndices)ib->Unlock();if(!valid||selCount==0||uint64_t(hi-lo+1)*stride>1024*1024)return;
        void* lockedVertices=nullptr;const BYTE* vertices=static_cast<const BYTE*>(verticesUP);
        if(!verticesUP){if(FAILED(vb->Lock(offset+lo*stride,(hi-lo+1)*stride,&lockedVertices,D3DLOCK_READONLY))){static bool logged=false;if(!logged){Log("Torch draw matched but vertex buffer is not readable");logged=true;}return;}
            vertices=static_cast<const BYTE*>(lockedVertices);}
        float matrices[225][4]{};HRESULT constants=d->GetVertexShaderConstantF(31,matrices[0],rigid?225:3);
        Vec3 centers[32];UINT centerCounts[32];size_t centerCount=0;
        if(SUCCEEDED(constants))for(size_t s=0;s<selCount;++s){const UINT index=selected[s];const BYTE* vertex=vertices+(index-(verticesUP?0:lo))*stride;
            UINT bi=rigid?vertex[bone]:0;if(bi>=75)continue;float v[3];memcpy(v,vertex+position,sizeof(v));
            Vec3 p;float* components=&p.x;for(UINT j=0;j<3;++j){const float* row=matrices[bi*3+j];components[j]=row[0]*v[0]+row[1]*v[1]+row[2]*v[2]+row[3];}
            const float positionSquared=Dot(p,p);if(!std::isfinite(positionSquared))continue;
            size_t cluster=0;while(cluster<centerCount){const Vec3 delta=centers[cluster]-p;if(Dot(delta,delta)<=2.25f)break;++cluster;}
            if(cluster==centerCount){if(centerCount>=32)continue;centers[centerCount]=p;centerCounts[centerCount++]=1;}
            else {UINT n=++centerCounts[cluster];centers[cluster]=centers[cluster]+(p-centers[cluster])*(1.f/n);}}
        if(lockedVertices)vb->Unlock();
        // Particle batches can contain several torches. Never average the
        // whole batch into a phantom emitter halfway between real flames.
        const float camera[3]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z};
        ULONGLONG now=GetTickCount64();
        for(size_t c=0;c<centerCount;++c){const Vec3& view=centers[c];
        const float viewPosition[3]={view.x,view.y,view.z};float worldPosition[3]{};
        ViewPositionToWorld(&f.viewRaw.m[0][0],camera,viewPosition,worldPosition);
        Vec3 world{worldPosition[0],worldPosition[1],worldPosition[2]};
        const Vec3 cameraDelta=world-f.cameraPosition;
        const float worldSquared=Dot(world,world),cameraDistanceSquared=Dot(cameraDelta,cameraDelta);
        if(!std::isfinite(worldSquared)||!std::isfinite(cameraDistanceSquared)||cameraDistanceSquared>14400.f)continue;
        auto found=std::find_if(m_attached.begin(),m_attached.end(),[&](const AttachedEmitter& e){const Vec3 delta=e.light.position-world;return now-e.seen<kAttachedLifetimeMs&&Dot(delta,delta)<2.25f;});
        if(found!=m_attached.end()){found->light.position=world;found->seen=now;continue;}
        if(m_attached.size()>=64)continue;
        LocalLightSource light;light.stableId=0x544f524300000000ull|m_nextAttachedId++;light.position=world;
        light.color={1.f,.52f,.16f};light.radius=10;light.intensity=1.8f;
        light.flags=LocalLightPoint|LocalLightAttached|LocalLightVolumetric;m_attached.push_back({light,now});
        Log("Captured attached flame emitter id="+std::to_string(light.stableId));
        }
    }

    void LocalLightManager::SelectForFrame(const Vec3& cameraPosition, const Vec3& cameraForward)
    {
        ++m_selectionFrame;
        m_selected.clear();
        m_selectionSignature = 0;
        // Slot reuse gives every generation a fresh stableId, so the
        // smoothing map would otherwise grow without bound over a long
        // session (and cost more per frame as it did). A bounded flush
        // costs at most one unsmoothed frame, which is invisible.
        if (m_smoothed.size() > 128) m_smoothed.clear();
        if (!m_enabled || m_intensityScale <= 0.0f) return;

        auto& candidates = m_candidates;
        if (candidates.capacity() < m_nativeLights.size() + m_manifestLights.size() + 16u)
            candidates.reserve(m_nativeLights.size() + m_manifestLights.size() + 16u);
        candidates.clear();
        const ULONGLONG now = GetTickCount64();
        for (auto& pair : m_nativeLights)
        {
            const DWORD index = pair.first;
            NativeSlot& slot = pair.second;
            // WoW enables many model/world lights only for the draw calls
            // which consume them and disables the slot again before our
            // end-of-world composite. Keep a short grace window and repeated-
            // observation filter for those draw-scoped enables, but honor a
            // light that D3D still reports as enabled: SetLight state persists,
            // so requiring another call every 350 ms made a valid one-shot
            // static light silently disappear.
            // D3D light enables are often scoped to one batch, but nearby batches can
            // be separated by CPU work, occlusion, or a skipped frame. A short
            // grace window here made the entire source blink at distance as its
            // producer crossed batch boundaries. Keep confirmed sources alive
            // long enough to bridge those gaps; the camera/radius tests below
            // still cull genuinely irrelevant lights.
            constexpr ULONGLONG kNativeEnableGraceMs = 700;
            constexpr ULONGLONG kNativeObservationGraceMs = 1400;
            const bool recentlyEnabled = slot.lastEnabledTick != 0 && now - slot.lastEnabledTick <= kNativeEnableGraceMs;
            const bool recentlySeen = slot.lastSeenTick != 0 && now - slot.lastSeenTick <= kNativeObservationGraceMs;
            if (!slot.valid || (!slot.enabled &&
                (!recentlyEnabled || !recentlySeen || slot.stableFrames < 6))) continue;
            const D3DLIGHT9& d = slot.light;
            if (!std::isfinite(d.Position.x) || !std::isfinite(d.Position.y) || !std::isfinite(d.Position.z) ||
                !std::isfinite(d.Diffuse.r) || !std::isfinite(d.Diffuse.g) || !std::isfinite(d.Diffuse.b) ||
                (d.Type == D3DLIGHT_SPOT &&
                    (!std::isfinite(d.Direction.x) || !std::isfinite(d.Direction.y) || !std::isfinite(d.Direction.z) ||
                     !std::isfinite(d.Theta) || !std::isfinite(d.Phi)))) continue;
            LocalLightSource light;
            light.stableId = 0x4e41544900000000ull ^ uint64_t(index) ^ (uint64_t(slot.generation) << 24);
            light.position = { d.Position.x, d.Position.y, d.Position.z };
            light.direction = Normalize({ d.Direction.x, d.Direction.y, d.Direction.z });
            light.color = {
                std::max(0.0f, d.Diffuse.r),
                std::max(0.0f, d.Diffuse.g),
                std::max(0.0f, d.Diffuse.b)
            };
            light.radius = EstimateRadius(d);
            light.intensity = std::clamp(std::max({ light.color.x, light.color.y, light.color.z }), 0.05f, 8.0f);
            if (light.intensity > 0.001f)
            {
                light.color = light.color * (1.0f / light.intensity);
            }
            light.innerCone = d.Type == D3DLIGHT_SPOT ? std::cos(d.Theta * 0.5f) : -1.0f;
            light.outerCone = d.Type == D3DLIGHT_SPOT ? std::cos(d.Phi * 0.5f) : -1.0f;
            light.flags = (d.Type == D3DLIGHT_SPOT ? LocalLightSpot : LocalLightPoint) |
                          LocalLightNative | LocalLightVolumetric;
            light.confidence = 1.0f;

            const Vec3 delta = light.position - cameraPosition;
            const float distanceSquared = Dot(delta, delta);
            const float maxDistance = light.radius * 2.0f + 80.0f;
            if (!std::isfinite(distanceSquared) || distanceSquared > maxDistance * maxDistance) continue;
            // NOTHING about the camera's HEADING may decide whether a lamp is drawn.
            // It used to: a light one radius behind was culled outright, and the score
            // carried a factor of six between "in front" and "behind", so a yaw moved
            // lamps in and out of the four/eight slots and re-ordered the rest - which
            // `StablePool`'s hysteresis turned into swaps and cross-fades rather than a
            // clean picture. The report is exactly that: "lights break more often on
            // left-right rotation than up-down". Distance and intensity are facts about
            // where the lamp is; the direction the player happens to face is not, and a
            // lamp behind the player is still lighting ground the player can turn onto.
            // It is also the wrong test for "off screen": the frustum culls what is not
            // drawn, and haloes reach past a screen edge from lights just outside it.
            // `cameraForward` stays in the signature because the volume march and the
            // water stage take the same pose, but this selection no longer reads it.
            (void)cameraForward;
            const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
            light.score = light.intensity * light.radius * light.radius / std::max(4.0f, distanceSquared);

            auto previous = m_smoothed.find(light.stableId);
            if (previous != m_smoothed.end())
            {
                constexpr float k = 0.24f;
                light.position = previous->second.position * (1.0f - k) + light.position * k;
                light.color = previous->second.color * (1.0f - k) + light.color * k;
                light.intensity = previous->second.intensity * (1.0f - k) + light.intensity * k;
            }
            m_smoothed[light.stableId] = light;
            candidates.push_back(light);
        }

        const ULONGLONG emitterNow=GetTickCount64();
        std::erase_if(m_attached,[&](const AttachedEmitter& e){return emitterNow-e.seen>kAttachedLifetimeMs;});
        for(auto& emitter:m_attached){auto light=emitter.light;const Vec3 delta=light.position-cameraPosition;
            const float distanceSquared=Dot(delta,delta);
            if(!std::isfinite(distanceSquared)||distanceSquared>10000.f)continue;
            const float age=float(emitterNow-emitter.seen);
            light.intensity*=age<=kAttachedHoldMs?1.f:std::clamp(1.f-(age-kAttachedHoldMs)/float(kAttachedLifetimeMs-kAttachedHoldMs),0.f,1.f);
            light.score=4.f*light.intensity*light.radius*light.radius/std::max(4.f,distanceSquared);candidates.push_back(light);}

        for (auto light : m_manifestLights)
        {
            const Vec3 delta = light.position - cameraPosition;
            const float distanceSquared = Dot(delta, delta);
            const float maxDistance = light.radius * 2.f + 80.f;
            // Camera basis conventions differ between the fixed-function and
            // shader paths in this client. Distance is authoritative; the
            // surface shader naturally rejects lights that cannot affect
            // visible pixels. Test the broad radius in squared space first so
            // the 6,000+ authored sources do not each need a square root.
            if (!std::isfinite(distanceSquared) || distanceSquared > maxDistance * maxDistance) continue;
            light.score = light.intensity * light.radius * light.radius / std::max(4.f, distanceSquared);
            candidates.push_back(light);
        }

        const size_t limit = m_quality == 0 ? 4u : 8u;
        const auto byPriority = [](const LocalLightSource& a, const LocalLightSource& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.stableId < b.stableId;
        };
        // Only the best four/eight feed either the surface shader or the
        // volume march, and the boundary between "in" and "out" used to be
        // decided on the raw score every frame. Two lamps of similar strength
        // then swapped places as the player walked between them - one going
        // dark and another lighting up in the same frame, which is the
        // reported flicker near a group of lights. Incumbents now hold their
        // slot until a challenger beats them by kDisplacementMargin, and at
        // most one slot changes hands per frame.
        const size_t count = local_light_detail::StablePool(candidates, m_selected, limit, byPriority);

        // Every slot cross-fades. A swap is a real change in what the pool can
        // show, and it must dissolve rather than pop: a light entering ramps up
        // over kFadeInMs, one that left keeps its slot while it ramps down over
        // kFadeOutMs. Bounded by the same 100 ms ceiling as every other frame
        // delta here, so an Alt+Tab does not manufacture a fade.
        const ULONGLONG selectionNow = GetTickCount64();
        const float elapsedMs = m_lastSelectionTick != 0
            ? float(std::min<ULONGLONG>(selectionNow - m_lastSelectionTick, 100ull))
            : 16.f;
        m_lastSelectionTick = selectionNow;

        // Persistent, like m_candidates: the pool is rebuilt every frame on
        // the render thread and must not allocate one.
        std::vector<LocalLightSource>& next = m_nextSelected;
        next.clear();
        next.reserve(limit);
        for (size_t index = 0; index < count; ++index)
        {
            LocalLightSource light = candidates[index];
            const auto weight = m_fade.find(light.stableId);
            const float fade = local_light_detail::FadeToward(
                weight != m_fade.end() ? weight->second : 0.f, true, elapsedMs);
            m_fade[light.stableId] = fade;
            light.intensity *= fade;
            next.push_back(light);
        }
        // A source that fell out of the candidate set (out of range, its draw
        // batch ended) is exactly the one a walking player sees blink. Hold it
        // for the length of its fade, with its last published position.
        for (auto it = m_fade.begin(); it != m_fade.end();)
        {
            bool stillCandidate = false;
            for (size_t index = 0; index < count; ++index)
                if (candidates[index].stableId == it->first) { stillCandidate = true; break; }
            if (stillCandidate) { ++it; continue; }
            const float previousFade = it->second;
            const float fade = local_light_detail::FadeToward(previousFade, false, elapsedMs);
            // Fully dissolved: forget the weight rather than accumulate a zero
            // entry per light the player has ever walked past. A forgotten
            // light that comes back later starts its fade-in from zero, which
            // is the same curve it would have had.
            if (fade <= 0.001f) { it = m_fade.erase(it); continue; }
            it->second = fade;
            if (next.size() >= limit) { ++it; continue; }
            for (const auto& previous : m_selected)
            {
                if (previous.stableId != it->first) continue;
                LocalLightSource light = previous;
                // The published copy already carries the fade it had last
                // frame, so scale it by how much the fade moved, not again by
                // its whole value.
                if (previousFade > 1e-3f)
                    light.intensity *= std::min(1.f, fade / previousFade);
                next.push_back(light);
                break;
            }
            ++it;
        }
        m_selected.swap(next);

        // Water's per-frame constant cache needs to notice a changed emitter
        // set - and a fade in progress - even if another water surface draws
        // in the same frame.
        m_selectionSignature = local_light_detail::SelectionSignature(m_selected);

        if ((m_selectionFrame % 300) == 0)
        {
            float nearestSquared = std::numeric_limits<float>::infinity();
            for (const auto& light : m_manifestLights)
            {
                const Vec3 delta = light.position - cameraPosition;
                nearestSquared = std::min(nearestSquared, Dot(delta, delta));
            }
            const float nearest = std::sqrt(nearestSquared);
            size_t nativeSelected = 0;
            size_t manifestSelected = 0;
            size_t attachedSelected = 0;
            for (const auto& light : m_selected)
            {
                if (light.flags & LocalLightNative) ++nativeSelected;
                if (light.flags & LocalLightManifest) ++manifestSelected;
                if (light.flags & LocalLightAttached) ++attachedSelected;
            }
            Log("Source diagnostic: native_slots=" + std::to_string(m_nativeLights.size()) +
                " selected=" + std::to_string(m_selected.size()) +
                " native_selected=" + std::to_string(nativeSelected) +
                " manifest_selected=" + std::to_string(manifestSelected) +
                " attached_selected=" + std::to_string(attachedSelected) +
                " attached=" + std::to_string(m_attached.size()) +
                " quality=" + std::to_string(m_quality) +
                " camera=(" + std::to_string(cameraPosition.x) + "," +
                std::to_string(cameraPosition.y) + "," + std::to_string(cameraPosition.z) +
                ") nearest_manifest=" + std::to_string(nearest));

            std::string selectedDetails = "Selected source detail:";
            for (size_t index = 0; index < m_selected.size(); ++index)
            {
                const auto& light = m_selected[index];
                const char* kind = (light.flags & LocalLightAttached) ? "attached" :
                    (light.flags & LocalLightManifest) ? "manifest" :
                    (light.flags & LocalLightNative) ? "native" : "other";
                selectedDetails += " [" + std::to_string(index) + "] kind=" + kind +
                    " id=" + std::to_string(light.stableId) +
                    " flags=" + std::to_string(light.flags) +
                    " position=(" + std::to_string(light.position.x) + "," +
                    std::to_string(light.position.y) + "," + std::to_string(light.position.z) + ")" +
                    " radius=" + std::to_string(light.radius) +
                    " intensity=" + std::to_string(light.intensity) +
                    " score=" + std::to_string(light.score);
            }
            Log(selectedDetails);
        }
    }

    void LocalLightManager::Log(const std::string& message) const
    {
        if (m_logPath.empty()) return;
        std::ofstream out(std::filesystem::path(m_logPath), std::ios::app);
        out << message << '\n';
    }
}
