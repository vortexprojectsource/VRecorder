#include "CaptureEngine.hpp"
#include "BicubicShaders.hpp"

#include <dwmapi.h>
#include <intrin.h>
#include <objbase.h>
#include <psapi.h>
#include <smmintrin.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace vrec {

namespace {

bool CpuHasSse41() noexcept {
    static const bool has = []() {
        int info[4]{};
        __cpuid(info, 1);
        return (info[2] & (1 << 19)) != 0;
    }();
    return has;
}

void StreamCopyRow(uint8_t* dst, const uint8_t* src, size_t bytes, bool useStream) noexcept {
    size_t i = 0;
    if (useStream && ((reinterpret_cast<uintptr_t>(src) & 15u) == 0)) {
        for (; i + 64 <= bytes; i += 64) {
            const __m128i a = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(src + i));
            const __m128i b = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(src + i + 16));
            const __m128i c = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(src + i + 32));
            const __m128i d = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(src + i + 48));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), a);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 16), b);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 32), c);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 48), d);
        }
    }
    if (i < bytes) {
        std::memcpy(dst + i, src + i, bytes - i);
    }
}

int64_t QueryQpcNow() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

int64_t QueryQpcFreq() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceFrequency(&li);
    return std::max<int64_t>(1, li.QuadPart);
}

BOOL CALLBACK FallbackMonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorInfo info{};
        info.index = static_cast<int>(list->size());
        info.desktopRect = mi.rcMonitor;
        info.width = mi.rcMonitor.right - mi.rcMonitor.left;
        info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        info.deviceName = mi.szDevice;
        wchar_t buf[96]{};
        std::swprintf(buf, 96, L"Display %d · %dx%d%s",
                      info.index + 1, info.width, info.height,
                      info.isPrimary ? L" (Primary)" : L"");
        info.label = buf;
        list->push_back(std::move(info));
    }
    return TRUE;
}

struct WinEnumCtx {
    HWND excludeHwnd = nullptr;
    std::vector<WindowInfo>* list = nullptr;
};

std::wstring QueryProcessBaseName(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(hProc, 0, path, &len)) {
        const wchar_t* slash = wcsrchr(path, L'\\');
        name = slash ? (slash + 1) : path;
    }
    CloseHandle(hProc);
    return name;
}

BOOL CALLBACK WindowEnumProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WinEnumCtx*>(lParam);
    if (!hwnd || hwnd == ctx->excludeHwnd) return TRUE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE;
    }

    const int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0) return TRUE;

    std::wstring title(static_cast<size_t>(titleLen) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), titleLen + 1);
    title.resize(wcslen(title.c_str()));
    if (title.empty()) return TRUE;

    if (_wcsicmp(title.c_str(), L"Program Manager") == 0 ||
        _wcsicmp(title.c_str(), L"Windows Input Experience") == 0) {
        return TRUE;
    }

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w < 64 || h < 64) return TRUE;

    WindowInfo wi{};
    wi.hwnd = hwnd;
    wi.title = std::move(title);
    wi.processName = QueryProcessBaseName(hwnd);
    wi.width = w;
    wi.height = h;
    ctx->list->push_back(std::move(wi));
    return TRUE;
}

void OverlaySoftwareCursorOnDc(HDC dc, int originX, int originY) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    ICONINFO ii{};
    int x = ci.ptScreenPos.x - originX;
    int y = ci.ptScreenPos.y - originY;
    if (GetIconInfo(ci.hCursor, &ii)) {
        x -= static_cast<int>(ii.xHotspot);
        y -= static_cast<int>(ii.yHotspot);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }
    DrawIconEx(dc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
}

struct PersistentCursorDib {
    HDC memDc = nullptr;
    HBITMAP bmp = nullptr;
    HBITMAP oldBmp = nullptr;
    void* pBits = nullptr;
    int size = 0;

    ~PersistentCursorDib() { Release(); }

    void Release() {
        if (bmp) {
            if (memDc && oldBmp) SelectObject(memDc, oldBmp);
            DeleteObject(bmp);
            bmp = nullptr;
            oldBmp = nullptr;
        }
        if (memDc) {
            DeleteDC(memDc);
            memDc = nullptr;
        }
        pBits = nullptr;
        size = 0;
    }

    bool Ensure(int cw) {
        if (bmp && memDc && pBits && size == cw) return true;
        Release();
        memDc = CreateCompatibleDC(nullptr);
        if (!memDc) return false;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = cw;
        bmi.bmiHeader.biHeight = -cw;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        if (bmp && pBits) {
            oldBmp = static_cast<HBITMAP>(SelectObject(memDc, bmp));
            size = cw;
            return true;
        }
        Release();
        return false;
    }
};

struct CachedCursorSprite {
    HCURSOR hCur = nullptr;
    int cw = 0;
    int hotX = 0;
    int hotY = 0;
    bool isAnimated = false;
    std::vector<uint32_t> onBlack;
    std::vector<uint32_t> onWhite;

    bool Prepare(HCURSOR cur, int boxW, PersistentCursorDib& dib) {
        if (!cur || boxW <= 0) return false;
        if (cur == hCur && cw == boxW && !isAnimated && !onBlack.empty()) {
            return true;
        }
        if (!dib.Ensure(boxW)) return false;

        ICONINFO ii{};
        hotX = 0;
        hotY = 0;
        if (GetIconInfo(cur, &ii)) {
            hotX = static_cast<int>(ii.xHotspot);
            hotY = static_cast<int>(ii.yHotspot);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }

        const size_t pxCount = static_cast<size_t>(boxW) * boxW;
        onBlack.resize(pxCount);
        onWhite.resize(pxCount);
        auto* bits32 = static_cast<uint32_t*>(dib.pBits);

        std::fill_n(bits32, pxCount, 0xFF000000u);
        DrawIconEx(dib.memDc, 0, 0, cur, boxW, boxW, 0, nullptr, DI_NORMAL);
        GdiFlush();
        std::memcpy(onBlack.data(), bits32, pxCount * 4);

        std::fill_n(bits32, pxCount, 0xFFFFFFFFu);
        DrawIconEx(dib.memDc, 0, 0, cur, boxW, boxW, 0, nullptr, DI_NORMAL);
        GdiFlush();
        std::memcpy(onWhite.data(), bits32, pxCount * 4);

        std::fill_n(bits32, pxCount, 0xFF000000u);
        const BOOL hasStep1 = DrawIconEx(dib.memDc, 0, 0, cur, boxW, boxW, 1, nullptr, DI_NORMAL);
        GdiFlush();
        isAnimated = (hasStep1 != FALSE && std::memcmp(bits32, onBlack.data(), pxCount * 4) != 0);

        hCur = cur;
        cw = boxW;
        return !isAnimated;
    }
};

