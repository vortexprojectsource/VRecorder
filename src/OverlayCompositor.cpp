#include "OverlayCompositor.hpp"
#include "Win32Compat.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace vrec {

namespace {

D2D1_COLOR_F ArgbToD2d(uint32_t argb) noexcept {
    return D2D1_COLOR_F{
        static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(argb & 0xFF) / 255.0f,
        static_cast<float>((argb >> 24) & 0xFF) / 255.0f
    };
}

}

OverlayCompositor::OverlayCompositor() = default;
OverlayCompositor::~OverlayCompositor() = default;

void OverlayCompositor::SetOverlays(const std::vector<OverlayItem>& items) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_items = items;
}

std::vector<OverlayItem> OverlayCompositor::GetOverlays() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_items;
}

bool OverlayCompositor::HasVisibleOverlays() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& item : m_items) {
        if (item.visible) return true;
    }
    return false;
}

std::wstring OverlayCompositor::FormatCurrentClockText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[48]{};
    std::swprintf(buf, 48, L"%04u-%02u-%02u  %02u:%02u:%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool OverlayCompositor::RasterizeSprite(const OverlayItem& item, const std::wstring& resolvedText,
                                        uint32_t outW, uint32_t outH, CachedSprite& sprite) {
    if (item.kind == OverlayKind::Image) {
        if (item.imagePath.empty()) return false;
        auto it = m_imageCache.find(item.imagePath);
        if (it == m_imageCache.end()) {
            DecodedImage di{};
            if (!compat::DecodeImageFileToPbgra(item.imagePath, di.w, di.h, di.pbgra) || di.w == 0 || di.h == 0) {
                return false;
            }
            it = m_imageCache.emplace(item.imagePath, std::move(di)).first;
        }
        const DecodedImage& src = it->second;
        const uint32_t targetW = std::clamp(static_cast<uint32_t>(std::lround(item.boxWidthNorm * outW)), 16u, outW);
        const float aspect = static_cast<float>(src.h) / static_cast<float>(src.w);
        const uint32_t targetH = std::clamp(static_cast<uint32_t>(std::lround(targetW * aspect)), 16u, outH);

        sprite.w = targetW;
        sprite.h = targetH;
        sprite.pbgra.resize(static_cast<size_t>(targetW) * targetH * 4);
        for (uint32_t y = 0; y < targetH; ++y) {
            const uint32_t sy = std::min(src.h - 1, static_cast<uint32_t>((static_cast<uint64_t>(y) * src.h) / targetH));
            for (uint32_t x = 0; x < targetW; ++x) {
                const uint32_t sx = std::min(src.w - 1, static_cast<uint32_t>((static_cast<uint64_t>(x) * src.w) / targetW));
                const size_t sOff = (static_cast<size_t>(sy) * src.w + sx) * 4;
                const size_t dOff = (static_cast<size_t>(y) * targetW + x) * 4;
                sprite.pbgra[dOff + 0] = src.pbgra[sOff + 0];
                sprite.pbgra[dOff + 1] = src.pbgra[sOff + 1];
                sprite.pbgra[dOff + 2] = src.pbgra[sOff + 2];
                sprite.pbgra[dOff + 3] = src.pbgra[sOff + 3];
            }
        }
        return true;
    }

    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw.GetAddressOf())))) {
        return false;
    }

    const float resScale = static_cast<float>(outH) / 1080.0f;
    const float pxFont = std::clamp(static_cast<float>(item.fontSize) * resScale, 10.0f, 160.0f);

    uint32_t spW = 64, spH = 32;
    ComPtr<IDWriteTextFormat> fmt;
    ComPtr<IDWriteTextLayout> layout;

    if (item.kind == OverlayKind::Box) {
        spW = std::clamp(static_cast<uint32_t>(std::lround(item.boxWidthNorm * outW)), 24u, outW);
        spH = std::clamp(static_cast<uint32_t>(std::lround(item.boxHeightNorm * outH)), 24u, outH);
    } else {
        const std::wstring& txt = resolvedText.empty() ? L" " : resolvedText;
        if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        pxFont, L"en-US", fmt.GetAddressOf()))) {
            return false;
        }
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (FAILED(dw->CreateTextLayout(txt.c_str(), static_cast<UINT32>(txt.size()),
                                        fmt.Get(), static_cast<float>(outW), static_cast<float>(outH),
                                        layout.GetAddressOf()))) {
            return false;
        }
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        const float padX = std::max(10.0f, pxFont * 0.45f);
        const float padY = std::max(5.0f, pxFont * 0.22f);
        spW = std::clamp(static_cast<uint32_t>(std::ceil(tm.widthIncludingTrailingWhitespace + padX * 2.0f)), 16u, outW);
        spH = std::clamp(static_cast<uint32_t>(std::ceil(tm.height + padY * 2.0f)), 16u, outH);
    }

    ComPtr<IWICBitmap> wicBmp;
    if (FAILED(wic->CreateBitmap(spW, spH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, wicBmp.GetAddressOf()))) {
        return false;
    }

    const D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(wicBmp.Get(), &rtp, rt.GetAddressOf()))) {
        return false;
    }
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    ComPtr<ID2D1SolidColorBrush> br;
    if (FAILED(rt->CreateSolidColorBrush(ArgbToD2d(item.colorArgb), br.GetAddressOf()))) return false;

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0.0f));

    if (item.kind == OverlayKind::Box) {
        const float corner = std::max(4.0f, std::min(spW, spH) * 0.08f);
        const float bw = std::max(2.0f, 3.0f * resScale);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(bw * 0.5f, bw * 0.5f, spW - bw * 0.5f, spH - bw * 0.5f), corner, corner);
        if (item.bgEnabled) {
            br->SetColor(ArgbToD2d(item.bgArgb));
            rt->FillRoundedRectangle(rr, br.Get());
        }
        br->SetColor(ArgbToD2d(item.colorArgb));
        rt->DrawRoundedRectangle(rr, br.Get(), bw);
    } else {
        const float padX = std::max(10.0f, pxFont * 0.45f);
        const float padY = std::max(5.0f, pxFont * 0.22f);
        if (item.bgEnabled) {
            br->SetColor(ArgbToD2d(item.bgArgb));
            const float r = std::max(4.0f, pxFont * 0.28f);
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, static_cast<float>(spW), static_cast<float>(spH)), r, r), br.Get());
        }
        br->SetColor(ArgbToD2d(item.colorArgb));
        rt->DrawTextLayout(D2D1::Point2F(padX, padY), layout.Get(), br.Get());
    }
    rt->EndDraw();

    sprite.w = spW;
    sprite.h = spH;
    sprite.pbgra.resize(static_cast<size_t>(spW) * spH * 4);
    const HRESULT cpy = wicBmp->CopyPixels(nullptr, spW * 4, static_cast<UINT>(sprite.pbgra.size()), sprite.pbgra.data());
    return SUCCEEDED(cpy);
}

