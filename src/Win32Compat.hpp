#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace vrec::compat {

inline UINT GetDpiForSystemCompat() noexcept {
    using PfnGetDpiForSystem = UINT(WINAPI*)();
    static const auto pfn = reinterpret_cast<PfnGetDpiForSystem>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (pfn) {
        const UINT d = pfn();
        if (d > 0) return d;
    }
    if (HDC hdc = GetDC(nullptr)) {
        const int px = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        if (px > 0) return static_cast<UINT>(px);
    }
    return 96;
}

inline UINT GetDpiForWindowCompat(HWND hwnd) noexcept {
    if (hwnd) {
        using PfnGetDpiForWindow = UINT(WINAPI*)(HWND);
        static const auto pfn = reinterpret_cast<PfnGetDpiForWindow>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (pfn) {
            const UINT d = pfn(hwnd);
            if (d > 0) return d;
        }
    }
    return GetDpiForSystemCompat();
}

inline int GetSystemMetricsForDpiCompat(int index, UINT dpi) noexcept {
    using PfnGetSystemMetricsForDpi = int(WINAPI*)(int, UINT);
    static const auto pfn = reinterpret_cast<PfnGetSystemMetricsForDpi>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    if (pfn) {
        const int v = pfn(index, dpi);
        if (v > 0) return v;
    }
    const UINT sysDpi = GetDpiForSystemCompat();
    const int base = GetSystemMetrics(index);
    return (sysDpi > 0) ? MulDiv(base, static_cast<int>(dpi), static_cast<int>(sysDpi)) : base;
}

inline int ScaleDip(float dip, UINT dpi) noexcept {
    return static_cast<int>(std::lround(dip * (static_cast<float>(dpi) / 96.0f)));
}

inline bool DecodeWicDecoderToPbgra(IWICImagingFactory* factory,
                                        IWICBitmapDecoder* decoder,
                                        uint32_t& outW,
                                        uint32_t& outH,
                                        std::vector<uint8_t>& outPixels) {
    if (!factory || !decoder) return false;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return false;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const UINT stride = w * 4;
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * h);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(buf.size()), buf.data()))) {
        return false;
    }

    outW = w;
    outH = h;
    outPixels = std::move(buf);
    return true;
}

inline bool DecodeImageFileToPbgra(const std::wstring& path,
                                   uint32_t& outW,
                                   uint32_t& outH,
                                   std::vector<uint8_t>& outPixels) {
    outW = 0;
    outH = 0;
    outPixels.clear();
    if (path.empty()) return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        return false;
    }
    return DecodeWicDecoderToPbgra(factory.Get(), decoder.Get(), outW, outH, outPixels);
}

inline bool DecodeImageResourceToPbgra(HINSTANCE hInst,
                                       UINT resId,
                                       uint32_t& outW,
                                       uint32_t& outH,
                                       std::vector<uint8_t>& outPixels) {
    outW = 0;
    outH = 0;
    outPixels.clear();
    if (!hInst) return false;

    HRSRC hrsrc = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hrsrc) return false;
    const DWORD sz = SizeofResource(hInst, hrsrc);
    HGLOBAL hglb = LoadResource(hInst, hrsrc);
    if (!hglb || sz == 0) return false;
    void* ptr = LockResource(hglb);
    if (!ptr) return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) || !stream) return false;
    if (FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(ptr), sz))) return false;

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        return false;
    }
    return DecodeWicDecoderToPbgra(factory.Get(), decoder.Get(), outW, outH, outPixels);
}

}