void OverlayBgraCursor(std::vector<uint8_t>& frame, int fw, int fh, const RECT& originRect,
                       int scaleFromW = 0, int scaleFromH = 0) {
    if (frame.empty() || fw <= 0 || fh <= 0) return;
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

    const int boxSize = GetSystemMetrics(SM_CXCURSOR);
    const int cw = std::clamp(boxSize > 0 ? boxSize : 32, 16, 64);

    thread_local PersistentCursorDib s_cursorDib;
    thread_local CachedCursorSprite s_sprite;
    const bool useCached = s_sprite.Prepare(ci.hCursor, cw, s_cursorDib);

    const int rawX = ci.ptScreenPos.x - originRect.left;
    const int rawY = ci.ptScreenPos.y - originRect.top;
    const int mappedX = (scaleFromW > 0) ? static_cast<int>((static_cast<int64_t>(rawX) * fw) / scaleFromW) : rawX;
    const int mappedY = (scaleFromH > 0) ? static_cast<int>((static_cast<int64_t>(rawY) * fh) / scaleFromH) : rawY;
    const int mappedHotX = (scaleFromW > 0) ? static_cast<int>((static_cast<int64_t>(s_sprite.hotX) * fw) / scaleFromW) : s_sprite.hotX;
    const int mappedHotY = (scaleFromH > 0) ? static_cast<int>((static_cast<int64_t>(s_sprite.hotY) * fh) / scaleFromH) : s_sprite.hotY;
    const int curX = mappedX - mappedHotX;
    const int curY = mappedY - mappedHotY;
    if (curX < -cw || curY < -cw || curX >= fw || curY >= fh) return;

    const int sx0 = std::max(0, curX);
    const int sx1 = std::min(fw, curX + cw);
    const int sy0 = std::max(0, curY);
    const int sy1 = std::min(fh, curY + cw);
    if (sx0 >= sx1 || sy0 >= sy1) return;

    if (useCached) {
        auto* framePx = reinterpret_cast<uint32_t*>(frame.data());
        for (int sy = sy0; sy < sy1; ++sy) {
            const int cy = sy - curY;
            const uint32_t* blkRow = s_sprite.onBlack.data() + static_cast<size_t>(cy) * cw;
            const uint32_t* whtRow = s_sprite.onWhite.data() + static_cast<size_t>(cy) * cw;
            uint32_t* dstRow = framePx + static_cast<size_t>(sy) * fw;
            for (int sx = sx0; sx < sx1; ++sx) {
                const int cx = sx - curX;
                const uint32_t blk = blkRow[cx];
                const uint32_t wht = whtRow[cx];
                if (blk == 0xFF000000u && wht == 0xFFFFFFFFu) continue;

                const uint32_t d = dstRow[sx];
                auto blendCh = [](int dstV, int bV, int wV) -> uint32_t {
                    if (wV < bV) return static_cast<uint32_t>((dstV ^ bV) & 0xFF);
                    const int invA = wV - bV;
                    if (invA <= 0) return static_cast<uint32_t>(bV);
                    return static_cast<uint32_t>(std::min(255, bV + ((dstV * invA + 127) / 255)));
                };
                const uint32_t ob = blendCh(d & 0xFF, blk & 0xFF, wht & 0xFF);
                const uint32_t og = blendCh((d >> 8) & 0xFF, (blk >> 8) & 0xFF, (wht >> 8) & 0xFF);
                const uint32_t or_ = blendCh((d >> 16) & 0xFF, (blk >> 16) & 0xFF, (wht >> 16) & 0xFF);
                dstRow[sx] = 0xFF000000u | (or_ << 16) | (og << 8) | ob;
            }
        }
        return;
    }

    auto* dstBits = static_cast<uint8_t*>(s_cursorDib.pBits);
    const size_t patchRowBytes = static_cast<size_t>(cw) * 4;
    for (int row = 0; row < cw; ++row) {
        const int sy = curY + row;
        uint8_t* dstRow = dstBits + static_cast<size_t>(row) * patchRowBytes;
        if (sy < 0 || sy >= fh) {
            std::memset(dstRow, 0, patchRowBytes);
            continue;
        }
        if (sx0 > curX) std::memset(dstRow, 0, static_cast<size_t>(sx0 - curX) * 4);
        const size_t srcOff = (static_cast<size_t>(sy) * fw + sx0) * 4;
        std::memcpy(dstRow + static_cast<size_t>(sx0 - curX) * 4, &frame[srcOff], static_cast<size_t>(sx1 - sx0) * 4);
    }

    DrawIconEx(s_cursorDib.memDc, 0, 0, ci.hCursor, cw, cw, 0, nullptr, DI_NORMAL);
    GdiFlush();

    for (int row = 0; row < cw; ++row) {
        const int sy = curY + row;
        if (sy < 0 || sy >= fh) continue;
        const uint8_t* srcRow = dstBits + static_cast<size_t>(row) * patchRowBytes + static_cast<size_t>(sx0 - curX) * 4;
        const size_t dstOff = (static_cast<size_t>(sy) * fw + sx0) * 4;
        std::memcpy(&frame[dstOff], srcRow, static_cast<size_t>(sx1 - sx0) * 4);
    }
}

void OverlayNv12Cursor(std::vector<uint8_t>& nv12, int outW, int outH,
                       const RECT& monRect, const RECT& srcCropRc, const RECT& dstRc,
                       bool hasCur, const CURSORINFO& ci) {
    if (!hasCur || !ci.hCursor || outW < 2 || outH < 2 || nv12.empty()) return;
    const int cropW = std::max<int>(1, srcCropRc.right - srcCropRc.left);
    const int cropH = std::max<int>(1, srcCropRc.bottom - srcCropRc.top);
    const int dstW = std::max<int>(1, dstRc.right - dstRc.left);
    const int dstH = std::max<int>(1, dstRc.bottom - dstRc.top);

    const int boxSize = GetSystemMetrics(SM_CXCURSOR);
    const int cw = std::clamp(boxSize > 0 ? boxSize : 32, 16, 64);

    thread_local PersistentCursorDib s_cursorDib;
    thread_local CachedCursorSprite s_sprite;
    const bool useCached = s_sprite.Prepare(ci.hCursor, cw, s_cursorDib);
    if (!useCached && s_sprite.onBlack.empty()) return;

    const float relX = static_cast<float>((ci.ptScreenPos.x - monRect.left - s_sprite.hotX) - srcCropRc.left) / static_cast<float>(cropW);
    const float relY = static_cast<float>((ci.ptScreenPos.y - monRect.top  - s_sprite.hotY) - srcCropRc.top)  / static_cast<float>(cropH);
    const int outCurX = dstRc.left + static_cast<int>(std::lround(relX * static_cast<float>(dstW)));
    const int outCurY = dstRc.top  + static_cast<int>(std::lround(relY * static_cast<float>(dstH)));
    const int outCw = std::max(8, static_cast<int>(std::lround(static_cast<float>(cw) * static_cast<float>(dstW) / static_cast<float>(cropW))));
    const int outCh = std::max(8, static_cast<int>(std::lround(static_cast<float>(cw) * static_cast<float>(dstH) / static_cast<float>(cropH))));

    const int px0 = std::max(0, outCurX);
    const int px1 = std::min(outW, outCurX + outCw);
    const int py0 = std::max(0, outCurY);
    const int py1 = std::min(outH, outCurY + outCh);
    if (px0 >= px1 || py0 >= py1) return;

    const size_t yBytes = static_cast<size_t>(outW) * outH;
    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + yBytes;

    for (int py = py0; py < py1; ++py) {
        const int cy = std::clamp(((py - outCurY) * cw) / outCh, 0, cw - 1);
        const uint32_t* blkRow = s_sprite.onBlack.data() + static_cast<size_t>(cy) * cw;
        const uint32_t* whtRow = s_sprite.onWhite.data() + static_cast<size_t>(cy) * cw;
        for (int px = px0; px < px1; ++px) {
            const int cx = std::clamp(((px - outCurX) * cw) / outCw, 0, cw - 1);
            const uint32_t blk = blkRow[cx];
            const uint32_t wht = whtRow[cx];
            if (blk == 0xFF000000u && wht == 0xFFFFFFFFu) continue;

            uint8_t& yRef = yPlane[static_cast<size_t>(py) * outW + px];
            const size_t uvOff = static_cast<size_t>(py >> 1) * outW + (px & ~1);
            uint8_t& uRef = uvPlane[uvOff];
            uint8_t& vRef = uvPlane[uvOff + 1];

            const int cy7 = static_cast<int>(yRef) - 16;
            const int cu7 = static_cast<int>(uRef) - 128;
            const int cv7 = static_cast<int>(vRef) - 128;
            const int dr = std::clamp((298 * cy7 + 459 * cv7 + 128) >> 8, 0, 255);
            const int dg = std::clamp((298 * cy7 - 55 * cu7 - 136 * cv7 + 128) >> 8, 0, 255);
            const int db = std::clamp((298 * cy7 + 541 * cu7 + 128) >> 8, 0, 255);

            auto blendCh = [](int dstV, int bV, int wV) -> int {
                if (wV < bV) return (dstV ^ bV) & 0xFF;
                const int invA = wV - bV;
                if (invA <= 0) return bV;
                return std::min(255, bV + ((dstV * invA + 127) / 255));
            };
            const int ob = blendCh(db, static_cast<int>(blk & 0xFF), static_cast<int>(wht & 0xFF));
            const int og = blendCh(dg, static_cast<int>((blk >> 8) & 0xFF), static_cast<int>((wht >> 8) & 0xFF));
            const int or_ = blendCh(dr, static_cast<int>((blk >> 16) & 0xFF), static_cast<int>((wht >> 16) & 0xFF));

            yRef = static_cast<uint8_t>(std::clamp(((47 * or_ + 157 * og + 16 * ob + 128) >> 8) + 16, 16, 235));
            if ((px & 1) == 0 && (py & 1) == 0) {
                uRef = static_cast<uint8_t>(std::clamp(((-26 * or_ - 86 * og + 112 * ob + 128) >> 8) + 128, 16, 240));
                vRef = static_cast<uint8_t>(std::clamp(((112 * or_ - 102 * og - 10 * ob + 128) >> 8) + 128, 16, 240));
            }
        }
    }
}

}

struct CaptureEngine::FramePool : public std::enable_shared_from_this<CaptureEngine::FramePool> {
    std::mutex mu;
    std::vector<std::unique_ptr<FrameCopy>> freeSlots;

    std::shared_ptr<FrameCopy> Acquire() {
        std::unique_ptr<FrameCopy> item;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!freeSlots.empty()) {
                item = std::move(freeSlots.back());
                freeSlots.pop_back();
            }
        }
        if (!item) item = std::make_unique<FrameCopy>();

        std::weak_ptr<FramePool> wp = shared_from_this();
        FrameCopy* raw = item.release();
        return std::shared_ptr<FrameCopy>(raw, [wp](FrameCopy* p) {
            if (!p) return;
            if (auto pool = wp.lock()) {
                std::lock_guard<std::mutex> lk(pool->mu);
                if (pool->freeSlots.size() < 4) {
                    pool->freeSlots.emplace_back(p);
                    return;
                }
            }
            delete p;
        });
    }
};