void OverlayCompositor::CompositeOntoFrame(std::vector<uint8_t>& frameBgra, uint32_t outW, uint32_t outH) {
    if (frameBgra.size() < static_cast<size_t>(outW) * outH * 4 || outW == 0 || outH == 0) return;

    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_items.empty()) return;

    std::wstring clockStr;
    for (const auto& item : m_items) {
        if (!item.visible) continue;

        std::wstring resolved = item.text;
        if (item.kind == OverlayKind::Clock) {
            if (clockStr.empty()) clockStr = FormatCurrentClockText();
            resolved = clockStr;
        }

        wchar_t sigBuf[256]{};
        std::swprintf(sigBuf, 256, L"%d|%u|%u|%d|%08X|%d|%08X|%.3f|%.3f|",
                      static_cast<int>(item.kind), outW, outH, item.fontSize,
                      item.colorArgb, item.bgEnabled ? 1 : 0, item.bgArgb,
                      item.boxWidthNorm, item.boxHeightNorm);
        std::wstring sig = std::wstring(sigBuf) + resolved + L"|" + item.imagePath;

        auto& cached = m_spriteCache[item.id];
        if (cached.signature != sig || cached.pbgra.empty()) {
            if (RasterizeSprite(item, resolved, outW, outH, cached)) {
                cached.signature = sig;
            } else {
                continue;
            }
        }

        if (cached.w == 0 || cached.h == 0 || cached.pbgra.empty()) continue;

        const int dstX = std::clamp(static_cast<int>(std::lround(item.xNorm * outW)),
                                    0, std::max(0, static_cast<int>(outW) - static_cast<int>(cached.w)));
        const int dstY = std::clamp(static_cast<int>(std::lround(item.yNorm * outH)),
                                    0, std::max(0, static_cast<int>(outH) - static_cast<int>(cached.h)));

        const uint32_t copyW = std::min(cached.w, outW - static_cast<uint32_t>(dstX));
        const uint32_t copyH = std::min(cached.h, outH - static_cast<uint32_t>(dstY));

        for (uint32_t y = 0; y < copyH; ++y) {
            uint8_t* dstRow = frameBgra.data() + (static_cast<size_t>(dstY + y) * outW + dstX) * 4;
            const uint8_t* srcRow = cached.pbgra.data() + static_cast<size_t>(y) * cached.w * 4;
            for (uint32_t x = 0; x < copyW; ++x) {
                const uint32_t sa = srcRow[x * 4 + 3];
                if (sa == 0) continue;
                if (sa == 255) {
                    dstRow[x * 4 + 0] = srcRow[x * 4 + 0];
                    dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                    dstRow[x * 4 + 2] = srcRow[x * 4 + 2];
                    dstRow[x * 4 + 3] = 255;
                } else {
                    const uint32_t invA = 255u - sa;
                    dstRow[x * 4 + 0] = static_cast<uint8_t>(std::min(255u, srcRow[x * 4 + 0] + ((dstRow[x * 4 + 0] * invA + 127u) / 255u)));
                    dstRow[x * 4 + 1] = static_cast<uint8_t>(std::min(255u, srcRow[x * 4 + 1] + ((dstRow[x * 4 + 1] * invA + 127u) / 255u)));
                    dstRow[x * 4 + 2] = static_cast<uint8_t>(std::min(255u, srcRow[x * 4 + 2] + ((dstRow[x * 4 + 2] * invA + 127u) / 255u)));
                    dstRow[x * 4 + 3] = 255;
                }
            }
        }
    }
}

