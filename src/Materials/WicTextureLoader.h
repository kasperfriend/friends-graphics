#pragma once
#include <d3d9.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace renderer
{
    enum class WicTextureUsage
    {
        NormalMap,
        LinearData
    };

    inline HRESULT LoadTextureFromFileWic(
        IDirect3DDevice9* device,
        const std::wstring& path,
        IDirect3DTexture9** outTexture,
        WicTextureUsage usage = WicTextureUsage::NormalMap)
    {
        if (!device || !outTexture)
            return E_POINTER;
        *outTexture = nullptr;

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr))
            return hr;

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
            decoder.GetAddressOf());
        if (FAILED(hr))
            return hr;

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.GetAddressOf());
        if (FAILED(hr))
            return hr;

        UINT width = 0, height = 0;
        hr = frame->GetSize(&width, &height);
        if (FAILED(hr) || width == 0 || height == 0)
            return E_FAIL;

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(hr))
            return hr;

        hr = converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
            return hr;

        // Create texture with complete mipmap chain (Levels = 0)
        Microsoft::WRL::ComPtr<IDirect3DTexture9> tex;
        hr = device->CreateTexture(
            width, height, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            tex.GetAddressOf(), nullptr);
        if (FAILED(hr))
            return hr;

        // Fill Mip Level 0
        D3DLOCKED_RECT lr0{};
        hr = tex->LockRect(0, &lr0, nullptr, 0);
        if (FAILED(hr))
            return hr;

        const UINT stride0 = width * 4;
        const UINT bufferSize0 = stride0 * height;
        if (static_cast<UINT>(lr0.Pitch) == stride0)
        {
            hr = converter->CopyPixels(nullptr, 0, stride0, static_cast<BYTE*>(lr0.pBits));
        }
        else
        {
            std::vector<BYTE> temp(bufferSize0);
            hr = converter->CopyPixels(nullptr, 0, stride0, temp.data());
            if (SUCCEEDED(hr))
            {
                for (UINT y = 0; y < height; ++y)
                {
                    memcpy(static_cast<BYTE*>(lr0.pBits) + y * lr0.Pitch, temp.data() + y * stride0, stride0);
                }
            }
        }
        tex->UnlockRect(0);

        if (FAILED(hr))
            return hr;

        // Generate downsampled mipmaps with progressive Toksvig/flat normal convergence
        // Eliminates distance aliasing and black specular/shadow noise completely
        const DWORD levelCount = tex->GetLevelCount();
        for (DWORD lvl = 1; lvl < levelCount; ++lvl)
        {
            D3DSURFACE_DESC descPrev{}, descCur{};
            if (FAILED(tex->GetLevelDesc(lvl - 1, &descPrev)) ||
                FAILED(tex->GetLevelDesc(lvl, &descCur)))
                break;

            D3DLOCKED_RECT lrPrev{}, lrCur{};
            if (FAILED(tex->LockRect(lvl - 1, &lrPrev, nullptr, D3DLOCK_READONLY)) ||
                FAILED(tex->LockRect(lvl, &lrCur, nullptr, 0)))
            {
                if (lrPrev.pBits) tex->UnlockRect(lvl - 1);
                break;
            }

            const UINT pW = descPrev.Width;
            const UINT pH = descPrev.Height;
            const UINT cW = descCur.Width;
            const UINT cH = descCur.Height;

            // Normal maps converge to a flat normal in the distance. Linear data
            // (notably height maps) must retain its scalar values in every mip.
            const float flatWeight = usage == WicTextureUsage::NormalMap
                ? std::clamp(static_cast<float>(lvl) / 3.0f, 0.0f, 1.0f)
                : 0.0f;

            for (UINT cy = 0; cy < cH; ++cy)
            {
                const UINT py0 = cy * 2;
                const UINT py1 = std::min(py0 + 1, pH - 1);

                const BYTE* row0 = static_cast<const BYTE*>(lrPrev.pBits) + py0 * lrPrev.Pitch;
                const BYTE* row1 = static_cast<const BYTE*>(lrPrev.pBits) + py1 * lrPrev.Pitch;
                BYTE* dstRow = static_cast<BYTE*>(lrCur.pBits) + cy * lrCur.Pitch;

                for (UINT cx = 0; cx < cW; ++cx)
                {
                    const UINT px0 = cx * 2;
                    const UINT px1 = std::min(px0 + 1, pW - 1);

                    const BYTE* p00 = row0 + px0 * 4;
                    const BYTE* p01 = row0 + px1 * 4;
                    const BYTE* p10 = row1 + px0 * 4;
                    const BYTE* p11 = row1 + px1 * 4;

                    BYTE* dst = dstRow + cx * 4;
                    if (usage == WicTextureUsage::NormalMap)
                    {
                        // Unpack 4 parent normal vectors [-1, 1]. PNG alpha is
                        // intentionally ignored: generated height is a separate texture.
                        float nx = 0.25f * (((p00[2] + p01[2] + p10[2] + p11[2]) / 255.0f) * 2.0f - 4.0f);
                        float ny = 0.25f * (((p00[1] + p01[1] + p10[1] + p11[1]) / 255.0f) * 2.0f - 4.0f);
                        float nz = 0.25f * (((p00[0] + p01[0] + p10[0] + p11[0]) / 255.0f) * 2.0f - 4.0f);

                        nx = (1.0f - flatWeight) * nx;
                        ny = (1.0f - flatWeight) * ny;
                        nz = (1.0f - flatWeight) * nz + flatWeight;

                        float lenInv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz + 1e-6f);
                        nx *= lenInv;
                        ny *= lenInv;
                        nz *= lenInv;

                        dst[0] = static_cast<BYTE>(std::clamp((nz * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                        dst[1] = static_cast<BYTE>(std::clamp((ny * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                        dst[2] = static_cast<BYTE>(std::clamp((nx * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                        dst[3] = 255;
                    }
                    else
                    {
                        // Packed linear material map: R=height, G=roughness.
                        // Average channels independently; replicating red here used
                        // to silently destroy roughness in every generated mip.
                        for (UINT channel = 0; channel < 4; ++channel)
                        {
                            dst[channel] = static_cast<BYTE>((
                                static_cast<UINT>(p00[channel]) + p01[channel] +
                                p10[channel] + p11[channel] + 2u) / 4u);
                        }
                    }
                }
            }

            tex->UnlockRect(lvl - 1);
            tex->UnlockRect(lvl);
        }

        *outTexture = tex.Detach();
        return S_OK;
    }
}