CaptureEngine::CaptureEngine()
    : m_framePool(std::make_shared<FramePool>()) {}
CaptureEngine::~CaptureEngine() { Stop(); }

std::shared_ptr<FrameCopy> CaptureEngine::AcquirePooledFrame() {
    if (!m_framePool) m_framePool = std::make_shared<FramePool>();
    return m_framePool->Acquire();
}

void CaptureEngine::Start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread(&CaptureEngine::CaptureThreadProc, this);
}

void CaptureEngine::Stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void CaptureEngine::ConfigureTarget(int sourceType, int monitorIndex, const std::wstring& windowTitle,
                                    HWND excludeHwnd, bool showCursor, bool activeCaptureNeeded, int targetFps) {
    std::lock_guard<std::mutex> lk(m_cfgMutex);
    const bool changed = (m_sourceType != sourceType) || (m_monitorIndex != monitorIndex) ||
                         (m_windowTitle != windowTitle);
    m_sourceType = sourceType;
    m_monitorIndex = std::max(0, monitorIndex);
    m_windowTitle = windowTitle;
    m_excludeHwnd = excludeHwnd;
    m_showCursor = showCursor;
    m_activeCapture = activeCaptureNeeded;
    m_targetFps = std::clamp(targetFps, 8, 120);
    if (changed) {
        ++m_cfgEpoch;
        m_cfgEpochAtom.store(m_cfgEpoch, std::memory_order_release);
    }
}

void CaptureEngine::SetGpuNv12Target(bool enabled, uint32_t outW, uint32_t outH,
                                      int cropX, int cropY, int cropW, int cropH,
                                      int dstX, int dstY, int dstW, int dstH) {
    std::lock_guard<std::mutex> lk(m_cfgMutex);
    const bool changed = (m_gpuNv12Enabled != enabled) || (m_gpuOutW != outW) || (m_gpuOutH != outH) ||
                         (m_gpuCropX != cropX) || (m_gpuCropY != cropY) || (m_gpuCropW != cropW) || (m_gpuCropH != cropH) ||
                         (m_gpuDstX != dstX) || (m_gpuDstY != dstY) || (m_gpuDstW != dstW) || (m_gpuDstH != dstH);
    m_gpuNv12Enabled = enabled;
    m_gpuOutW = outW;
    m_gpuOutH = outH;
    m_gpuCropX = cropX;
    m_gpuCropY = cropY;
    m_gpuCropW = cropW;
    m_gpuCropH = cropH;
    m_gpuDstX = dstX;
    m_gpuDstY = dstY;
    m_gpuDstW = dstW;
    m_gpuDstH = dstH;
    if (changed) {
        ++m_cfgEpoch;
        m_cfgEpochAtom.store(m_cfgEpoch, std::memory_order_release);
    }
}

uint64_t CaptureEngine::GetCurrentTargetEpoch() const noexcept {
    return m_cfgEpochAtom.load(std::memory_order_acquire);
}

bool CaptureEngine::GetMonitorBounds(int monitorIdx, int& outW, int& outH) {
    const auto mons = EnumerateMonitors();
    if (mons.empty()) return false;
    const int safeIdx = std::clamp(monitorIdx, 0, static_cast<int>(mons.size()) - 1);
    outW = std::max(64, mons[static_cast<size_t>(safeIdx)].width);
    outH = std::max(64, mons[static_cast<size_t>(safeIdx)].height);
    return true;
}

std::vector<MonitorInfo> CaptureEngine::EnumerateMonitors() {
    std::vector<MonitorInfo> result;
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT aIdx = 0; factory->EnumAdapters1(aIdx, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++aIdx) {
            ComPtr<IDXGIOutput> output;
            for (UINT oIdx = 0; adapter->EnumOutputs(oIdx, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++oIdx) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                    MonitorInfo info{};
                    info.index = static_cast<int>(result.size());
                    info.adapterIndex = aIdx;
                    info.outputIndex = oIdx;
                    info.desktopRect = desc.DesktopCoordinates;
                    info.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                    info.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
                    info.isPrimary = (desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0);
                    info.deviceName = desc.DeviceName;
                    wchar_t buf[96]{};
                    std::swprintf(buf, 96, L"Display %d · %dx%d%s",
                                  info.index + 1, info.width, info.height,
                                  info.isPrimary ? L" (Primary)" : L"");
                    info.label = buf;
                    result.push_back(std::move(info));
                }
            }
        }
    }
    if (result.empty()) {
        EnumDisplayMonitors(nullptr, nullptr, FallbackMonitorEnumProc, reinterpret_cast<LPARAM>(&result));
    }
    return result;
}