void OverlayCompositor::CompositeCameraOntoFrame(std::vector<uint8_t>& frameBgra, uint32_t outW, uint32_t outH,
                                                 const FrameCopy& camFrame, const AppConfig& cfg) {
    if (!cfg.camEnabled || camFrame.width == 0 || camFrame.height == 0 || camFrame.bgra.empty()) return;
    if (frameBgra.size() < static_cast<size_t>(outW) * outH * 4 || outW == 0 || outH == 0) return;

    const float aspect = (cfg.camShape == 2)
        ? 1.0f
        : (static_cast<float>(camFrame.height) / static_cast<float>(camFrame.width));
    const int dstW = std::clamp(static_cast<int>(std::lround(cfg.camWidthNorm * outW)), 32, static_cast<int>(outW));
    const int dstH = std::clamp(static_cast<int>(std::lround(dstW * aspect)), 32, static_cast<int>(outH));
    const int dstX = std::clamp(static_cast<int>(std::lround(cfg.camXNorm * outW)), 0, std::max(0, static_cast<int>(outW) - dstW));
    const int dstY = std::clamp(static_cast<int>(std::lround(cfg.camYNorm * outH)), 0, std::max(0, static_cast<int>(outH) - dstH));

    uint32_t srcCropX = 0, srcCropY = 0, srcCropW = camFrame.width, srcCropH = camFrame.height;
    if (cfg.camShape == 2 && camFrame.width != camFrame.height) {
        const uint32_t side = std::min(camFrame.width, camFrame.height);
        srcCropX = (camFrame.width - side) / 2;
        srcCropY = (camFrame.height - side) / 2;
        srcCropW = side;
        srcCropH = side;
    }

    const float halfW = static_cast<float>(dstW) * 0.5f;
    const float halfH = static_cast<float>(dstH) * 0.5f;
    const float rad = (cfg.camShape == 0) ? 0.0f
                    : (cfg.camShape == 1 ? std::min(halfW, halfH) * 0.16f : std::min(halfW, halfH));
    const float borderPx = cfg.camBorder ? std::max(2.0f, static_cast<float>(outH) * 0.0028f) : 0.0f;

    const uint8_t bColB = 68, bColG = 68, bColR = 239;

    for (int y = 0; y < dstH; ++y) {
        const uint32_t sy = srcCropY + std::min(srcCropH - 1, static_cast<uint32_t>((static_cast<uint64_t>(y) * srcCropH) / dstH));
        const uint8_t* srcRow = camFrame.bgra.data() + static_cast<size_t>(sy) * camFrame.width * 4;
        uint8_t* dstRow = frameBgra.data() + (static_cast<size_t>(dstY + y) * outW + dstX) * 4;
        const float dy = std::fabs((static_cast<float>(y) + 0.5f) - halfH) - (halfH - rad);

        for (int x = 0; x < dstW; ++x) {
            float dist = -10.0f;
            if (rad > 0.0f) {
                const float dx = std::fabs((static_cast<float>(x) + 0.5f) - halfW) - (halfW - rad);
                const float ox = std::max(dx, 0.0f);
                const float oy = std::max(dy, 0.0f);
                dist = std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - rad;
            } else if (borderPx > 0.0f) {
                const float edgeX = std::min(static_cast<float>(x) + 0.5f, static_cast<float>(dstW - x) - 0.5f);
                const float edgeY = std::min(static_cast<float>(y) + 0.5f, static_cast<float>(dstH - y) - 0.5f);
                dist = -std::min(edgeX, edgeY);
            }

            if (dist > 0.7f) continue;

            const uint32_t sx = srcCropX + std::min(srcCropW - 1, static_cast<uint32_t>((static_cast<uint64_t>(x) * srcCropW) / dstW));
            uint8_t sr = srcRow[sx * 4 + 2];
            uint8_t sg = srcRow[sx * 4 + 1];
            uint8_t sb = srcRow[sx * 4 + 0];

            if (borderPx > 0.0f && dist >= -borderPx) {
                sr = bColR; sg = bColG; sb = bColB;
            }

            if (dist > -0.7f) {
                const float cov = std::clamp(0.7f - dist, 0.0f, 1.0f);
                const float inv = 1.0f - cov;
                dstRow[x * 4 + 0] = static_cast<uint8_t>(sb * cov + dstRow[x * 4 + 0] * inv);
                dstRow[x * 4 + 1] = static_cast<uint8_t>(sg * cov + dstRow[x * 4 + 1] * inv);
                dstRow[x * 4 + 2] = static_cast<uint8_t>(sr * cov + dstRow[x * 4 + 2] * inv);
            } else {
                dstRow[x * 4 + 0] = sb;
                dstRow[x * 4 + 1] = sg;
                dstRow[x * 4 + 2] = sr;
            }
            dstRow[x * 4 + 3] = 255;
        }
    }
}

