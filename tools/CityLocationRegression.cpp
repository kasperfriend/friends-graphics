#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include "../src/Game/WoWClientContext.h"
using Microsoft::WRL::ComPtr;
volatile uint32_t mapId=571,zoneId=65,areaId=4164;
#define CHECK(x) do { if(FAILED(x)){printf("FAIL line %d\n",__LINE__);return 1;} } while(0)
int main() {
    renderer::WoWClientContext client;
    if(!client.Detect())return 2;
    // Detect never hashes (fast load); the provider does in Configure, so the
    // config this test writes must reference an explicitly computed hash.
    client.EnsureSha256(L"");
    std::ofstream config("EnvironmentProfiles.ini");
    config<<"[EnvironmentSystem]\nEnabled=1\nDebug=1\nAreaDebounceMs=300\n[LocationProvider]\nExeSHA256="<<client.sha256
        <<"\nMapRva="<<(uintptr_t(&mapId)-client.base)<<"\nZoneRva="<<(uintptr_t(&zoneId)-client.base)
        <<"\nAreaRva="<<(uintptr_t(&areaId)-client.base)<<"\nVerifiedInWorld=1\n";config.close();
    std::ofstream settings("ModernWoWRenderer.ini");settings<<"[Graphics]\nUnifiedToggle=1\nEnabled=1\n[Volume]\nEnabled=1\n";settings.close();
    HMODULE dll=LoadLibraryW(L".\\d3d9.dll");if(!dll)return 3;
    auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(dll,"Direct3DCreate9"));
    ComPtr<IDirect3D9> api;api.Attach(create(D3D_SDK_VERSION));
    HWND window=CreateWindowW(L"STATIC",L"City location regression",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;pp.BackBufferWidth=128;pp.BackBufferHeight=128;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D24X8;
    ComPtr<IDirect3DDevice9> d;CHECK(api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,d.GetAddressOf()));
    // Deliberately supply no camera constants or vertex shader. Exercise the
    // actual DLL's Present -> location provider -> editable preset path.
    for(int frame=0;frame<8;++frame){CHECK(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff404040,1,0));CHECK(d->BeginScene());CHECK(d->EndScene());CHECK(d->Present(nullptr,nullptr,nullptr,nullptr));Sleep(130);}
    std::ifstream input("ModernWoWRenderer.log");std::string log((std::istreambuf_iterator<char>(input)),{});
    if(log.find("Location changed: Map=571 Zone=65 Area=4164")==std::string::npos){puts("FAIL location rejected without camera");return 4;}
    std::ifstream effects("VolumeEffects.log");std::string volume((std::istreambuf_iterator<char>(effects)),{});
    if(volume.find("camera captured")!=std::string::npos){puts("FAIL fixture unexpectedly captured camera");return 5;}
    d.Reset();DestroyWindow(window);
    puts("PASS actual proxy resolves location without any camera constants");
}