std::vector<WindowInfo> CaptureEngine::EnumerateWindows(HWND excludeHwnd) {
    std::vector<WindowInfo> result;
    WinEnumCtx ctx{excludeHwnd, &result};
    EnumWindows(WindowEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return result;
}

FramePtr CaptureEngine::GetLatestSharedFrame(uint64_t lastKnownSeq) const {
    std::lock_guard<std::mutex> lk(m_frameMutex);
    if (!m_latestFrame || m_frameSeq == 0 || m_frameSeq == lastKnownSeq ||
        (m_latestFrame->bgra.empty() && m_latestFrame->nv12.empty())) {
        return nullptr;
    }
    return m_latestFrame;
}

bool CaptureEngine::CopyLatestFrame(FrameCopy& outFrame, uint64_t lastKnownSeq) const {
    FramePtr snap = GetLatestSharedFrame(lastKnownSeq);
    if (!snap) return false;
    outFrame.sequence = snap->sequence;
    if (!snap->bgra.empty()) {
        outFrame.width = snap->width;
        outFrame.height = snap->height;
        outFrame.bgra = snap->bgra;
        return true;
    }
    if (!snap->nv12.empty() && snap->nv12Width > 0 && snap->nv12Height > 0) {
        const uint32_t w = snap->nv12Width;
        const uint32_t h = snap->nv12Height;
        outFrame.width = w;
        outFrame.height = h;
        outFrame.bgra.resize(static_cast<size_t>(w) * h * 4);
        const uint8_t* yp = snap->nv12.data();
        const uint8_t* uvp = yp + static_cast<size_t>(w) * h;
        auto* dst32 = reinterpret_cast<uint32_t*>(outFrame.bgra.data());
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* yRow = yp + static_cast<size_t>(y) * w;
            const uint8_t* uvRow = uvp + static_cast<size_t>(y >> 1) * w;
            uint32_t* dRow = dst32 + static_cast<size_t>(y) * w;
            for (uint32_t x = 0; x < w; ++x) {
                const int c = static_cast<int>(yRow[x]) - 16;
                const int d = static_cast<int>(uvRow[x & ~1u]) - 128;
                const int e = static_cast<int>(uvRow[(x & ~1u) + 1]) - 128;
                const uint32_t r = static_cast<uint32_t>(std::clamp((298 * c + 459 * e + 128) >> 8, 0, 255));
                const uint32_t g = static_cast<uint32_t>(std::clamp((298 * c - 55 * d - 136 * e + 128) >> 8, 0, 255));
                const uint32_t b = static_cast<uint32_t>(std::clamp((298 * c + 541 * d + 128) >> 8, 0, 255));
                dRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        return true;
    }
    return false;
}

bool CaptureEngine::GetCurrentDimensions(uint32_t& outW, uint32_t& outH) const {
    std::lock_guard<std::mutex> lk(m_frameMutex);
    if (!m_latestFrame) return false;
    outW = m_latestFrame->width > 0 ? m_latestFrame->width : m_latestFrame->nv12Width;
    outH = m_latestFrame->height > 0 ? m_latestFrame->height : m_latestFrame->nv12Height;
    return outW > 0 && outH > 0;
}

double CaptureEngine::GetMeasuredCaptureFps() const {
    return m_measuredFps.load();
}

std::wstring CaptureEngine::GetBackendName() const {
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_backendName;
}

bool CaptureEngine::HasActiveFrame() const {
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_latestFrame &&
           ((m_latestFrame->width > 0 && m_latestFrame->height > 0 && !m_latestFrame->bgra.empty()) ||
            (m_latestFrame->nv12Width > 0 && m_latestFrame->nv12Height > 0 && !m_latestFrame->nv12.empty()));
}

void CaptureEngine::EnsureGdiScratch(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (m_gdiBmp && m_gdiW == w && m_gdiH == h) return;
    ReleaseGdiScratch();

    m_gdiDc = CreateCompatibleDC(nullptr);
    if (!m_gdiDc) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_gdiBmp = CreateDIBSection(m_gdiDc, &bmi, DIB_RGB_COLORS, &m_gdiBits, nullptr, 0);
    if (m_gdiBmp) {
        m_gdiOldBmp = static_cast<HBITMAP>(SelectObject(m_gdiDc, m_gdiBmp));
        m_gdiW = w;
        m_gdiH = h;
    }
}

void CaptureEngine::ReleaseGdiScratch() {
    if (m_gdiBmp) {
        if (m_gdiDc && m_gdiOldBmp) SelectObject(m_gdiDc, m_gdiOldBmp);
        DeleteObject(m_gdiBmp);
        m_gdiBmp = nullptr;
        m_gdiOldBmp = nullptr;
    }
    if (m_gdiDc) {
        DeleteDC(m_gdiDc);
        m_gdiDc = nullptr;
    }
    m_gdiBits = nullptr;
    m_gdiW = 0;
    m_gdiH = 0;
}

void CaptureEngine::ReleaseVideoProcessor() {
    m_bicRtv.Reset();
    m_bicDstTex.Reset();
    m_bicSrv.Reset();
    m_bicSrcTex.Reset();
    m_vpUseBicubic = false;
    m_vpOutView.Reset();
    m_vpInView.Reset();
    for (int s = 0; s < kStagingCount; ++s) {
        m_vpStaging[s].Reset();
    }
    m_vpOutTex.Reset();
    m_vpInTex.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();
    m_vpInW = m_vpInH = m_vpOutW = m_vpOutH = 0;
    m_vpOutIsNv12 = false;
    m_stagingPendingIdx = -1;
}

void CaptureEngine::ReleaseDxgi() {
    ReleaseVideoProcessor();
    m_bicRs.Reset();
    m_bicCb.Reset();
    m_bicPs.Reset();
    m_bicVs.Reset();
    m_vCtx.Reset();
    m_vDev.Reset();
    m_vpDisabled = false;
    for (int s = 0; s < kStagingCount; ++s) {
        m_stagingTex[s].Reset();
    }
    m_stagingWriteIdx = 0;
    m_stagingPendingIdx = -1;
    m_dupl.Reset();
    m_d3dCtx.Reset();
    m_d3dDevice.Reset();
    m_duplMonitorIdx = -1;
    m_duplW = 0;
    m_duplH = 0;
}

bool CaptureEngine::InitDxgiForMonitor(int monitorIdx, const std::vector<MonitorInfo>& mons) {
    ReleaseDxgi();
    if (mons.empty()) return false;
    const int safeIdx = std::clamp(monitorIdx, 0, static_cast<int>(mons.size()) - 1);
    const auto& mon = mons[static_cast<size_t>(safeIdx)];

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(mon.adapterIndex, adapter.GetAddressOf()))) {
        return false;
    }

    const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT dhr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                    fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
                                    m_d3dDevice.GetAddressOf(), nullptr, m_d3dCtx.GetAddressOf());
    if (FAILED(dhr)) {
        dhr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
                                m_d3dDevice.GetAddressOf(), nullptr, m_d3dCtx.GetAddressOf());
    }
    if (FAILED(dhr) || !m_d3dDevice) {
        return false;
    }

    m_d3dDevice.As(&m_vDev);
    m_d3dCtx.As(&m_vCtx);
    m_vpDisabled = (!m_vDev || !m_vCtx);

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(mon.outputIndex, output.GetAddressOf()))) {
        ReleaseDxgi();
        return false;
    }
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) {
        ReleaseDxgi();
        return false;
    }

    if (FAILED(output1->DuplicateOutput(m_d3dDevice.Get(), m_dupl.GetAddressOf()))) {
        ReleaseDxgi();
        return false;
    }

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    m_duplDesktopRect = desc.DesktopCoordinates;
    m_duplW = static_cast<uint32_t>(std::max<LONG>(1, desc.DesktopCoordinates.right - desc.DesktopCoordinates.left));
    m_duplH = static_cast<uint32_t>(std::max<LONG>(1, desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top));

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_duplW;
    td.Height = m_duplH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int s = 0; s < kStagingCount; ++s) {
        if (FAILED(m_d3dDevice->CreateTexture2D(&td, nullptr, m_stagingTex[s].GetAddressOf()))) {
            ReleaseDxgi();
            return false;
        }
    }
    m_stagingWriteIdx = 0;
    m_stagingPendingIdx = -1;

    m_duplMonitorIdx = safeIdx;
    m_dxgiFailStreak = 0;
    return true;
}

bool CaptureEngine::EnsureBicubicShaders() {
    if (m_bicVs && m_bicPs && m_bicCb && m_bicRs) return true;
    if (!m_d3dDevice) return false;
    if (FAILED(m_d3dDevice->CreateVertexShader(shaders::kBicubicVsBytecode, sizeof(shaders::kBicubicVsBytecode),
                                               nullptr, m_bicVs.ReleaseAndGetAddressOf())) || !m_bicVs) {
        return false;
    }
    if (FAILED(m_d3dDevice->CreatePixelShader(shaders::kBicubicPsBytecode, sizeof(shaders::kBicubicPsBytecode),
                                              nullptr, m_bicPs.ReleaseAndGetAddressOf())) || !m_bicPs) {
        m_bicVs.Reset();
        return false;
    }
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 48;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_d3dDevice->CreateBuffer(&cbd, nullptr, m_bicCb.ReleaseAndGetAddressOf())) || !m_bicCb) {
        m_bicVs.Reset();
        m_bicPs.Reset();
        return false;
    }
    D3D11_RASTERIZER_DESC rsd{};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    rsd.DepthClipEnable = FALSE;
    if (FAILED(m_d3dDevice->CreateRasterizerState(&rsd, m_bicRs.ReleaseAndGetAddressOf())) || !m_bicRs) {
        m_bicVs.Reset();
        m_bicPs.Reset();
        m_bicCb.Reset();
        return false;
    }
    return true;
}

void CaptureEngine::RunBicubicGpuPass(const RECT& srcRc, const RECT& dstRc) {
    if (!m_d3dCtx || !m_bicRtv || !m_bicSrv || !m_bicVs || !m_bicPs || !m_bicCb) return;
    const float clearCol[4] = {9.0f / 255.0f, 9.0f / 255.0f, 9.0f / 255.0f, 1.0f};
    m_d3dCtx->ClearRenderTargetView(m_bicRtv.Get(), clearCol);

    struct BicubicCbData {
        float srcRectPx[4];
        float srcBounds[4];
        float cubicA;
        float pad[3];
    } cb{};
    cb.srcRectPx[0] = static_cast<float>(srcRc.left);
    cb.srcRectPx[1] = static_cast<float>(srcRc.top);
    cb.srcRectPx[2] = static_cast<float>(std::max<LONG>(1, srcRc.right - srcRc.left));
    cb.srcRectPx[3] = static_cast<float>(std::max<LONG>(1, srcRc.bottom - srcRc.top));
    cb.srcBounds[0] = static_cast<float>(srcRc.left);
    cb.srcBounds[1] = static_cast<float>(srcRc.top);
    cb.srcBounds[2] = static_cast<float>(std::max<LONG>(srcRc.left, srcRc.right - 1));
    cb.srcBounds[3] = static_cast<float>(std::max<LONG>(srcRc.top, srcRc.bottom - 1));
    cb.cubicA = -0.75f;
    m_d3dCtx->UpdateSubresource(m_bicCb.Get(), 0, nullptr, &cb, 0, 0);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(dstRc.left);
    vp.TopLeftY = static_cast<float>(dstRc.top);
    vp.Width    = static_cast<float>(std::max<LONG>(1, dstRc.right - dstRc.left));
    vp.Height   = static_cast<float>(std::max<LONG>(1, dstRc.bottom - dstRc.top));
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtvs[1] = {m_bicRtv.Get()};
    ID3D11ShaderResourceView* srvs[1] = {m_bicSrv.Get()};
    ID3D11Buffer* cbs[1] = {m_bicCb.Get()};
    m_d3dCtx->OMSetRenderTargets(1, rtvs, nullptr);
    m_d3dCtx->RSSetState(m_bicRs.Get());
    m_d3dCtx->RSSetViewports(1, &vp);
    m_d3dCtx->IASetInputLayout(nullptr);
    m_d3dCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_d3dCtx->VSSetShader(m_bicVs.Get(), nullptr, 0);
    m_d3dCtx->PSSetShader(m_bicPs.Get(), nullptr, 0);
    m_d3dCtx->PSSetConstantBuffers(0, 1, cbs);
    m_d3dCtx->PSSetShaderResources(0, 1, srvs);
    m_d3dCtx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[1] = {nullptr};
    ID3D11RenderTargetView* nullRtv[1] = {nullptr};
    m_d3dCtx->PSSetShaderResources(0, 1, nullSrv);
    m_d3dCtx->OMSetRenderTargets(1, nullRtv, nullptr);
}