namespace {

inline void BlendPixelOntoNv12(uint8_t* yPlane, uint8_t* uvPlane, uint32_t outW, uint32_t outH,
                               uint32_t px, uint32_t py, uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa) noexcept {
    if (sa == 0 || px >= outW || py >= outH) return;
    uint8_t& yRef = yPlane[static_cast<size_t>(py) * outW + px];
    const size_t uvOff = static_cast<size_t>(py >> 1) * outW + (px & ~1u);
    uint8_t& uRef = uvPlane[uvOff];
    uint8_t& vRef = uvPlane[uvOff + 1];

    uint8_t fr = sr, fg = sg, fb = sb;
    if (sa < 255) {
        const int cy = static_cast<int>(yRef) - 16;
        const int cu = static_cast<int>(uRef) - 128;
        const int cv = static_cast<int>(vRef) - 128;
        const int dr = std::clamp((298 * cy + 459 * cv + 128) >> 8, 0, 255);
        const int dg = std::clamp((298 * cy - 55 * cu - 136 * cv + 128) >> 8, 0, 255);
        const int db = std::clamp((298 * cy + 541 * cu + 128) >> 8, 0, 255);
        const uint32_t invA = 255u - sa;
        fr = static_cast<uint8_t>(std::min(255u, static_cast<uint32_t>(sr) + ((static_cast<uint32_t>(dr) * invA + 127u) / 255u)));
        fg = static_cast<uint8_t>(std::min(255u, static_cast<uint32_t>(sg) + ((static_cast<uint32_t>(dg) * invA + 127u) / 255u)));
        fb = static_cast<uint8_t>(std::min(255u, static_cast<uint32_t>(sb) + ((static_cast<uint32_t>(db) * invA + 127u) / 255u)));
    }

    yRef = static_cast<uint8_t>(std::clamp(((47 * fr + 157 * fg + 16 * fb + 128) >> 8) + 16, 16, 235));
    if ((px & 1u) == 0 && (py & 1u) == 0) {
        uRef = static_cast<uint8_t>(std::clamp(((-26 * fr - 86 * fg + 112 * fb + 128) >> 8) + 128, 16, 240));
        vRef = static_cast<uint8_t>(std::clamp(((112 * fr - 102 * fg - 10 * fb + 128) >> 8) + 128, 16, 240));
    }
}

}

