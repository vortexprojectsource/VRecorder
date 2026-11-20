#include "TrayIconRenderer.hpp"
#include "Win32Compat.hpp"
#include "../resources/resource.h"

#include <algorithm>

namespace vrec {

namespace {

int GetTrayIconSizePx(UINT dpi) {
    int sm = compat::GetSystemMetricsForDpiCompat(SM_CXSMICON, dpi);
    return (sm > 0) ? sm : 16;
}

HICON BitmapToIcon(HBITMAP hColorBmp, HBITMAP hMaskBmp) {
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hColorBmp;
    ii.hbmMask = hMaskBmp;
    return CreateIconIndirect(&ii);
}

}

TrayIconRenderer::TrayIconRenderer() = default;
TrayIconRenderer::~TrayIconRenderer() = default;

HICON TrayIconRenderer::CreateStudioTrayIcon(HINSTANCE hInst, bool isRecording, bool isPaused, UINT dpi) {
    const int size = std::max(16, GetTrayIconSizePx(dpi));
    HINSTANCE resInst = hInst ? hInst : GetModuleHandleW(nullptr);

    if (!isRecording) {
        HICON hIcon = static_cast<HICON>(LoadImageW(
            resInst, MAKEINTRESOURCEW(IDI_VR_APP), IMAGE_ICON,
            size, size, LR_DEFAULTCOLOR
        ));
        if (hIcon) return hIcon;
        hIcon = LoadIconW(resInst, MAKEINTRESOURCEW(IDI_VR_APP));
        if (hIcon) return hIcon;
    }

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (!memDc) return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hColor = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hColor || !bits) {
        if (hColor) DeleteObject(hColor);
        DeleteDC(memDc);
        return nullptr;
    }

    HBITMAP hOld = static_cast<HBITMAP>(SelectObject(memDc, hColor));

    std::memset(bits, 0, static_cast<size_t>(size) * size * 4);

    HICON hBase = static_cast<HICON>(LoadImageW(
        resInst, MAKEINTRESOURCEW(IDI_VR_APP), IMAGE_ICON,
        size, size, LR_DEFAULTCOLOR
    ));
    if (hBase) {
        DrawIconEx(memDc, 0, 0, hBase, size, size, 0, nullptr, DI_NORMAL);
        DestroyIcon(hBase);
    }

    if (isRecording) {
        const int bSize = std::max(6, size * 40 / 100);
        const int bx = size - bSize - 1;
        const int by = size - bSize - 1;

        HBRUSH brBadge = isPaused
            ? CreateSolidBrush(RGB(245, 245, 245))
            : CreateSolidBrush(RGB(235, 35, 35));
        HPEN penBadge = CreatePen(PS_SOLID, 1, RGB(20, 20, 22));
        HGDIOBJ oldBr = SelectObject(memDc, brBadge);
        HGDIOBJ oldPen = SelectObject(memDc, penBadge);

        if (isPaused) {
            const int barW = std::max(2, bSize * 28 / 100);
            RECT r1{bx, by, bx + barW, by + bSize};
            RECT r2{bx + bSize - barW, by, bx + bSize, by + bSize};
            FillRect(memDc, &r1, brBadge);
            FillRect(memDc, &r2, brBadge);
        } else {
            Ellipse(memDc, bx, by, bx + bSize, by + bSize);
        }

        SelectObject(memDc, oldBr);
        SelectObject(memDc, oldPen);
        DeleteObject(brBadge);
        DeleteObject(penBadge);
    }

    SelectObject(memDc, hOld);
    DeleteDC(memDc);

    HBITMAP hMask = CreateBitmap(size, size, 1, 1, nullptr);
    HICON hIcon = BitmapToIcon(hColor, hMask);
    if (hMask) DeleteObject(hMask);
    DeleteObject(hColor);
    return hIcon;
}

}