bool CaptureEngine::EnsureVideoProcessor(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH,
                                         bool outIsNv12, const RECT& srcRc, const RECT& dstRc) {
    if (m_vpDisabled || !m_vDev || !m_vCtx || !m_d3dDevice) return false;
    const LONG cropW = std::max<LONG>(1, srcRc.right - srcRc.left);
    const LONG cropH = std::max<LONG>(1, srcRc.bottom - srcRc.top);
    const LONG dstW  = std::max<LONG>(1, dstRc.right - dstRc.left);
    const LONG dstH  = std::max<LONG>(1, dstRc.bottom - dstRc.top);
    const bool wantBicubic = outIsNv12 && (cropW != dstW || cropH != dstH) && EnsureBicubicShaders();

    if (m_vp && m_vpInW == inW && m_vpInH == inH && m_vpOutW == outW && m_vpOutH == outH &&
        m_vpOutIsNv12 == outIsNv12 && m_vpUseBicubic == wantBicubic) {
        if (srcRc.left != m_vpSrcRect.left || srcRc.top != m_vpSrcRect.top ||
            srcRc.right != m_vpSrcRect.right || srcRc.bottom != m_vpSrcRect.bottom ||
            dstRc.left != m_vpDstRect.left || dstRc.top != m_vpDstRect.top ||
            dstRc.right != m_vpDstRect.right || dstRc.bottom != m_vpDstRect.bottom) {
            m_vpSrcRect = srcRc;
            m_vpDstRect = dstRc;
            if (!m_vpUseBicubic) {
                m_vCtx->VideoProcessorSetStreamSourceRect(m_vp.Get(), 0, TRUE, &m_vpSrcRect);
                m_vCtx->VideoProcessorSetStreamDestRect(m_vp.Get(), 0, TRUE, &m_vpDstRect);
            }
            m_stagingPendingIdx = -1;
        }
        return true;
    }

    ReleaseVideoProcessor();

    bool useBicubic = wantBicubic;
    if (useBicubic) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = inW;
        sd.Height = inH;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_TEXTURE2D_DESC dd = sd;
        dd.Width = outW;
        dd.Height = outH;
        dd.BindFlags = D3D11_BIND_RENDER_TARGET;

        if (FAILED(m_d3dDevice->CreateTexture2D(&sd, nullptr, m_bicSrcTex.GetAddressOf())) ||
            FAILED(m_d3dDevice->CreateShaderResourceView(m_bicSrcTex.Get(), nullptr, m_bicSrv.GetAddressOf())) ||
            FAILED(m_d3dDevice->CreateTexture2D(&dd, nullptr, m_bicDstTex.GetAddressOf())) ||
            FAILED(m_d3dDevice->CreateRenderTargetView(m_bicDstTex.Get(), nullptr, m_bicRtv.GetAddressOf()))) {
            ReleaseVideoProcessor();
            useBicubic = false;
        }
    }

    const uint32_t vpSrcW = useBicubic ? outW : inW;
    const uint32_t vpSrcH = useBicubic ? outH : inH;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = vpSrcW;
    cd.InputHeight = vpSrcH;
    cd.OutputWidth = outW;
    cd.OutputHeight = outH;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(m_vDev->CreateVideoProcessorEnumerator(&cd, m_vpEnum.GetAddressOf())) || !m_vpEnum ||
        FAILED(m_vDev->CreateVideoProcessor(m_vpEnum.Get(), 0, m_vp.GetAddressOf())) || !m_vp) {
        m_vpDisabled = true;
        ReleaseVideoProcessor();
        return false;
    }

    D3D11_TEXTURE2D_DESC inD{};
    inD.Width = vpSrcW;
    inD.Height = vpSrcH;
    inD.MipLevels = 1;
    inD.ArraySize = 1;
    inD.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    inD.SampleDesc.Count = 1;
    inD.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(m_d3dDevice->CreateTexture2D(&inD, nullptr, m_vpInTex.GetAddressOf())) || !m_vpInTex) {
        m_vpDisabled = true;
        ReleaseVideoProcessor();
        return false;
    }

    const DXGI_FORMAT outFmt = outIsNv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
    D3D11_TEXTURE2D_DESC outD{};
    outD.Width = outW;
    outD.Height = outH;
    outD.MipLevels = 1;
    outD.ArraySize = 1;
    outD.Format = outFmt;
    outD.SampleDesc.Count = 1;
    outD.Usage = D3D11_USAGE_DEFAULT;
    outD.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_d3dDevice->CreateTexture2D(&outD, nullptr, m_vpOutTex.GetAddressOf())) || !m_vpOutTex) {
        m_vpDisabled = true;
        ReleaseVideoProcessor();
        return false;
    }

    outD.Usage = D3D11_USAGE_STAGING;
    outD.BindFlags = 0;
    outD.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int s = 0; s < kStagingCount; ++s) {
        if (FAILED(m_d3dDevice->CreateTexture2D(&outD, nullptr, m_vpStaging[s].GetAddressOf())) || !m_vpStaging[s]) {
            m_vpDisabled = true;
            ReleaseVideoProcessor();
            return false;
        }
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(m_vDev->CreateVideoProcessorInputView(m_vpInTex.Get(), m_vpEnum.Get(), &ivd, m_vpInView.GetAddressOf()))) {
        m_vpDisabled = true;
        ReleaseVideoProcessor();
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(m_vDev->CreateVideoProcessorOutputView(m_vpOutTex.Get(), m_vpEnum.Get(), &ovd, m_vpOutView.GetAddressOf()))) {
        m_vpDisabled = true;
        ReleaseVideoProcessor();
        return false;
    }

    m_vpSrcRect = srcRc;
    m_vpDstRect = dstRc;
    m_vpUseBicubic = useBicubic;
    const RECT fullOutRc{0, 0, static_cast<LONG>(outW), static_cast<LONG>(outH)};
    const RECT vpSrcRc = useBicubic ? fullOutRc : srcRc;
    const RECT vpDstRc = useBicubic ? fullOutRc : dstRc;
    m_vCtx->VideoProcessorSetStreamSourceRect(m_vp.Get(), 0, TRUE, &vpSrcRc);
    m_vCtx->VideoProcessorSetStreamDestRect(m_vp.Get(), 0, TRUE, &vpDstRc);
    m_vCtx->VideoProcessorSetOutputTargetRect(m_vp.Get(), TRUE, &fullOutRc);

    D3D11_VIDEO_COLOR bg{};
    bg.RGBA = {0.035f, 0.035f, 0.035f, 1.0f};
    m_vCtx->VideoProcessorSetOutputBackgroundColor(m_vp.Get(), FALSE, &bg);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.RGB_Range = 0;
    inCs.YCbCr_Matrix = 1;
    m_vCtx->VideoProcessorSetStreamColorSpace(m_vp.Get(), 0, &inCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.YCbCr_Matrix = 1;
    outCs.Nominal_Range = outIsNv12 ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    m_vCtx->VideoProcessorSetOutputColorSpace(m_vp.Get(), &outCs);

    m_vpInW = inW;
    m_vpInH = inH;
    m_vpOutW = outW;
    m_vpOutH = outH;
    m_vpOutIsNv12 = outIsNv12;
    m_stagingPendingIdx = -1;
    return true;
}

CaptureEngine::MapStatus CaptureEngine::MapStagingToRaw(int slotIdx, bool allowWait, bool copyNv12, uint32_t mapW, uint32_t mapH) {
    if (slotIdx < 0 || slotIdx >= kStagingCount || !m_d3dCtx) return MapStatus::Failed;
    ID3D11Texture2D* tex = m_vp ? m_vpStaging[slotIdx].Get() : m_stagingTex[slotIdx].Get();
    if (!tex) return MapStatus::Failed;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_d3dCtx->Map(tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (!allowWait) return MapStatus::StillDrawing;
        hr = m_d3dCtx->Map(tex, 0, D3D11_MAP_READ, 0, &mapped);
    }
    if (FAILED(hr)) return MapStatus::Failed;

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    const bool useSse41 = CpuHasSse41();

    if (copyNv12) {
        const size_t yBytes = static_cast<size_t>(mapW) * mapH;
        const size_t totalBytes = yBytes + (yBytes / 2);
        if (m_rawDxgiNv12.size() != totalBytes) m_rawDxgiNv12.resize(totalBytes);
        for (uint32_t y = 0; y < mapH; ++y) {
            StreamCopyRow(m_rawDxgiNv12.data() + static_cast<size_t>(y) * mapW,
                          src + static_cast<size_t>(y) * mapped.RowPitch,
                          mapW, useSse41);
        }
        const uint32_t uvRows = mapH >> 1;
        for (uint32_t y = 0; y < uvRows; ++y) {
            StreamCopyRow(m_rawDxgiNv12.data() + yBytes + static_cast<size_t>(y) * mapW,
                          src + static_cast<size_t>(mapH + y) * mapped.RowPitch,
                          mapW, useSse41);
        }
        m_d3dCtx->Unmap(tex, 0);
        m_rawNv12W = mapW;
        m_rawNv12H = mapH;
        m_rawDxgiBgra.clear();
        m_rawDxgiW = 0;
        m_rawDxgiH = 0;
        return MapStatus::Ok;
    }

    const size_t bytes = static_cast<size_t>(mapW) * mapH * 4;
    if (m_rawDxgiBgra.size() != bytes) m_rawDxgiBgra.resize(bytes);
    const size_t rowBytes = static_cast<size_t>(mapW) * 4;
    if (mapped.RowPitch == rowBytes && useSse41) {
        StreamCopyRow(m_rawDxgiBgra.data(), src, bytes, true);
    } else if (mapped.RowPitch == rowBytes) {
        std::memcpy(m_rawDxgiBgra.data(), src, bytes);
    } else {
        for (uint32_t y = 0; y < mapH; ++y) {
            StreamCopyRow(m_rawDxgiBgra.data() + static_cast<size_t>(y) * rowBytes,
                          src + static_cast<size_t>(y) * mapped.RowPitch,
                          rowBytes, useSse41);
        }
    }
    m_d3dCtx->Unmap(tex, 0);
    m_rawDxgiW = mapW;
    m_rawDxgiH = mapH;
    m_rawDxgiNv12.clear();
    m_rawNv12W = 0;
    m_rawNv12H = 0;
    return MapStatus::Ok;
}