void OverlayCompositor::CompositeOntoNv12(std::vector<uint8_t>& nv12, uint32_t outW, uint32_t outH) {
    const size_t yBytes = static_cast<size_t>(outW) * outH;
    if (outW < 2 || outH < 2 || nv12.size() < yBytes + yBytes / 2) return;

    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_items.empty()) return;

    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + yBytes;
    std::wstring clockStr;

    for (const auto& item : m_items) {
        if (!item.visible) continue;
        std::wstring resolved = item.text;
        if (item.kind == OverlayKind::Clock) {
            if (clockStr.empty()) clockStr = FormatCurrentClockText();
            resolved = clockStr;
        }
        wchar_t sigBuf[256]{};
        std::swprintf(sigBuf, 256, L"%d|%u|%u|%d|%08X|%d|%08X|%.3f|%.3f|",
                      static_cast<int>(item.kind), outW, outH, item.fontSize,
                      item.colorArgb, item.bgEnabled ? 1 : 0, item.bgArgb,
                      item.boxWidthNorm, item.boxHeightNorm);
        std::wstring sig = std::wstring(sigBuf) + resolved + L"|" + item.imagePath;

        auto& cached = m_spriteCache[item.id];
        if (cached.signature != sig || cached.pbgra.empty()) {
            if (RasterizeSprite(item, resolved, outW, outH, cached)) {
                cached.signature = sig;
            } else {
                continue;
            }
        }
        if (cached.w == 0 || cached.h == 0 || cached.pbgra.empty()) continue;

        const uint32_t dstX = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(item.xNorm * outW)),
                                                               0, std::max(0, static_cast<int>(outW) - static_cast<int>(cached.w))));
        const uint32_t dstY = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(item.yNorm * outH)),
                                                               0, std::max(0, static_cast<int>(outH) - static_cast<int>(cached.h))));
        const uint32_t copyW = std::min(cached.w, outW - dstX);
        const uint32_t copyH = std::min(cached.h, outH - dstY);

        for (uint32_t y = 0; y < copyH; ++y) {
            const uint8_t* srcRow = cached.pbgra.data() + static_cast<size_t>(y) * cached.w * 4;
            for (uint32_t x = 0; x < copyW; ++x) {
                const uint8_t sa = srcRow[x * 4 + 3];
                if (sa == 0) continue;
                BlendPixelOntoNv12(yPlane, uvPlane, outW, outH, dstX + x, dstY + y,
                                   srcRow[x * 4 + 2], srcRow[x * 4 + 1], srcRow[x * 4 + 0], sa);
            }
        }
    }
}