bool CaptureEngine::CaptureMonitorDxgi(int monitorIdx, const std::vector<MonitorInfo>& mons, bool showCursor,
                                         const GpuTransformCfg& gpuCfg, FrameCopy& outFrame, bool& outNewPublish) {
    outNewPublish = false;
    if (!m_dupl || m_duplMonitorIdx != monitorIdx) {
        const int64_t now = QueryQpcNow();
        if (now < m_nextDxgiRetryQpc) return false;
        if (!InitDxgiForMonitor(monitorIdx, mons)) {
            ++m_dxgiFailStreak;
            const int backoffSec = (m_dxgiFailStreak <= 1) ? 2 : ((m_dxgiFailStreak == 2) ? 6 : 20);
            m_nextDxgiRetryQpc = now + QueryQpcFreq() * backoffSec;
            return false;
        }
        m_rawDxgiBgra.clear();
        m_rawDxgiNv12.clear();
        m_lastCurPt = {-99999, -99999};
        m_lastCurH = nullptr;
        m_lastCurShown = false;
    }

    uint32_t targetW = m_duplW;
    uint32_t targetH = m_duplH;
    bool useVp = false;
    bool outIsNv12 = false;
    RECT srcRc{0, 0, static_cast<LONG>(m_duplW), static_cast<LONG>(m_duplH)};
    RECT dstRc{0, 0, static_cast<LONG>(m_duplW), static_cast<LONG>(m_duplH)};

    if (gpuCfg.enabled && gpuCfg.outW >= 64 && gpuCfg.outH >= 64) {
        targetW = gpuCfg.outW;
        targetH = gpuCfg.outH;
        outIsNv12 = true;
        srcRc.left   = std::clamp<LONG>(gpuCfg.srcRect.left, 0, static_cast<LONG>(m_duplW - 1));
        srcRc.top    = std::clamp<LONG>(gpuCfg.srcRect.top,  0, static_cast<LONG>(m_duplH - 1));
        srcRc.right  = std::clamp<LONG>(gpuCfg.srcRect.right > srcRc.left ? gpuCfg.srcRect.right : static_cast<LONG>(m_duplW), srcRc.left + 2, static_cast<LONG>(m_duplW));
        srcRc.bottom = std::clamp<LONG>(gpuCfg.srcRect.bottom > srcRc.top ? gpuCfg.srcRect.bottom : static_cast<LONG>(m_duplH), srcRc.top + 2, static_cast<LONG>(m_duplH));
        dstRc.left   = std::clamp<LONG>(gpuCfg.dstRect.left, 0, static_cast<LONG>(targetW - 2));
        dstRc.top    = std::clamp<LONG>(gpuCfg.dstRect.top,  0, static_cast<LONG>(targetH - 2));
        dstRc.right  = std::clamp<LONG>(gpuCfg.dstRect.right, dstRc.left + 2, static_cast<LONG>(targetW));
        dstRc.bottom = std::clamp<LONG>(gpuCfg.dstRect.bottom, dstRc.top + 2, static_cast<LONG>(targetH));
        useVp = EnsureVideoProcessor(m_duplW, m_duplH, targetW, targetH, true, srcRc, dstRc);
    } else if (gpuCfg.isPreviewDownscale && m_duplW > 1280) {
        targetW = (m_duplW >> 1) & ~1u;
        targetH = (m_duplH >> 1) & ~1u;
        outIsNv12 = false;
        dstRc = RECT{0, 0, static_cast<LONG>(targetW), static_cast<LONG>(targetH)};
        useVp = EnsureVideoProcessor(m_duplW, m_duplH, targetW, targetH, false, srcRc, dstRc);
    }

    if (!useVp) {
        if (m_vp) ReleaseVideoProcessor();
        targetW = m_duplW;
        targetH = m_duplH;
        outIsNv12 = false;
        srcRc = RECT{0, 0, static_cast<LONG>(m_duplW), static_cast<LONG>(m_duplH)};
        dstRc = srcRc;
    }

    const bool hasRaw = outIsNv12
        ? (!m_rawDxgiNv12.empty() && m_rawNv12W == targetW && m_rawNv12H == targetH)
        : (!m_rawDxgiBgra.empty() && m_rawDxgiW == targetW && m_rawDxgiH == targetH);

    CURSORINFO curInfo{};
    curInfo.cbSize = sizeof(curInfo);
    const bool hasCur = showCursor && GetCursorInfo(&curInfo) && (curInfo.flags & CURSOR_SHOWING) && curInfo.hCursor;
    const bool curChanged = (hasCur != m_lastCurShown) ||
                            (hasCur && (curInfo.ptScreenPos.x != m_lastCurPt.x ||
                                        curInfo.ptScreenPos.y != m_lastCurPt.y ||
                                        curInfo.hCursor != m_lastCurH));

    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> desktopRes;
    const UINT acqTimeoutMs = !hasRaw ? 12u : ((m_stagingPendingIdx < 0 && !curChanged) ? 2u : 0u);
    const HRESULT hr = m_dupl->AcquireNextFrame(acqTimeoutMs, &fi, desktopRes.GetAddressOf());

    auto publishCached = [&](bool changedPixels) -> bool {
        if (outIsNv12) {
            if (m_rawDxgiNv12.empty() || m_rawNv12W != targetW || m_rawNv12H != targetH) return false;
            if (!changedPixels && !curChanged) return true;
            outFrame.width = static_cast<uint32_t>(std::max<LONG>(1, srcRc.right - srcRc.left));
            outFrame.height = static_cast<uint32_t>(std::max<LONG>(1, srcRc.bottom - srcRc.top));
            outFrame.nv12Width = targetW;
            outFrame.nv12Height = targetH;
            outFrame.bgra.clear();
            outFrame.nv12.resize(m_rawDxgiNv12.size());
            std::memcpy(outFrame.nv12.data(), m_rawDxgiNv12.data(), m_rawDxgiNv12.size());
            if (hasCur) {
                OverlayNv12Cursor(outFrame.nv12, static_cast<int>(targetW), static_cast<int>(targetH),
                                  m_duplDesktopRect, srcRc, dstRc, hasCur, curInfo);
            }
        } else {
            if (m_rawDxgiBgra.empty() || m_rawDxgiW != targetW || m_rawDxgiH != targetH) return false;
            if (!changedPixels && !curChanged) return true;
            outFrame.width = targetW;
            outFrame.height = targetH;
            outFrame.nv12Width = 0;
            outFrame.nv12Height = 0;
            outFrame.nv12.clear();
            outFrame.bgra.resize(m_rawDxgiBgra.size());
            std::memcpy(outFrame.bgra.data(), m_rawDxgiBgra.data(), m_rawDxgiBgra.size());
            if (hasCur && targetW == m_duplW && targetH == m_duplH) {
                OverlayBgraCursor(outFrame.bgra, static_cast<int>(targetW), static_cast<int>(targetH), m_duplDesktopRect);
            } else if (hasCur) {
                OverlayBgraCursor(outFrame.bgra, static_cast<int>(targetW), static_cast<int>(targetH),
                                  m_duplDesktopRect, static_cast<int>(m_duplW), static_cast<int>(m_duplH));
            }
        }
        m_lastCurShown = hasCur;
        if (hasCur) { m_lastCurPt = curInfo.ptScreenPos; m_lastCurH = curInfo.hCursor; }
        outNewPublish = true;
        return true;
    };

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        bool drainedPending = false;
        if (m_stagingPendingIdx >= 0) {
            const MapStatus ms = MapStagingToRaw(m_stagingPendingIdx, gpuCfg.enabled, outIsNv12, targetW, targetH);
            if (ms == MapStatus::Ok) {
                drainedPending = true;
                m_stagingPendingIdx = -1;
            } else if (ms == MapStatus::Failed) {
                m_stagingPendingIdx = -1;
            }
        }
        if (!publishCached(drainedPending)) {
            return false;
        }
        return true;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST || FAILED(hr)) {
        ReleaseDxgi();
        m_nextDxgiRetryQpc = QueryQpcNow() + QueryQpcFreq();
        return false;
    }

    const bool hasDesktopUpdate = (fi.LastPresentTime.QuadPart != 0 || fi.AccumulatedFrames > 0 || !hasRaw);
    int submittedIdx = -1;
    if (hasDesktopUpdate) {
        ComPtr<ID3D11Texture2D> gpuTex;
        if (SUCCEEDED(desktopRes.As(&gpuTex)) && gpuTex) {
            if (m_stagingWriteIdx == m_stagingPendingIdx) {
                m_stagingWriteIdx = (m_stagingWriteIdx + 1) % kStagingCount;
            }
            submittedIdx = m_stagingWriteIdx;
            if (useVp) {
                if (m_vpUseBicubic && m_bicSrcTex && m_bicDstTex && m_bicSrv && m_bicRtv) {
                    m_d3dCtx->CopyResource(m_bicSrcTex.Get(), gpuTex.Get());
                    gpuTex.Reset();
                    desktopRes.Reset();
                    m_dupl->ReleaseFrame();
                    RunBicubicGpuPass(m_vpSrcRect, m_vpDstRect);
                    m_d3dCtx->CopyResource(m_vpInTex.Get(), m_bicDstTex.Get());
                } else {
                    m_d3dCtx->CopyResource(m_vpInTex.Get(), gpuTex.Get());
                    gpuTex.Reset();
                    desktopRes.Reset();
                    m_dupl->ReleaseFrame();
                }
                D3D11_VIDEO_PROCESSOR_STREAM st{};
                st.Enable = TRUE;
                st.pInputSurface = m_vpInView.Get();
                if (SUCCEEDED(m_vCtx->VideoProcessorBlt(m_vp.Get(), m_vpOutView.Get(), 0, 1, &st))) {
                    m_d3dCtx->CopyResource(m_vpStaging[submittedIdx].Get(), m_vpOutTex.Get());
                    if (gpuCfg.enabled) m_d3dCtx->Flush();
                    m_stagingWriteIdx = (m_stagingWriteIdx + 1) % kStagingCount;
                } else {
                    submittedIdx = -1;
                }
            } else {
                m_d3dCtx->CopyResource(m_stagingTex[submittedIdx].Get(), gpuTex.Get());
                gpuTex.Reset();
                desktopRes.Reset();
                m_dupl->ReleaseFrame();
                if (gpuCfg.enabled) m_d3dCtx->Flush();
                m_stagingWriteIdx = (m_stagingWriteIdx + 1) % kStagingCount;
            }
        } else {
            desktopRes.Reset();
            m_dupl->ReleaseFrame();
        }
    } else {
        desktopRes.Reset();
        m_dupl->ReleaseFrame();
    }

    bool updatedPixels = false;
    if (submittedIdx >= 0) {
        if (m_stagingPendingIdx >= 0) {
            MapStatus ms = MapStagingToRaw(m_stagingPendingIdx, false, outIsNv12, targetW, targetH);
            if (ms == MapStatus::StillDrawing && gpuCfg.enabled) {
                ms = MapStagingToRaw(m_stagingPendingIdx, true, outIsNv12, targetW, targetH);
            }
            updatedPixels = (ms == MapStatus::Ok);
            m_stagingPendingIdx = submittedIdx;
        } else if (!hasRaw) {
            if (gpuCfg.enabled) {
                updatedPixels = (MapStagingToRaw(submittedIdx, true, outIsNv12, targetW, targetH) == MapStatus::Ok);
                m_stagingPendingIdx = -1;
            } else {
                m_stagingPendingIdx = submittedIdx;
            }
        } else {
            m_stagingPendingIdx = submittedIdx;
        }
    } else if (m_stagingPendingIdx >= 0) {
        const MapStatus ms = MapStagingToRaw(m_stagingPendingIdx, gpuCfg.enabled, outIsNv12, targetW, targetH);
        if (ms == MapStatus::Ok) {
            updatedPixels = true;
            m_stagingPendingIdx = -1;
        } else if (ms == MapStatus::Failed) {
            m_stagingPendingIdx = -1;
        }
    }

    if (!publishCached(updatedPixels) && !hasRaw && !gpuCfg.enabled) {
        return false;
    }
    return true;
}

bool CaptureEngine::CaptureMonitorGdi(const MonitorInfo& mon, bool showCursor,
                                      std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH) {
    if (mon.width <= 0 || mon.height <= 0) return false;
    EnsureGdiScratch(mon.width, mon.height);
    if (!m_gdiDc || !m_gdiBits) return false;

    HDC srcDc = nullptr;
    bool needsDelete = false;
    if (!mon.deviceName.empty()) {
        srcDc = CreateDCW(L"DISPLAY", mon.deviceName.c_str(), nullptr, nullptr);
        if (srcDc) needsDelete = true;
    }
    if (!srcDc) {
        srcDc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        if (srcDc) needsDelete = true;
    }
    if (!srcDc) {
        srcDc = GetDC(nullptr);
    }
    if (!srcDc) return false;

    const int sx = (needsDelete && !mon.deviceName.empty()) ? 0 : mon.desktopRect.left;
    const int sy = (needsDelete && !mon.deviceName.empty()) ? 0 : mon.desktopRect.top;

    const BOOL bltOk = BitBlt(m_gdiDc, 0, 0, mon.width, mon.height,
                              srcDc, sx, sy,
                              SRCCOPY | CAPTUREBLT);
    if (needsDelete) DeleteDC(srcDc);
    else ReleaseDC(nullptr, srcDc);

    if (!bltOk) return false;

    if (showCursor) {
        OverlaySoftwareCursorOnDc(m_gdiDc, mon.desktopRect.left, mon.desktopRect.top);
    }
    GdiFlush();

    outW = static_cast<uint32_t>(mon.width);
    outH = static_cast<uint32_t>(mon.height);
    const size_t bytes = static_cast<size_t>(outW) * outH * 4;
    outBgra.resize(bytes);
    std::memcpy(outBgra.data(), m_gdiBits, bytes);
    return true;
}

bool CaptureEngine::CaptureWindowGdi(HWND targetHwnd, bool showCursor,
                                       std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH) {
    if (!IsWindow(targetHwnd) || !IsWindowVisible(targetHwnd) || IsIconic(targetHwnd)) return false;

    RECT rc{};
    if (!GetWindowRect(targetHwnd, &rc)) return false;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w < 16 || h < 16) return false;

    RECT visRc = rc;
    DwmGetWindowAttribute(targetHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visRc, sizeof(visRc));
    const int clipX = std::clamp<int>(visRc.left - rc.left, 0, w - 16);
    const int clipY = std::clamp<int>(visRc.top - rc.top, 0, h - 16);
    const int clipW = std::clamp<int>(visRc.right - rc.left - clipX, 16, w - clipX);
    const int clipH = std::clamp<int>(visRc.bottom - rc.top - clipY, 16, h - clipY);

    EnsureGdiScratch(w, h);
    if (!m_gdiDc || !m_gdiBits) return false;

    std::memset(m_gdiBits, 0, static_cast<size_t>(w) * h * 4);
    BOOL ok = PrintWindow(targetHwnd, m_gdiDc, 2 );
    if (!ok) {
        ok = PrintWindow(targetHwnd, m_gdiDc, 0);
    }
    if (!ok) {
        HDC scrDc = GetDC(nullptr);
        if (scrDc) {
            ok = BitBlt(m_gdiDc, 0, 0, w, h, scrDc, rc.left, rc.top, SRCCOPY);
            ReleaseDC(nullptr, scrDc);
        }
    }
    if (!ok) return false;

    if (showCursor) {
        OverlaySoftwareCursorOnDc(m_gdiDc, rc.left, rc.top);
    }
    GdiFlush();

    outW = static_cast<uint32_t>(clipW);
    outH = static_cast<uint32_t>(clipH);
    const size_t dstRowBytes = static_cast<size_t>(outW) * 4;
    outBgra.resize(dstRowBytes * outH);
    if (clipX == 0 && clipY == 0 && clipW == w && clipH == h) {
        std::memcpy(outBgra.data(), m_gdiBits, dstRowBytes * outH);
    } else {
        const auto* srcBase = static_cast<const uint8_t*>(m_gdiBits);
        const size_t srcStride = static_cast<size_t>(w) * 4;
        const size_t xOff = static_cast<size_t>(clipX) * 4;
        for (int row = 0; row < clipH; ++row) {
            std::memcpy(outBgra.data() + static_cast<size_t>(row) * dstRowBytes,
                        srcBase + static_cast<size_t>(clipY + row) * srcStride + xOff,
                        dstRowBytes);
        }
    }
    return true;
}