void OverlayCompositor::CompositeCameraOntoNv12(std::vector<uint8_t>& nv12, uint32_t outW, uint32_t outH,
                                                const FrameCopy& camFrame, const AppConfig& cfg) {
    if (!cfg.camEnabled || camFrame.width == 0 || camFrame.height == 0 || camFrame.bgra.empty()) return;
    const size_t yBytes = static_cast<size_t>(outW) * outH;
    if (outW < 2 || outH < 2 || nv12.size() < yBytes + yBytes / 2) return;

    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + yBytes;

    const float aspect = (cfg.camShape == 2)
        ? 1.0f
        : (static_cast<float>(camFrame.height) / static_cast<float>(camFrame.width));
    const int dstW = std::clamp(static_cast<int>(std::lround(cfg.camWidthNorm * outW)), 32, static_cast<int>(outW));
    const int dstH = std::clamp(static_cast<int>(std::lround(dstW * aspect)), 32, static_cast<int>(outH));
    const int dstX = std::clamp(static_cast<int>(std::lround(cfg.camXNorm * outW)), 0, std::max(0, static_cast<int>(outW) - dstW));
    const int dstY = std::clamp(static_cast<int>(std::lround(cfg.camYNorm * outH)), 0, std::max(0, static_cast<int>(outH) - dstH));

    uint32_t srcCropX = 0, srcCropY = 0, srcCropW = camFrame.width, srcCropH = camFrame.height;
    if (cfg.camShape == 2 && camFrame.width != camFrame.height) {
        const uint32_t side = std::min(camFrame.width, camFrame.height);
        srcCropX = (camFrame.width - side) / 2;
        srcCropY = (camFrame.height - side) / 2;
        srcCropW = side;
        srcCropH = side;
    }

    const float halfW = static_cast<float>(dstW) * 0.5f;
    const float halfH = static_cast<float>(dstH) * 0.5f;
    const float rad = (cfg.camShape == 0) ? 0.0f
                    : (cfg.camShape == 1 ? std::min(halfW, halfH) * 0.16f : std::min(halfW, halfH));
    const float borderPx = cfg.camBorder ? std::max(2.0f, static_cast<float>(outH) * 0.0028f) : 0.0f;

    for (int y = 0; y < dstH; ++y) {
        const uint32_t sy = srcCropY + std::min(srcCropH - 1, static_cast<uint32_t>((static_cast<uint64_t>(y) * srcCropH) / dstH));
        const uint8_t* srcRow = camFrame.bgra.data() + static_cast<size_t>(sy) * camFrame.width * 4;
        const float dy = std::fabs((static_cast<float>(y) + 0.5f) - halfH) - (halfH - rad);

        for (int x = 0; x < dstW; ++x) {
            float dist = -10.0f;
            if (rad > 0.0f) {
                const float dx = std::fabs((static_cast<float>(x) + 0.5f) - halfW) - (halfW - rad);
                const float ox = std::max(dx, 0.0f);
                const float oy = std::max(dy, 0.0f);
                dist = std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - rad;
            } else if (borderPx > 0.0f) {
                const float edgeX = std::min(static_cast<float>(x) + 0.5f, static_cast<float>(dstW - x) - 0.5f);
                const float edgeY = std::min(static_cast<float>(y) + 0.5f, static_cast<float>(dstH - y) - 0.5f);
                dist = -std::min(edgeX, edgeY);
            }
            if (dist > 0.7f) continue;

            const uint32_t sx = srcCropX + std::min(srcCropW - 1, static_cast<uint32_t>((static_cast<uint64_t>(x) * srcCropW) / dstW));
            uint8_t sr = srcRow[sx * 4 + 2];
            uint8_t sg = srcRow[sx * 4 + 1];
            uint8_t sb = srcRow[sx * 4 + 0];
            if (borderPx > 0.0f && dist >= -borderPx) {
                sr = 239; sg = 68; sb = 68;
            }
            const uint8_t covA = (dist > -0.7f)
                ? static_cast<uint8_t>(std::lround(std::clamp(0.7f - dist, 0.0f, 1.0f) * 255.0f))
                : 255u;
            BlendPixelOntoNv12(yPlane, uvPlane, outW, outH,
                               static_cast<uint32_t>(dstX + x), static_cast<uint32_t>(dstY + y),
                               sr, sg, sb, covA);
        }
    }
}

}