void CaptureEngine::CaptureThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HDESK hInputDesk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (hInputDesk) {
        SetThreadDesktop(hInputDesk);
        CloseDesktop(hInputDesk);
    } else {
        HDESK hDefDesk = OpenDesktopA("Default", 0, FALSE, GENERIC_READ);
        if (hDefDesk) {
            SetThreadDesktop(hDefDesk);
            CloseDesktop(hDefDesk);
        }
    }
    bool timer1msActive = false;

    HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t qpcFreq = QueryQpcFreq();
    int64_t fpsWindowStart = QueryQpcNow();
    uint32_t fpsFrameCount = 0;

    std::vector<MonitorInfo> cachedMonitors = EnumerateMonitors();
    HWND cachedWindow = nullptr;
    uint64_t lastEpoch = 0;
    int64_t lastMonRefreshQpc = QueryQpcNow();
    int inactiveTicks = 0;

    while (m_running.load()) {
        int sourceType = 0, monitorIdx = 0, targetFps = 60;
        std::wstring winTitle;
        HWND excludeHwnd = nullptr;
        bool showCursor = true, activeCapture = true;
        uint64_t epoch = 0;
        GpuTransformCfg gpuCfg{};
        {
            std::lock_guard<std::mutex> lk(m_cfgMutex);
            sourceType = m_sourceType;
            monitorIdx = m_monitorIndex;
            winTitle = m_windowTitle;
            excludeHwnd = m_excludeHwnd;
            showCursor = m_showCursor;
            activeCapture = m_activeCapture;
            targetFps = m_targetFps;
            epoch = m_cfgEpoch;
            gpuCfg.enabled = m_gpuNv12Enabled && (sourceType == 0 || sourceType == 2);
            gpuCfg.isPreviewDownscale = !gpuCfg.enabled && (sourceType == 0);
            gpuCfg.outW = m_gpuOutW;
            gpuCfg.outH = m_gpuOutH;
            gpuCfg.srcRect = RECT{m_gpuCropX, m_gpuCropY, m_gpuCropX + m_gpuCropW, m_gpuCropY + m_gpuCropH};
            gpuCfg.dstRect = RECT{m_gpuDstX, m_gpuDstY, m_gpuDstX + m_gpuDstW, m_gpuDstY + m_gpuDstH};
        }

        const bool want1ms = activeCapture && (targetFps >= 30 || gpuCfg.enabled);
        if (want1ms != timer1msActive) {
            if (want1ms) timeBeginPeriod(1);
            else timeEndPeriod(1);
            timer1msActive = want1ms;
        }

        if (!activeCapture) {
            if (++inactiveTicks >= 25 && (m_dupl || m_d3dDevice || m_gdiDc)) {
                ReleaseDxgi();
                ReleaseGdiScratch();
            }
            Sleep(45);
            continue;
        }
        inactiveTicks = 0;

        if (epoch != lastEpoch) {
            cachedWindow = nullptr;
            m_rawDxgiBgra.clear();
            m_rawDxgiNv12.clear();
            m_stagingPendingIdx = -1;
            m_nextDxgiRetryQpc = 0;
            m_dxgiFailStreak = 0;
            cachedMonitors = EnumerateMonitors();
            lastMonRefreshQpc = QueryQpcNow();
            lastEpoch = epoch;
            std::lock_guard<std::mutex> lk(m_frameMutex);
            m_latestFrame.reset();
            ++m_frameSeq;
            m_latestSeqAtom.store(m_frameSeq, std::memory_order_release);
            m_backendName = (sourceType == 1 && winTitle.empty()) ? L"Select App" : L"Switching";
        }

        const int64_t tickStart = QueryQpcNow();
        std::shared_ptr<FrameCopy> pkt = AcquirePooledFrame();
        pkt->targetEpoch = epoch;
        bool grabbed = false;
        bool needPublish = false;
        std::wstring usedBackend = L"DXGI";

        if (sourceType == 1) {
            if (winTitle.empty()) {
                std::lock_guard<std::mutex> lk(m_frameMutex);
                if (m_latestFrame || m_backendName != L"Select App") {
                    m_latestFrame.reset();
                    ++m_frameSeq;
                    m_latestSeqAtom.store(m_frameSeq, std::memory_order_release);
                    m_backendName = L"Select App";
                }
            } else {
                if (!cachedWindow || !IsWindow(cachedWindow)) {
                    cachedWindow = nullptr;
                    const auto wins = EnumerateWindows(excludeHwnd);
                    for (const auto& wi : wins) {
                        if (_wcsicmp(wi.title.c_str(), winTitle.c_str()) == 0) {
                            cachedWindow = wi.hwnd;
                            break;
                        }
                    }
                }
                if (cachedWindow && IsWindowVisible(cachedWindow) && !IsIconic(cachedWindow)) {
                    uint32_t w = 0, h = 0;
                    pkt->nv12.clear();
                    pkt->nv12Width = 0;
                    pkt->nv12Height = 0;
                    grabbed = CaptureWindowGdi(cachedWindow, showCursor, pkt->bgra, w, h);
                    pkt->width = w;
                    pkt->height = h;
                    needPublish = grabbed;
                    usedBackend = L"Window";
                } else {
                    std::lock_guard<std::mutex> lk(m_frameMutex);
                    m_latestFrame.reset();
                    ++m_frameSeq;
                    m_latestSeqAtom.store(m_frameSeq, std::memory_order_release);
                    m_backendName = (!cachedWindow || !IsWindow(cachedWindow)) ? L"App Not Found" : L"App Minimized";
                }
            }
        } else {

            if (cachedMonitors.empty() || (!m_dupl && (tickStart - lastMonRefreshQpc) > qpcFreq * 5)) {
                cachedMonitors = EnumerateMonitors();
                lastMonRefreshQpc = tickStart;
            }
            const int safeIdx = cachedMonitors.empty() ? 0 : std::clamp(monitorIdx, 0, static_cast<int>(cachedMonitors.size()) - 1);
            grabbed = CaptureMonitorDxgi(safeIdx, cachedMonitors, showCursor, gpuCfg, *pkt, needPublish);
            if (grabbed && m_vp && needPublish) {
                usedBackend = gpuCfg.enabled ? L"DXGI-GPU" : L"DXGI";
            }
            if (!grabbed && !cachedMonitors.empty()) {
                uint32_t w = 0, h = 0;
                pkt->nv12.clear();
                pkt->nv12Width = 0;
                pkt->nv12Height = 0;
                grabbed = CaptureMonitorGdi(cachedMonitors[static_cast<size_t>(safeIdx)], showCursor, pkt->bgra, w, h);
                pkt->width = w;
                pkt->height = h;
                needPublish = grabbed;
                usedBackend = L"GDI";
            }
        }

        if (grabbed && needPublish && ((pkt->width > 0 && pkt->height > 0 && !pkt->bgra.empty()) ||
                                       (pkt->nv12Width > 0 && pkt->nv12Height > 0 && !pkt->nv12.empty()))) {
            {
                std::lock_guard<std::mutex> lk(m_frameMutex);
                ++m_frameSeq;
                pkt->sequence = m_frameSeq;
                m_latestFrame = std::move(pkt);
                m_latestSeqAtom.store(m_frameSeq, std::memory_order_release);
                m_backendName = usedBackend;
            }
            ++fpsFrameCount;
        } else if (grabbed) {
            ++fpsFrameCount;
        }

        const int64_t now = QueryQpcNow();
        const double sec = static_cast<double>(now - fpsWindowStart) / static_cast<double>(qpcFreq);
        if (sec >= 0.5) {
            m_measuredFps.store(static_cast<double>(fpsFrameCount) / sec);
            fpsFrameCount = 0;
            fpsWindowStart = now;
        }

        const int64_t intervalQpc = qpcFreq / std::max(1, targetFps);
        const int64_t elapsedQpc = now - tickStart;
        if (elapsedQpc < intervalQpc) {
            const int64_t rem100ns = ((intervalQpc - elapsedQpc) * 10000000LL) / qpcFreq;
            if (hTimer && rem100ns > 1500LL) {
                LARGE_INTEGER li{};
                li.QuadPart = -rem100ns;
                SetWaitableTimerEx(hTimer, &li, 0, nullptr, nullptr, nullptr, 0);
                WaitForSingleObject(hTimer, 50);
            } else {
                const DWORD ms = static_cast<DWORD>((intervalQpc - elapsedQpc) * 1000LL / qpcFreq);
                if (ms > 0) Sleep(ms);
                else SwitchToThread();
            }
        }
    }

    if (hTimer) CloseHandle(hTimer);
    ReleaseDxgi();
    ReleaseGdiScratch();
    if (timer1msActive) timeEndPeriod(1);
    CoUninitialize();
}

}
