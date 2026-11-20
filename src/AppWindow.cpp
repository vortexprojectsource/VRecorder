#include "AppWindow.hpp"
#include "ColorCorrection.hpp"
#include "VectorIcons.hpp"
#include "Win32Compat.hpp"
#include "../resources/resource.h"

#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace vrec {

namespace {

constexpr wchar_t kWindowClassName[] = L"VRecorder_V1_HostWindow";
constexpr float kDefaultDipWidth  = 1280.0f;
constexpr float kDefaultDipHeight = 760.0f;
constexpr float kMinDipWidth      = 1040.0f;
constexpr float kMinDipHeight     = 640.0f;

constexpr UINT_PTR IDT_PREVIEW_POLL = 1001;
constexpr UINT_PTR IDT_STATS_POLL   = 1002;
constexpr UINT_PTR IDT_WINDOW_FADE  = 1003;
constexpr UINT_PTR IDT_STATUS_RESET = 1004;
constexpr UINT_PTR IDT_UI_ANIM      = 1005;
constexpr UINT_PTR IDT_CARET_BLINK  = 1006;

constexpr UINT kPreviewPollMs  = 80;
constexpr UINT kStatsPollMs    = 400;
constexpr UINT kFadeTimerMs    = 12;
constexpr UINT kStatusResetMs  = 4500;
constexpr UINT kUiAnimMs       = 16;
constexpr UINT kCaretBlinkMs   = 500;

constexpr int kHotkeyRecId     = 1;
constexpr int kHotkeyPauseId   = 2;

constexpr UINT kTrayIconId       = 100;
constexpr UINT IDM_TRAY_SHOW     = 40001;
constexpr UINT IDM_TRAY_REC      = 40002;
constexpr UINT IDM_TRAY_PAUSE    = 40003;
constexpr UINT IDM_TRAY_FOLDER   = 40004;
constexpr UINT IDM_TRAY_SETTINGS = 40005;
constexpr UINT IDM_TRAY_EXIT     = 40006;

constexpr DWORD kDwmUseImmersiveDarkMode   = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmRoundCorner            = 2;
constexpr DWORD kDwmBorderColor            = 34;

constexpr std::array<uint32_t, 5> kOverlayColors = {
    0xFFFFFFFFu,
    0xFFEF4444u,
    0xFF09090Bu,
    0xFFFACC15u,
    0xFF22D3EEu
};

D2D1_COLOR_F D2dColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept {
    return D2D1_COLOR_F{
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
        static_cast<float>(a) / 255.0f
    };
}

D2D1_COLOR_F ArgbToD2d(uint32_t argb) noexcept {
    return D2D1_COLOR_F{
        static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(argb & 0xFF) / 255.0f,
        static_cast<float>((argb >> 24) & 0xFF) / 255.0f
    };
}

bool PointInRectF(const D2D1_RECT_F& r, D2D1_POINT_2F p) noexcept {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

std::wstring FormatDurationHms(double totalSec) {
    const uint64_t s = static_cast<uint64_t>(std::max(0.0, std::floor(totalSec)));
    const uint64_t hrs = s / 3600;
    const uint64_t mins = (s % 3600) / 60;
    const uint64_t secs = s % 60;
    wchar_t buf[32]{};
    std::swprintf(buf, 32, L"%02llu:%02llu:%02llu", hrs, mins, secs);
    return buf;
}

std::wstring FormatBytesHuman(uint64_t bytes) {
    wchar_t buf[32]{};
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        std::swprintf(buf, 32, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else {
        std::swprintf(buf, 32, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

std::wstring ExtractFileNameOnly(const std::wstring& path) {
    const size_t sl = path.find_last_of(L"\\/");
    return (sl != std::wstring::npos) ? path.substr(sl + 1) : path;
}

UINT MapHotkeyToVk(const std::wstring& key) {
    if (_wcsicmp(key.c_str(), L"F8") == 0)  return VK_F8;
    if (_wcsicmp(key.c_str(), L"F9") == 0)  return VK_F9;
    if (_wcsicmp(key.c_str(), L"F10") == 0) return VK_F10;
    if (_wcsicmp(key.c_str(), L"F11") == 0) return VK_F11;
    return 0;
}

void ApplyEllipsisNoWrap(IDWriteFactory* dw, IDWriteTextFormat* fmt) {
    if (!dw || !fmt) return;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> sign;
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt, &sign))) {
        DWRITE_TRIMMING trim{};
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        fmt->SetTrimming(&trim, sign.Get());
    }
}

int ParseClampedInt(const std::wstring& s, int minVal, int maxVal, int fallback) {
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return fallback;
    return std::clamp(static_cast<int>(v), minVal, maxVal);
}

std::wstring ResolutionShortLabel(const AppConfig& cfg, uint32_t outW, uint32_t outH) {
    wchar_t buf[48]{};
    if (_wcsicmp(cfg.resolution.c_str(), L"native") == 0) {
        std::swprintf(buf, 48, L"Native (%ux%u)", outW, outH);
    } else if (_wcsicmp(cfg.resolution.c_str(), L"vertical") == 0) {
        wcscpy_s(buf, L"1080x1920 (Vert)");
    } else if (_wcsicmp(cfg.resolution.c_str(), L"custom") == 0) {
        std::swprintf(buf, 48, L"Custom %ux%u", outW, outH);
    } else {
        std::swprintf(buf, 48, L"%s (%ux%u)", cfg.resolution.c_str(), outW, outH);
    }
    return buf;
}

float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

float EaseOutCubic(float t) {
    const float u = Clamp01(t);
    const float inv = 1.0f - u;
    return 1.0f - inv * inv * inv;
}

D2D1_COLOR_F ColorLerp(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    const float k = Clamp01(t);
    return D2D1::ColorF(
        a.r + (b.r - a.r) * k,
        a.g + (b.g - a.g) * k,
        a.b + (b.b - a.b) * k,
        a.a + (b.a - a.a) * k
    );
}

uint64_t MakeAnimKey(uint32_t cid, int argInt, const std::wstring& argStr) {
    uint64_t h = 14695981039346656037ULL;
    for (wchar_t ch : argStr) {
        h ^= static_cast<uint64_t>(ch);
        h *= 1099511628211ULL;
    }
    const uint64_t ai = static_cast<uint64_t>(static_cast<uint32_t>(argInt));
    return (static_cast<uint64_t>(cid) << 48) ^ (ai << 32) ^ (h & 0xFFFFFFFFULL);
}

}

AppWindow::AppWindow(HINSTANCE hInst, bool startMinimized)
    : m_hInst(hInst)
    , m_startMinimized(startMinimized)
    , m_recorder(m_capture, m_camera, m_audio, m_overlays) {
    m_config = ConfigManager::Load();
    m_overlays.SetOverlays(m_config.overlays);
    if (!m_config.overlays.empty()) {
        m_selectedOverlayId = m_config.overlays.front().id;
    }
}

AppWindow::~AppWindow() {
    timeEndPeriod(1);
    m_recorder.StopRecording();
    m_capture.Stop();
    m_camera.Stop();
    m_audio.Stop();

    DiscardD2DDeviceResources();
    if (m_appIconLarge) DestroyIcon(m_appIconLarge);
    if (m_appIconSmall) DestroyIcon(m_appIconSmall);
    if (m_currentTrayIcon) DestroyIcon(m_currentTrayIcon);
}

bool AppWindow::Create() {
    timeBeginPeriod(1);
    if (!EnsureD2DFactoryResources()) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = m_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;

    HINSTANCE resInst = m_hInst ? m_hInst : GetModuleHandleW(nullptr);
    m_dpi = compat::GetDpiForSystemCompat();
    m_appIconLarge = static_cast<HICON>(LoadImageW(
        resInst, MAKEINTRESOURCEW(IDI_VR_APP), IMAGE_ICON,
        compat::GetSystemMetricsForDpiCompat(SM_CXICON, m_dpi),
        compat::GetSystemMetricsForDpiCompat(SM_CYICON, m_dpi),
        LR_DEFAULTCOLOR
    ));
    if (!m_appIconLarge) {
        m_appIconLarge = LoadIconW(resInst, MAKEINTRESOURCEW(IDI_VR_APP));
    }
    m_appIconSmall = static_cast<HICON>(LoadImageW(
        resInst, MAKEINTRESOURCEW(IDI_VR_APP), IMAGE_ICON,
        compat::GetSystemMetricsForDpiCompat(SM_CXSMICON, m_dpi),
        compat::GetSystemMetricsForDpiCompat(SM_CYSMICON, m_dpi),
        LR_DEFAULTCOLOR
    ));
    if (!m_appIconSmall) {
        m_appIconSmall = m_appIconLarge;
    }
    wc.hIcon = m_appIconLarge;
    wc.hIconSm = m_appIconSmall;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
    const DWORD exStyle = WS_EX_APPWINDOW;

    const int winW = compat::ScaleDip(kDefaultDipWidth, m_dpi);
    const int winH = compat::ScaleDip(kDefaultDipHeight, m_dpi);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int posX = workArea.left + ((workArea.right - workArea.left) - winW) / 2;
    const int posY = workArea.top + ((workArea.bottom - workArea.top) - winH) / 2;

    m_hwnd = CreateWindowExW(
        exStyle,
        kWindowClassName,
        L"VRecorder",
        style,
        posX, posY, winW, winH,
        nullptr, nullptr, m_hInst, this
    );
    if (!m_hwnd) {
        return false;
    }

    if (m_appIconLarge) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_appIconLarge));
    }
    if (m_appIconSmall) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_appIconSmall));
    }

    ApplyDwmTheme();
    ApplyAlwaysOnTop();
    RegisterGlobalHotkeys();

    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    if (m_startMinimized) {
        ShowWindow(m_hwnd, SW_HIDE);
    } else {
        StartWindowFade(FadeMode::In);
    }

    return true;
}

bool AppWindow::PreTranslateMessage(MSG* msg) {
    if (!msg) return false;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) {
        if (m_openPopup != PopupKind::None || m_activeEditField != EditFieldId::None) {
            OnKeyDown(msg->wParam);
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK AppWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<AppWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->WndProc(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void AppWindow::OnGetMinMaxInfo(MINMAXINFO* mmi) const {
    if (!mmi) return;
    const UINT dpi = m_dpi > 0 ? m_dpi : compat::GetDpiForSystemCompat();
    mmi->ptMinTrackSize.x = compat::ScaleDip(kMinDipWidth, dpi);
    mmi->ptMinTrackSize.y = compat::ScaleDip(kMinDipHeight, dpi);

    HMONITOR mon = m_hwnd ? MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (mon) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcMonitor.top;
        }
    }
}

LRESULT AppWindow::OnNcHitTest(int screenX, int screenY) const {
    POINT pt{screenX, screenY};
    ScreenToClient(m_hwnd, &pt);
    const D2D1_POINT_2F dip = PtToDip(pt.x, pt.y);

    if (!IsZoomed(m_hwnd)) {
        constexpr float b = 6.0f;
        const bool l = dip.x < b;
        const bool r = dip.x >= (m_clientW - b);
        const bool t = dip.y < b;
        const bool bt = dip.y >= (m_clientH - b);
        if (t && l)  return HTTOPLEFT;
        if (t && r)  return HTTOPRIGHT;
        if (bt && l) return HTBOTTOMLEFT;
        if (bt && r) return HTBOTTOMRIGHT;
        if (l)       return HTLEFT;
        if (r)       return HTRIGHT;
        if (t)       return HTTOP;
        if (bt)      return HTBOTTOM;
    }

    if (dip.y >= 0.0f && dip.y < 42.0f && dip.x < (m_clientW - 224.0f)) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT AppWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_GETICON:
        if (wParam == ICON_SMALL || wParam == 2 ) {
            return reinterpret_cast<LRESULT>(m_appIconSmall ? m_appIconSmall : m_appIconLarge);
        }
        return reinterpret_cast<LRESULT>(m_appIconLarge ? m_appIconLarge : m_appIconSmall);

    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_NCCALCSIZE:
        if (wParam == TRUE) return 0;
        break;

    case WM_NCACTIVATE:
        return TRUE;

    case WM_GETMINMAXINFO:
        OnGetMinMaxInfo(reinterpret_cast<MINMAXINFO*>(lParam));
        return 0;

    case WM_NCHITTEST:
        return OnNcHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_PRINT:
        if (lParam & PRF_CLIENT) OnPrintClient(reinterpret_cast<HDC>(wParam));
        return 0;

    case WM_PRINTCLIENT:
        OnPrintClient(reinterpret_cast<HDC>(wParam));
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (m_hoverId == ControlId::InputOverlayText || m_hoverId == ControlId::InputOutWidth ||
                m_hoverId == ControlId::InputOutHeight || m_hoverId == ControlId::InputCropX ||
                m_hoverId == ControlId::InputCropY || m_hoverId == ControlId::InputCropW ||
                m_hoverId == ControlId::InputCropH) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            if (m_hoverId == ControlId::CanvasOverlayResize || m_hoverId == ControlId::CanvasCropResize) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                return TRUE;
            }
            if (m_hoverId == ControlId::CanvasOverlayDrag || m_hoverId == ControlId::CanvasCropDrag) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            const bool hand = (m_hoverId != ControlId::None && m_hoverId != ControlId::ModalBackdrop);
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(m_hwnd, &pt);
        OnMouseWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_CHAR:
        OnChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_HOTKEY:
        if (wParam == kHotkeyRecId) ToggleStartStopRecording();
        else if (wParam == kHotkeyPauseId) TogglePauseRecording();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_DPICHANGED:
        OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            if (m_config.minimizeToTray) HideToTray();
            else MinimizeWindow();
            return 0;
        }
        if ((wParam & 0xFFF0) == SC_RESTORE && IsIconic(m_hwnd)) {
            DefWindowProcW(m_hwnd, msg, wParam, lParam);
            StartWindowFade(FadeMode::In);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (!m_closingForReal && m_config.closeToTray) {
            HideToTray();
            return 0;
        }
        if (!m_closingForReal && IsWindowVisible(m_hwnd)) {
            StartWindowFade(FadeMode::OutClose);
            return 0;
        }
        DestroyWindow(m_hwnd);
        return 0;

    case WM_DESTROY:
        OnDestroy();
        PostQuitMessage(0);
        return 0;

    case WM_TIMER:
        OnTimer(static_cast<UINT_PTR>(wParam));
        return 0;

    case WM_APP_SHOW_WINDOW:
        ShowMainWindow();
        return 0;

    case WM_APP_TRAY_ICON:
        OnTrayMessage(lParam);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TRAY_SHOW:
            if (IsWindowVisible(m_hwnd)) HideToTray();
            else ShowMainWindow();
            return 0;
        case IDM_TRAY_REC:
            ToggleStartStopRecording();
            return 0;
        case IDM_TRAY_PAUSE:
            TogglePauseRecording();
            return 0;
        case IDM_TRAY_FOLDER:
            OpenRecordingsFolder();
            return 0;
        case IDM_TRAY_SETTINGS:
            ShowMainWindow();
            SwitchToSettings(true);
            return 0;
        case IDM_TRAY_EXIT:
            m_closingForReal = true;
            DestroyWindow(m_hwnd);
            return 0;
        }
        break;
    }

    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

void AppWindow::OnCreate() {
    m_dpi = compat::GetDpiForWindowCompat(m_hwnd);
    if (m_dpi == 0) m_dpi = 96;

    RefreshAvailableLists();
    if (m_config.hasCaptureSource) m_selectedLayer = CanvasLayer::MainSource;
    else if (m_config.camAdded)    m_selectedLayer = CanvasLayer::Camera;
    else if (!m_config.overlays.empty()) {
        m_selectedLayer = CanvasLayer::Overlay;
        m_selectedOverlayId = m_config.overlays.front().id;
    } else {
        m_selectedLayer = CanvasLayer::None;
    }
    SyncEnginesFromConfig();

    m_capture.Start();
    m_camera.Start();
    m_audio.Start();

    const size_t encCount = m_encoders.size() > 1 ? (m_encoders.size() - 1) : 0;
    SetActionStatus(L"Ready · " + std::to_wstring(encCount) + L" Encoders Detected", false);

    InitTrayIcon();
    if (m_appIconLarge) SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_appIconLarge));
    if (m_appIconSmall) SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_appIconSmall));
    SetTimer(m_hwnd, IDT_PREVIEW_POLL, kPreviewPollMs, nullptr);
    SetTimer(m_hwnd, IDT_STATS_POLL, kStatsPollMs, nullptr);
    EnsureHwndRenderTarget();
    GetOrCreateLogoBitmap();
    m_introAnim = 0.01f;
    KickUiAnimation();
    InvalidateView();
}

void AppWindow::OnDestroy() {
    timeEndPeriod(1);
    UnregisterHotKey(m_hwnd, kHotkeyRecId);
    UnregisterHotKey(m_hwnd, kHotkeyPauseId);

    KillTimer(m_hwnd, IDT_PREVIEW_POLL);
    KillTimer(m_hwnd, IDT_STATS_POLL);
    KillTimer(m_hwnd, IDT_WINDOW_FADE);
    KillTimer(m_hwnd, IDT_STATUS_RESET);
    KillTimer(m_hwnd, IDT_UI_ANIM);
    KillTimer(m_hwnd, IDT_CARET_BLINK);

    m_recorder.StopRecording();
    m_capture.Stop();
    m_camera.Stop();
    m_audio.Stop();
    RemoveTrayIcon();
}

void AppWindow::OnSize(UINT, UINT) {
    if (!m_hwnd) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const UINT w = static_cast<UINT>(std::max(1L, rc.right - rc.left));
    const UINT h = static_cast<UINT>(std::max(1L, rc.bottom - rc.top));
    const float scale = 96.0f / static_cast<float>(m_dpi > 0 ? m_dpi : 96);
    m_clientW = std::max(kMinDipWidth * 0.5f, static_cast<float>(w) * scale);
    m_clientH = std::max(kMinDipHeight * 0.5f, static_cast<float>(h) * scale);

    if (m_hwndRt && FAILED(m_hwndRt->Resize(D2D1::SizeU(w, h)))) {
        DiscardD2DDeviceResources();
    }
    InvalidateView();
}

void AppWindow::OnDpiChanged(UINT newDpi, const RECT* suggestedRect) {
    if (newDpi > 0) m_dpi = newDpi;
    DiscardD2DDeviceResources();
    if (suggestedRect) {
        SetWindowPos(m_hwnd, nullptr,
                     suggestedRect->left, suggestedRect->top,
                     suggestedRect->right - suggestedRect->left,
                     suggestedRect->bottom - suggestedRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EnsureHwndRenderTarget();
    UpdateTrayIcon(true);
    InvalidateView();
}

void AppWindow::OnTimer(UINT_PTR timerId) {
    if (timerId == IDT_PREVIEW_POLL) {
        if (!IsWindowVisible(m_hwnd) || IsIconic(m_hwnd)) return;
        const RecorderStats recSt = m_recorder.GetStats();
        const bool isRec = recSt.isRecording;
        const ULONGLONG nowTick = GetTickCount64();
        if (isRec && (nowTick - m_lastRecPreviewTick < 200)) return;
        if (isRec) m_lastRecPreviewTick = nowTick;

        if ((m_settingsAnim > 0.004f && m_settingsAnim < 0.996f) ||
            (m_popupAnim > 0.004f && m_popupAnim < 0.996f)) {
            return;
        }
        if (!m_showingSettings) {
            bool needRedraw = false;
            if (!m_config.hasCaptureSource || !m_config.sourceVisible ||
                (m_config.sourceType == 1 && m_config.windowTitleFilter.empty())) {
                if (m_previewFramePtr) {
                    m_previewFramePtr.reset();
                    needRedraw = true;
                }
            } else {
                const uint64_t curPreviewSeq = m_previewFramePtr ? m_previewFramePtr->sequence : 0;
                const uint64_t latestCapSeq = m_capture.GetLatestSequence();
                if (latestCapSeq != curPreviewSeq) {
                    if (auto fresh = m_capture.GetLatestSharedFrame(curPreviewSeq)) {
                        m_previewFramePtr = std::move(fresh);
                        needRedraw = true;
                    }
                }
                if (!m_capture.HasActiveFrame() && m_config.sourceType == 1 && m_previewFramePtr) {
                    m_previewFramePtr.reset();
                    needRedraw = true;
                }
            }
            if (m_config.camAdded && m_config.camEnabled) {
                const uint64_t latestCamSeq = m_camera.GetLatestSequence();
                if (latestCamSeq != m_camPreviewFrame.sequence &&
                    m_camera.CopyLatestFrame(m_camPreviewFrame, m_camPreviewFrame.sequence)) {
                    needRedraw = true;
                }
            } else if (!m_camPreviewFrame.bgra.empty() || m_camPreviewFrame.width != 0) {
                m_camPreviewFrame.bgra.clear();
                m_camPreviewFrame.width = 0;
                m_camPreviewFrame.height = 0;
                m_camPreviewFrame.sequence = 0;
                needRedraw = true;
            }

            const float dMeter = m_config.desktopAudioMuted ? 0.0f : Clamp01(m_audio.GetDesktopMeterLevel());
            const float mMeter = (m_config.micMuted || _wcsicmp(m_config.micDeviceId.c_str(), L"none") == 0)
                               ? 0.0f : Clamp01(m_audio.GetMicMeterLevel());
            m_targetDeskMeter = dMeter;
            m_targetMicMeter  = mMeter;
            if (m_targetDeskMeter > 0.005f || m_targetMicMeter > 0.005f || m_uiDeskMeter > 0.005f || m_uiMicMeter > 0.005f) {
                KickUiAnimation();
            }
            m_lastUiDeskMeter = static_cast<double>(m_uiDeskMeter);
            m_lastUiMicMeter  = static_cast<double>(m_uiMicMeter);
            if (isRec && !recSt.isPaused) {
                m_recPulsePhase = std::fmod(m_recPulsePhase + 0.88f, 6.2831853f);
                needRedraw = true;
            }
            const int64_t recSec = static_cast<int64_t>(recSt.elapsedSec);
            if (recSec != m_lastUiRecSec && isRec) {
                m_lastUiRecSec = recSec;
                needRedraw = true;
            }
            for (const auto& ov : m_config.overlays) {
                if (ov.visible && ov.kind == OverlayKind::Clock) {
                    SYSTEMTIME stClk{};
                    GetLocalTime(&stClk);
                    if (static_cast<int>(stClk.wSecond) != m_lastClockUiSec) {
                        m_lastClockUiSec = stClk.wSecond;
                        needRedraw = true;
                    }
                    break;
                }
            }
            if (needRedraw) {
                InvalidateView();
            }
        }
    } else if (timerId == IDT_STATS_POLL) {
        const RecorderStats st = m_recorder.GetStats();
        const bool winActive = (IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd));
        const bool wantActive = st.isRecording || (winActive && !m_showingSettings);
        if (wantActive != m_lastSyncActive) {
            m_lastSyncActive = wantActive;
            SyncEnginesFromConfig();
        }
        UpdateTrayIcon(false);
        if (st.isRecording && winActive) {
            if (m_showingSettings && !st.isPaused) {
                m_recPulsePhase = std::fmod(m_recPulsePhase + 1.2f, 6.2831853f);
            }
            InvalidateView();
        }
    } else if (timerId == IDT_CARET_BLINK) {
        if (m_activeEditField != EditFieldId::None) {
            m_caretVisible = !m_caretVisible;
            InvalidateView();
        } else {
            KillTimer(m_hwnd, IDT_CARET_BLINK);
        }
    } else if (timerId == IDT_UI_ANIM) {
        LARGE_INTEGER nowQpc{}, freqQpc{};
        QueryPerformanceCounter(&nowQpc);
        QueryPerformanceFrequency(&freqQpc);
        float dtSec = 0.016f;
        if (m_animLastQpc > 0 && freqQpc.QuadPart > 0) {
            dtSec = static_cast<float>(nowQpc.QuadPart - m_animLastQpc) / static_cast<float>(freqQpc.QuadPart);
        }
        m_animLastQpc = nowQpc.QuadPart;
        dtSec = std::clamp(dtSec, 0.001f, 0.05f);
        StepUiAnimations(dtSec);
        InvalidateView();
        UpdateWindow(m_hwnd);
    } else if (timerId == IDT_WINDOW_FADE) {
        if (m_fadeMode == FadeMode::In) {
            m_fadeProgress = std::min(1.0, m_fadeProgress + 0.085);
            const double eased = 1.0 - std::pow(1.0 - m_fadeProgress, 3.0);
            ApplyLayeredAlpha(static_cast<BYTE>(std::lround(eased * 255.0)));
            if (m_fadeProgress >= 1.0) {
                KillTimer(m_hwnd, IDT_WINDOW_FADE);
                m_fadeMode = FadeMode::None;
                ClearLayeredStyle();
            }
        } else if (m_fadeMode == FadeMode::OutHide ||
                   m_fadeMode == FadeMode::OutMinimize ||
                   m_fadeMode == FadeMode::OutClose) {
            m_fadeProgress = std::max(0.0, m_fadeProgress - 0.10);
            const double eased = m_fadeProgress * m_fadeProgress;
            ApplyLayeredAlpha(static_cast<BYTE>(std::lround(eased * 255.0)));
            if (m_fadeProgress <= 0.0) {
                KillTimer(m_hwnd, IDT_WINDOW_FADE);
                const FadeMode doneMode = m_fadeMode;
                m_fadeMode = FadeMode::None;
                if (doneMode == FadeMode::OutHide) {
                    ShowWindow(m_hwnd, SW_HIDE);
                    ClearLayeredStyle();
                } else if (doneMode == FadeMode::OutMinimize) {
                    ShowWindow(m_hwnd, SW_MINIMIZE);
                    ClearLayeredStyle();
                } else if (doneMode == FadeMode::OutClose) {
                    m_closingForReal = true;
                    DestroyWindow(m_hwnd);
                }
            }
        } else {
            KillTimer(m_hwnd, IDT_WINDOW_FADE);
            ClearLayeredStyle();
        }
    } else if (timerId == IDT_STATUS_RESET) {
        KillTimer(m_hwnd, IDT_STATUS_RESET);
        if (!m_actionStatusText.empty()) {
            m_statusFadingOut = true;
            KickUiAnimation();
        }
    }
}

void AppWindow::OnTrayMessage(LPARAM lParam) {
    const UINT mouseMsg = LOWORD(lParam);
    if (mouseMsg == WM_LBUTTONUP || mouseMsg == WM_LBUTTONDBLCLK) {
        if (IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd)) HideToTray();
        else ShowMainWindow();
    } else if (mouseMsg == WM_RBUTTONUP || mouseMsg == WM_CONTEXTMENU) {
        ShowTrayContextMenu();
    }
}

AppWindow::ThemePalette AppWindow::GetPalette() const noexcept {
    ThemePalette p{};
    p.dark = (_wcsicmp(m_config.theme.c_str(), L"black") == 0);
    if (!p.dark) {
        p.bg           = D2dColor(255, 255, 255);
        p.surface      = D2dColor(248, 250, 252);
        p.surfaceAlt   = D2dColor(241, 245, 249);
        p.card         = D2dColor(255, 255, 255);
        p.border       = D2dColor(226, 232, 240);
        p.borderStrong = D2dColor(203, 213, 225);
        p.divider      = D2dColor(241, 245, 249);
        p.text         = D2dColor(9, 9, 11);
        p.textSub      = D2dColor(71, 85, 105);
        p.textMuted    = D2dColor(100, 116, 139);
        p.accent       = D2dColor(220, 38, 38);
        p.accentHover  = D2dColor(185, 28, 28);
        p.accentPress  = D2dColor(153, 27, 27);
        p.accentTint   = D2dColor(254, 242, 242);
        p.accentBorder = D2dColor(252, 165, 165);
        p.canvasBg     = D2dColor(15, 17, 21);
        p.amber        = D2dColor(217, 119, 6);
        p.green        = D2dColor(22, 163, 74);
    } else {
        p.bg           = D2dColor(9, 9, 11);
        p.surface      = D2dColor(17, 17, 20);
        p.surfaceAlt   = D2dColor(24, 24, 27);
        p.card         = D2dColor(17, 17, 20);
        p.border       = D2dColor(39, 39, 42);
        p.borderStrong = D2dColor(63, 63, 70);
        p.divider      = D2dColor(28, 28, 33);
        p.text         = D2dColor(250, 250, 250);
        p.textSub      = D2dColor(161, 161, 170);
        p.textMuted    = D2dColor(113, 113, 122);
        p.accent       = D2dColor(239, 68, 68);
        p.accentHover  = D2dColor(220, 38, 38);
        p.accentPress  = D2dColor(185, 28, 28);
        p.accentTint   = D2dColor(239, 68, 68, 38);
        p.accentBorder = D2dColor(239, 68, 68, 120);
        p.canvasBg     = D2dColor(5, 5, 7);
        p.amber        = D2dColor(245, 158, 11);
        p.green        = D2dColor(34, 197, 94);
    }
    return p;
}

void AppWindow::ApplyDwmTheme() {
    const BOOL dark = (_wcsicmp(m_config.theme.c_str(), L"black") == 0) ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(m_hwnd, kDwmWindowCornerPreference, &kDwmRoundCorner, sizeof(kDwmRoundCorner));
    COLORREF border = dark ? RGB(39, 39, 42) : RGB(203, 213, 225);
    DwmSetWindowAttribute(m_hwnd, kDwmBorderColor, &border, sizeof(border));
    MARGINS shadowMargins{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &shadowMargins);
}

void AppWindow::ApplyAlwaysOnTop() {
    SetWindowPos(m_hwnd,
                 m_config.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void AppWindow::RegisterGlobalHotkeys() {
    if (!m_hwnd) return;
    UnregisterHotKey(m_hwnd, kHotkeyRecId);
    UnregisterHotKey(m_hwnd, kHotkeyPauseId);

    const UINT vk = MapHotkeyToVk(m_config.recordHotkey);
    if (vk != 0) {
        RegisterHotKey(m_hwnd, kHotkeyRecId, MOD_NOREPEAT, vk);
        if (vk != VK_F10) RegisterHotKey(m_hwnd, kHotkeyPauseId, MOD_NOREPEAT, VK_F10);
    }
}

void AppWindow::InvalidateView() {
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool AppWindow::EnsureD2DFactoryResources() {
    if (!m_d2dFactory &&
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) {
        return false;
    }
    if (!m_dwFactory &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(m_dwFactory.GetAddressOf())))) {
        return false;
    }
    if (!m_roundStroke && m_d2dFactory) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f
        );
        m_d2dFactory->CreateStrokeStyle(&sp, nullptr, 0, m_roundStroke.GetAddressOf());
    }

    auto makeFmt = [&](ComPtr<IDWriteTextFormat>& out, float size, DWRITE_FONT_WEIGHT weight) -> bool {
        if (out) return true;
        if (FAILED(m_dwFactory->CreateTextFormat(
                L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                size, L"en-US", out.GetAddressOf()))) {
            return false;
        }
        ApplyEllipsisNoWrap(m_dwFactory.Get(), out.Get());
        return true;
    };

    return makeFmt(m_fmtReg9,    9.2f,  DWRITE_FONT_WEIGHT_REGULAR) &&
           makeFmt(m_fmtReg105,  10.5f, DWRITE_FONT_WEIGHT_REGULAR) &&
           makeFmt(m_fmtReg115,  11.5f, DWRITE_FONT_WEIGHT_REGULAR) &&
           makeFmt(m_fmtSemi10,  10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD) &&
           makeFmt(m_fmtSemi11,  11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD) &&
           makeFmt(m_fmtBold9,   9.0f,  DWRITE_FONT_WEIGHT_BOLD) &&
           makeFmt(m_fmtBold105, 10.5f, DWRITE_FONT_WEIGHT_BOLD) &&
           makeFmt(m_fmtBold115, 11.5f, DWRITE_FONT_WEIGHT_BOLD) &&
           makeFmt(m_fmtBold135, 13.5f, DWRITE_FONT_WEIGHT_BOLD) &&
           makeFmt(m_fmtBold145, 14.5f, DWRITE_FONT_WEIGHT_BOLD) &&
           makeFmt(m_fmtBold16,  16.0f, DWRITE_FONT_WEIGHT_BOLD);
}

bool AppWindow::EnsureHwndRenderTarget() {
    if (!m_hwnd || !EnsureD2DFactoryResources()) return false;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const UINT w = static_cast<UINT>(std::max(1L, rc.right - rc.left));
    const UINT h = static_cast<UINT>(std::max(1L, rc.bottom - rc.top));
    const float scale = 96.0f / static_cast<float>(m_dpi > 0 ? m_dpi : 96);
    m_clientW = std::max(kMinDipWidth * 0.5f, static_cast<float>(w) * scale);
    m_clientH = std::max(kMinDipHeight * 0.5f, static_cast<float>(h) * scale);

    if (!m_hwndRt) {
        const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(m_dpi),
            static_cast<float>(m_dpi)
        );
        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
            m_hwnd,
            D2D1::SizeU(w, h),
            D2D1_PRESENT_OPTIONS_NONE
        );
        if (FAILED(m_d2dFactory->CreateHwndRenderTarget(&rtProps, &hwndProps, m_hwndRt.GetAddressOf())) || !m_hwndRt) {
            return false;
        }
        m_hwndRt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_hwndRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    m_rt = m_hwndRt;
    if (!m_brush && FAILED(m_rt->CreateSolidColorBrush(D2dColor(0, 0, 0), m_brush.GetAddressOf()))) {
        return false;
    }
    return m_rt && m_brush;
}

void AppWindow::DiscardD2DDeviceResources() {
    m_overlayD2DBmps.clear();
    m_logoBitmap.Reset();
    m_previewBitmap.Reset();
    m_previewBmpW = 0;
    m_previewBmpH = 0;
    m_camBitmap.Reset();
    m_camBmpW = 0;
    m_camBmpH = 0;
    m_animLayer.Reset();
    m_brush.Reset();
    m_rt.Reset();
    m_hwndRt.Reset();
}

void AppWindow::RenderScene(const ThemePalette& pal) {
    m_hitControls.clear();
    m_rt->BeginDraw();
    m_rt->Clear(pal.bg);

    D2D1_MATRIX_3X2_F rootXform{};
    m_rt->GetTransform(&rootXform);

    const float iEase = (m_introAnim > 0.002f && m_introAnim < 0.998f) ? EaseOutCubic(m_introAnim) : 1.0f;
    const bool introActive = (iEase < 0.998f);
    if (introActive) {
        m_rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, 10.0f * (1.0f - iEase)) * rootXform);
        m_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                              D2D1::IdentityMatrix(), iEase), nullptr);
    }

    D2D1_MATRIX_3X2_F baseXform{};
    m_rt->GetTransform(&baseXform);
    const float sEase = EaseOutCubic(m_settingsAnim);
    const bool animatingSettings = (sEase > 0.002f && sEase < 0.998f);

    if (animatingSettings) {
        const D2D1_RECT_F bodyClip{0.0f, 43.0f, m_clientW, m_clientH};
        m_rt->PushAxisAlignedClip(bodyClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (m_showingSettings) {
            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(-32.0f * sEase, 0.0f) * baseXform);
            m_rt->PushLayer(D2D1::LayerParameters(bodyClip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), 1.0f - sEase), nullptr);
            DrawMainView(pal);
            m_rt->PopLayer();
            m_hitControls.clear();

            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(32.0f * (1.0f - sEase), 0.0f) * baseXform);
            m_rt->PushLayer(D2D1::LayerParameters(bodyClip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), sEase), nullptr);
            DrawSettingsView(pal);
            m_rt->PopLayer();
        } else {
            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(32.0f * (1.0f - sEase), 0.0f) * baseXform);
            m_rt->PushLayer(D2D1::LayerParameters(bodyClip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), sEase), nullptr);
            DrawSettingsView(pal);
            m_rt->PopLayer();
            m_hitControls.clear();

            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(-32.0f * sEase, 0.0f) * baseXform);
            m_rt->PushLayer(D2D1::LayerParameters(bodyClip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), 1.0f - sEase), nullptr);
            DrawMainView(pal);
            m_rt->PopLayer();
        }
        m_rt->SetTransform(baseXform);
        m_rt->PopAxisAlignedClip();
        DrawTitleBar(pal);
    } else {
        DrawTitleBar(pal);
        if (m_showingSettings) {
            DrawSettingsView(pal);
        } else {
            DrawMainView(pal);
        }
    }
    DrawPopups(pal);

    if (introActive) {
        m_rt->PopLayer();
        m_rt->SetTransform(rootXform);
    }

    const HRESULT endHr = m_rt->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET) {
        DiscardD2DDeviceResources();
    }
}

void AppWindow::OnPaint() {
    PAINTSTRUCT ps{};
    BeginPaint(m_hwnd, &ps);
    if (EnsureHwndRenderTarget()) {
        RenderScene(GetPalette());
    }
    EndPaint(m_hwnd, &ps);
    if (m_animNeedTimer) {
        m_animNeedTimer = false;
        KickUiAnimation();
    }
}

void AppWindow::OnPrintClient(HDC hdc) {
    if (!hdc || !m_hwnd || !EnsureD2DFactoryResources()) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));

    DiscardD2DDeviceResources();

    HDC memDc = CreateCompatibleDC(hdc);
    if (!memDc) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HBITMAP old = static_cast<HBITMAP>(SelectObject(memDc, bmp));
        ComPtr<ID2D1DCRenderTarget> dcRt;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(m_dpi),
            static_cast<float>(m_dpi)
        );
        if (SUCCEEDED(m_d2dFactory->CreateDCRenderTarget(&props, dcRt.GetAddressOf())) && dcRt) {
            dcRt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dcRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            const RECT bindRc{0, 0, w, h};
            if (SUCCEEDED(dcRt->BindDC(memDc, &bindRc))) {
                m_rt = dcRt;
                m_rt->CreateSolidColorBrush(D2dColor(0, 0, 0), m_brush.GetAddressOf());
                if (m_brush) RenderScene(GetPalette());
                BitBlt(hdc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
            }
        }
        DiscardD2DDeviceResources();
        SelectObject(memDc, old);
        DeleteObject(bmp);
    }
    DeleteDC(memDc);
}

void AppWindow::SyncEnginesFromConfig() {
    const RecorderStats st = m_recorder.GetStats();
    const bool winVisible = (IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd));
    const bool showActive = st.isRecording || (winVisible && !m_showingSettings);
    const bool needCapture = m_config.hasCaptureSource && m_config.sourceVisible && showActive;
    const int capFps = st.isRecording ? m_config.fps : 10;

    m_capture.ConfigureTarget(m_config.sourceType, m_config.monitorIndex,
                              m_config.windowTitleFilter, m_hwnd,
                              m_config.showCursor, needCapture, capFps);

    m_camera.Configure(m_config.camAdded && m_config.camEnabled && showActive,
                       m_config.camDeviceId, m_config.camMirror);

    m_audio.Configure(m_config.desktopDeviceId,
                      static_cast<float>(m_config.desktopVolume) / 100.0f,
                      m_config.desktopAudioMuted,
                      m_config.micDeviceId,
                      static_cast<float>(m_config.micVolume) / 100.0f,
                      m_config.micMuted);

    m_overlays.SetOverlays(m_config.overlays);
}

void AppWindow::RefreshAvailableLists() {
    m_monitors = CaptureEngine::EnumerateMonitors();
    m_windows  = CaptureEngine::EnumerateWindows(m_hwnd);
    m_cameras  = CameraEngine::EnumerateCameras();
    m_outAudioDevs = AudioEngine::EnumerateOutputDevices();
    m_inAudioDevs  = AudioEngine::EnumerateInputDevices();
    m_encoders     = EncoderManager::EnumerateAvailableEncoders();

    if (!m_monitors.empty() && m_config.cropW <= 64) {
        m_config.cropW = std::min(1280, m_monitors[0].width);
        m_config.cropH = std::min(720, m_monitors[0].height);
    }
}

void AppWindow::SaveAndApplyConfig() {
    ConfigManager::Save(m_config);
    ApplyDwmTheme();
    ApplyAlwaysOnTop();
    RegisterGlobalHotkeys();
    SyncEnginesFromConfig();
    InvalidateView();
}

void AppWindow::BrowseOutputFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Select Recordings Output Folder");
    if (SUCCEEDED(dlg->Show(m_hwnd))) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                m_config.outputDir = path;
                CoTaskMemFree(path);
                SaveAndApplyConfig();
                SetActionStatus(L"Output folder updated", false);
            }
        }
    }
}

void AppWindow::BrowseOverlayImage() {
    wchar_t fileBuf[1024]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"Select Overlay Image / Logo";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        if (OverlayItem* sel = FindSelectedOverlay(); sel && sel->kind == OverlayKind::Image) {
            sel->imagePath = fileBuf;
            sel->text = ExtractFileNameOnly(fileBuf);
        } else {
            OverlayItem ov{};
            ov.id = m_config.nextOverlayId++;
            ov.kind = OverlayKind::Image;
            ov.imagePath = fileBuf;
            ov.text = ExtractFileNameOnly(fileBuf);
            ov.xNorm = 0.80f;
            ov.yNorm = 0.08f;
            ov.boxWidthNorm = 0.16f;
            m_config.overlays.push_back(std::move(ov));
            m_selectedOverlayId = m_config.overlays.back().id;
        }
        SaveAndApplyConfig();
    }
}

void AppWindow::OpenRecordingsFolder() {
    const std::wstring dir = m_config.outputDir.empty() ? ConfigManager::GetDefaultOutputDirectory() : m_config.outputDir;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AppWindow::ToggleStartStopRecording() {
    CommitEditField(true);
    if (m_recorder.GetStats().isRecording) {
        m_recorder.StopRecording();
        SyncEnginesFromConfig();
        UpdateTrayIcon(true);
        const std::wstring saved = m_recorder.GetLastCompletedFile();
        if (!saved.empty()) {
            const std::wstring msg = L"Saved: " + ExtractFileNameOnly(saved);
            SetActionStatus(msg, false);
            if (m_config.showNotifications && !IsWindowVisible(m_hwnd)) {
                ShowTrayBalloon(L"VRecorder - Saved", msg.c_str(), false);
            }
        } else {
            SetActionStatus(L"Recording stopped", false);
        }
    } else {
        const bool anySourceActive =
            (m_config.hasCaptureSource && m_config.sourceVisible) ||
            (m_config.camAdded && m_config.camEnabled) ||
            !m_config.overlays.empty();
        if (!anySourceActive) {
            SetActionStatus(L"Add Display/Window or a Component first before recording!", true);
            InvalidateView();
            return;
        }
        if (m_config.hasCaptureSource && m_config.sourceVisible &&
            m_config.sourceType == 1 && m_config.windowTitleFilter.empty()) {
            SetActionStatus(L"Select an application window first before recording!", true);
            RefreshAvailableLists();
            InvalidateView();
            return;
        }
        SyncEnginesFromConfig();
        std::wstring err;
        if (m_recorder.StartRecording(m_config, m_encoders, err)) {
            SyncEnginesFromConfig();
            UpdateTrayIcon(true);
            uint32_t ow = 0, oh = 0;
            m_recorder.ResolveOutputDimensions(m_config, ow, oh);
            SetActionStatus(L"Recording started (" + std::to_wstring(ow) + L"x" + std::to_wstring(oh) +
                            L" @ " + std::to_wstring(m_config.fps) + L" FPS)", false);
        } else {
            SetActionStatus(err.empty() ? L"Failed to start recording" : err, true);
        }
    }
    KickUiAnimation();
    InvalidateView();
}

void AppWindow::TogglePauseRecording() {
    const RecorderStats st = m_recorder.GetStats();
    if (!st.isRecording) return;
    const bool nextPaused = !st.isPaused;
    m_recorder.SetPaused(nextPaused);
    UpdateTrayIcon(true);
    SetActionStatus(nextPaused ? L"Recording paused" : L"Recording resumed", false);
    KickUiAnimation();
    InvalidateView();
}

void AppWindow::ToggleMaximizeRestore() {
    if (!m_hwnd) return;
    ShowWindow(m_hwnd, IsZoomed(m_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

void AppWindow::SetActionStatus(const std::wstring& text, bool isError) {
    m_actionStatusText = text;
    m_actionStatusError = isError;
    m_statusFadingOut = false;
    m_statusAnim = text.empty() ? 0.0f : 0.01f;
    if (m_hwnd) SetTimer(m_hwnd, IDT_STATUS_RESET, kStatusResetMs, nullptr);
    KickUiAnimation();
    InvalidateView();
}

void AppWindow::BeginEditField(EditFieldId field, const std::wstring& initialText) {
    if (m_activeEditField != EditFieldId::None && m_activeEditField != field) {
        CommitEditField(true);
    }
    m_activeEditField = field;
    m_editBuffer = initialText;
    m_editSelectAll = true;
    m_caretVisible = true;
    SetTimer(m_hwnd, IDT_CARET_BLINK, kCaretBlinkMs, nullptr);
    InvalidateView();
}

void AppWindow::CommitEditField(bool applyChanges) {
    if (m_activeEditField == EditFieldId::None) return;
    const EditFieldId field = m_activeEditField;
    const std::wstring buf = m_editBuffer;
    m_activeEditField = EditFieldId::None;
    m_editBuffer.clear();
    m_editSelectAll = false;
    KillTimer(m_hwnd, IDT_CARET_BLINK);

    if (applyChanges) {
        const bool isRec = m_recorder.GetStats().isRecording;
        if (field == EditFieldId::OverlayText) {
            if (OverlayItem* sel = FindSelectedOverlay()) {
                sel->text = buf.empty() ? L"Text" : buf;
                SaveAndApplyConfig();
            }
        } else if (field == EditFieldId::OutWidth && !isRec) {
            m_config.customWidth = ParseClampedInt(buf, 64, 3840, m_config.customWidth) & ~1;
            m_config.resolution = L"custom";
            SaveAndApplyConfig();
        } else if (field == EditFieldId::OutHeight && !isRec) {
            m_config.customHeight = ParseClampedInt(buf, 64, 2160, m_config.customHeight) & ~1;
            m_config.resolution = L"custom";
            SaveAndApplyConfig();
        } else if (field == EditFieldId::CropX) {
            m_config.cropX = ParseClampedInt(buf, 0, 7680, m_config.cropX);
            SaveAndApplyConfig();
        } else if (field == EditFieldId::CropY) {
            m_config.cropY = ParseClampedInt(buf, 0, 4320, m_config.cropY);
            SaveAndApplyConfig();
        } else if (field == EditFieldId::CropW) {
            m_config.cropW = ParseClampedInt(buf, 64, 7680, m_config.cropW) & ~1;
            SaveAndApplyConfig();
        } else if (field == EditFieldId::CropH) {
            m_config.cropH = ParseClampedInt(buf, 64, 4320, m_config.cropH) & ~1;
            SaveAndApplyConfig();
        } else if (field == EditFieldId::SrcX) {
            m_config.srcPosX = static_cast<float>(ParseClampedInt(buf, -50, 95, static_cast<int>(std::lround(m_config.srcPosX * 100.0f)))) / 100.0f;
            SaveAndApplyConfig();
        } else if (field == EditFieldId::SrcY) {
            m_config.srcPosY = static_cast<float>(ParseClampedInt(buf, -50, 95, static_cast<int>(std::lround(m_config.srcPosY * 100.0f)))) / 100.0f;
            SaveAndApplyConfig();
        } else if (field == EditFieldId::SrcW) {
            m_config.srcScaleW = static_cast<float>(ParseClampedInt(buf, 15, 150, static_cast<int>(std::lround(m_config.srcScaleW * 100.0f)))) / 100.0f;
            SaveAndApplyConfig();
        } else if (field == EditFieldId::SrcH) {
            m_config.srcScaleH = static_cast<float>(ParseClampedInt(buf, 15, 150, static_cast<int>(std::lround(m_config.srcScaleH * 100.0f)))) / 100.0f;
            SaveAndApplyConfig();
        }
    }
    InvalidateView();
}

void AppWindow::AddOverlay(OverlayKind kind) {
    if (kind == OverlayKind::Image) {
        m_selectedOverlayId = 0;
        BrowseOverlayImage();
        return;
    }
    OverlayItem ov{};
    ov.id = m_config.nextOverlayId++;
    ov.kind = kind;
    ov.visible = true;
    const size_t offset = m_config.overlays.size() % 4;
    if (kind == OverlayKind::Text) {
        ov.xNorm = 0.04f;
        ov.yNorm = std::max(0.18f, 0.78f - static_cast<float>(offset) * 0.09f);
        ov.text = L"LIVE STREAM";
        ov.fontSize = 30;
        ov.colorArgb = 0xFFFFFFFF;
        ov.bgEnabled = true;
        ov.bgArgb = 0xC8DC2626;
    } else if (kind == OverlayKind::Clock) {
        ov.xNorm = 0.73f;
        ov.yNorm = std::min(0.45f, 0.04f + static_cast<float>(offset) * 0.07f);
        ov.text = L"Live Timestamp";
        ov.fontSize = 24;
        ov.colorArgb = 0xFFFFFFFF;
        ov.bgEnabled = true;
        ov.bgArgb = 0xB809090B;
    } else if (kind == OverlayKind::Box) {
        ov.xNorm = 0.05f + static_cast<float>(offset) * 0.04f;
        ov.yNorm = 0.14f + static_cast<float>(offset) * 0.06f;
        ov.text = L"Frame / Banner Box";
        ov.boxWidthNorm = 0.28f;
        ov.boxHeightNorm = 0.16f;
        ov.colorArgb = 0xFFEF4444;
        ov.bgEnabled = true;
        ov.bgArgb = 0x8009090B;
    }

    m_config.overlays.push_back(std::move(ov));
    m_selectedOverlayId = m_config.overlays.back().id;
    m_selectedLayer = CanvasLayer::Overlay;
    SaveAndApplyConfig();
    SetActionStatus(L"Overlay added — drag directly on Preview", false);
}

OverlayItem* AppWindow::FindSelectedOverlay() {
    for (auto& ov : m_config.overlays) {
        if (ov.id == m_selectedOverlayId) return &ov;
    }
    if (!m_config.overlays.empty()) {
        m_selectedOverlayId = m_config.overlays.front().id;
        return &m_config.overlays.front();
    }
    return nullptr;
}

void AppWindow::DeleteOverlay(uint32_t id) {
    auto& vec = m_config.overlays;
    vec.erase(std::remove_if(vec.begin(), vec.end(), [id](const OverlayItem& o) { return o.id == id; }), vec.end());
    if (m_selectedOverlayId == id) {
        m_selectedOverlayId = vec.empty() ? 0 : vec.front().id;
        if (vec.empty() && m_selectedLayer == CanvasLayer::Overlay) {
            if (m_config.hasCaptureSource) m_selectedLayer = CanvasLayer::MainSource;
            else if (m_config.camAdded)    m_selectedLayer = CanvasLayer::Camera;
            else                           m_selectedLayer = CanvasLayer::None;
        }
    }
    if (m_activeEditField == EditFieldId::OverlayText) CommitEditField(false);
    SaveAndApplyConfig();
}

D2D1_POINT_2F AppWindow::PtToDip(int px, int py) const noexcept {
    const float scale = 96.0f / static_cast<float>(m_dpi > 0 ? m_dpi : 96);
    return D2D1::Point2F(static_cast<float>(px) * scale, static_cast<float>(py) * scale);
}

const AppWindow::HitControl* AppWindow::HitTestDip(D2D1_POINT_2F pt) const {
    for (auto it = m_hitControls.rbegin(); it != m_hitControls.rend(); ++it) {
        if (PointInRectF(it->rect, pt)) {
            return it->enabled ? &(*it) : nullptr;
        }
    }
    return nullptr;
}

bool AppWindow::IsControlHovered(ControlId id, int argInt, const std::wstring& argStr) const {
    return m_hoverId == id && m_hoverArgInt == argInt && m_hoverArgStr == argStr;
}

bool AppWindow::IsControlPressed(ControlId id, int argInt, const std::wstring& argStr) const {
    return m_pressId == id && m_pressArgInt == argInt && m_pressArgStr == argStr;
}

void AppWindow::RegisterHit(ControlId id, const D2D1_RECT_F& rc, bool enabled, int argInt, std::wstring argStr) {
    HitControl hc{};
    hc.id = id;
    hc.rect = rc;
    hc.enabled = enabled;
    hc.argInt = argInt;
    hc.argStr = std::move(argStr);
    m_hitControls.push_back(std::move(hc));
}

void AppWindow::OnMouseMove(int px, int py) {
    if (!m_mouseTracking) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_hwnd;
        TrackMouseEvent(&tme);
        m_mouseTracking = true;
    }

    const D2D1_POINT_2F dip = PtToDip(px, py);

    if (m_canvasDrag != CanvasDragMode::None) {
        const float drawW = std::max(1.0f, m_activeDrawRc.right - m_activeDrawRc.left);
        const float drawH = std::max(1.0f, m_activeDrawRc.bottom - m_activeDrawRc.top);
        const float dxNorm = (dip.x - m_dragStartDip.x) / drawW;
        const float dyNorm = (dip.y - m_dragStartDip.y) / drawH;

        if (m_canvasDrag == CanvasDragMode::MoveOverlay) {
            if (OverlayItem* ov = FindSelectedOverlay()) {
                ov->xNorm = std::clamp(m_dragOrigXNorm + dxNorm, 0.0f, 0.94f);
                ov->yNorm = std::clamp(m_dragOrigYNorm + dyNorm, 0.0f, 0.94f);
                m_overlays.SetOverlays(m_config.overlays);
                InvalidateView();
            }
        } else if (m_canvasDrag == CanvasDragMode::ResizeOverlay) {
            if (OverlayItem* ov = FindSelectedOverlay()) {
                if (ov->kind == OverlayKind::Text || ov->kind == OverlayKind::Clock) {
                    const int deltaPt = static_cast<int>(std::lround((dip.x - m_dragStartDip.x) * 0.25f));
                    ov->fontSize = std::clamp(m_dragOrigFont + deltaPt, 12, 96);
                } else {
                    ov->boxWidthNorm  = std::clamp(m_dragOrigWNorm + dxNorm, 0.05f, 0.95f);
                    ov->boxHeightNorm = std::clamp(m_dragOrigHNorm + dyNorm, 0.04f, 0.95f);
                }
                m_overlays.SetOverlays(m_config.overlays);
                InvalidateView();
            }
        } else if (m_canvasDrag == CanvasDragMode::MoveCrop || m_canvasDrag == CanvasDragMode::ResizeCrop) {
            const uint32_t srcW = (m_previewFramePtr && m_previewFramePtr->width > 0) ? m_previewFramePtr->width : 1920u;
            const uint32_t srcH = (m_previewFramePtr && m_previewFramePtr->height > 0) ? m_previewFramePtr->height : 1080u;
            const int dSrcX = static_cast<int>(std::lround(dxNorm * static_cast<float>(srcW)));
            const int dSrcY = static_cast<int>(std::lround(dyNorm * static_cast<float>(srcH)));
            if (m_canvasDrag == CanvasDragMode::MoveCrop) {
                m_config.cropX = std::clamp(m_dragOrigCropX + dSrcX, 0, std::max(0, static_cast<int>(srcW) - m_config.cropW));
                m_config.cropY = std::clamp(m_dragOrigCropY + dSrcY, 0, std::max(0, static_cast<int>(srcH) - m_config.cropH));
            } else {
                m_config.cropW = std::clamp((m_dragOrigCropW + dSrcX) & ~1, 64, static_cast<int>(srcW) - m_config.cropX);
                m_config.cropH = std::clamp((m_dragOrigCropH + dSrcY) & ~1, 64, static_cast<int>(srcH) - m_config.cropY);
            }
            InvalidateView();
        } else if (m_canvasDrag == CanvasDragMode::MoveSource) {
            m_config.srcPosX = std::clamp(m_dragOrigSrcX + dxNorm, -0.45f, 0.92f);
            m_config.srcPosY = std::clamp(m_dragOrigSrcY + dyNorm, -0.45f, 0.92f);
            InvalidateView();
        } else if (m_canvasDrag == CanvasDragMode::ResizeSource) {
            float nx = m_dragOrigSrcX, ny = m_dragOrigSrcY, nw = m_dragOrigSrcW, nh = m_dragOrigSrcH;
            if (m_dragResizeCorner == 0) {
                nx += dxNorm; nw -= dxNorm;
                ny += dyNorm; nh -= dyNorm;
            } else if (m_dragResizeCorner == 1) {
                nw += dxNorm;
                ny += dyNorm; nh -= dyNorm;
            } else if (m_dragResizeCorner == 2) {
                nx += dxNorm; nw -= dxNorm;
                nh += dyNorm;
            } else {
                nw += dxNorm;
                nh += dyNorm;
            }
            m_config.srcScaleW = std::clamp(nw, 0.15f, 1.50f);
            m_config.srcScaleH = std::clamp(nh, 0.15f, 1.50f);
            m_config.srcPosX   = std::clamp(nx, -0.50f, 0.92f);
            m_config.srcPosY   = std::clamp(ny, -0.50f, 0.92f);
            InvalidateView();
        } else if (m_canvasDrag == CanvasDragMode::MoveCamera) {
            m_config.camXNorm = std::clamp(m_dragOrigCamX + dxNorm, 0.0f, 0.92f);
            m_config.camYNorm = std::clamp(m_dragOrigCamY + dyNorm, 0.0f, 0.92f);
            InvalidateView();
        } else if (m_canvasDrag == CanvasDragMode::ResizeCamera) {
            m_config.camWidthNorm = std::clamp(m_dragOrigCamW + dxNorm, 0.08f, 0.70f);
            InvalidateView();
        }
        return;
    }

    if (m_draggingSliderId != ControlId::None) {
        for (const auto& hc : m_hitControls) {
            if (hc.id == m_draggingSliderId && hc.isSlider) {
                ApplySliderFromDipX(hc, dip.x);
                break;
            }
        }
        return;
    }

    const HitControl* hit = HitTestDip(dip);
    const ControlId nextId = hit ? hit->id : ControlId::None;
    const int nextArgInt = hit ? hit->argInt : 0;
    const std::wstring nextArgStr = hit ? hit->argStr : L"";

    if (nextId != m_hoverId || nextArgInt != m_hoverArgInt || nextArgStr != m_hoverArgStr) {
        m_hoverId = nextId;
        m_hoverArgInt = nextArgInt;
        m_hoverArgStr = nextArgStr;
        if (m_hoverId != ControlId::None) {
            m_hoverAnims.try_emplace(MakeAnimKey(static_cast<uint32_t>(m_hoverId), m_hoverArgInt, m_hoverArgStr), 0.0f);
        }
        KickUiAnimation();
        InvalidateView();
    }
}

void AppWindow::OnMouseLeave() {
    m_mouseTracking = false;
    if (m_hoverId != ControlId::None) {
        m_hoverId = ControlId::None;
        m_hoverArgInt = 0;
        m_hoverArgStr.clear();
        KickUiAnimation();
        InvalidateView();
    }
}

void AppWindow::OnLButtonDown(int px, int py) {
    SetFocus(m_hwnd);
    const D2D1_POINT_2F dip = PtToDip(px, py);
    const HitControl* hit = HitTestDip(dip);

    if (m_activeEditField != EditFieldId::None) {
        const bool sameEdit = hit &&
            ((m_activeEditField == EditFieldId::OverlayText && hit->id == ControlId::InputOverlayText) ||
             (m_activeEditField == EditFieldId::OutWidth    && hit->id == ControlId::InputOutWidth) ||
             (m_activeEditField == EditFieldId::OutHeight   && hit->id == ControlId::InputOutHeight) ||
             (m_activeEditField == EditFieldId::CropX       && hit->id == ControlId::InputCropX) ||
             (m_activeEditField == EditFieldId::CropY       && hit->id == ControlId::InputCropY) ||
             (m_activeEditField == EditFieldId::CropW       && hit->id == ControlId::InputCropW) ||
             (m_activeEditField == EditFieldId::CropH       && hit->id == ControlId::InputCropH) ||
             (m_activeEditField == EditFieldId::SrcX        && hit->id == ControlId::InputSrcX) ||
             (m_activeEditField == EditFieldId::SrcY        && hit->id == ControlId::InputSrcY) ||
             (m_activeEditField == EditFieldId::SrcW        && hit->id == ControlId::InputSrcW) ||
             (m_activeEditField == EditFieldId::SrcH        && hit->id == ControlId::InputSrcH));
        if (!sameEdit) CommitEditField(true);
    }

    if (!hit) {
        if (m_showingSettings && dip.y >= 43.0f && dip.y <= 86.0f && dip.x > 60.0f) {
            ReleaseCapture();
            SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return;
    }

    m_pressId = hit->id;
    m_pressArgInt = hit->argInt;
    m_pressArgStr = hit->argStr;
    KickUiAnimation();

    if (hit->isSlider) {
        SetCapture(m_hwnd);
        m_draggingSliderId = hit->id;
        ApplySliderFromDipX(*hit, dip.x);
        return;
    }

    if (hit->id == ControlId::CanvasOverlayDrag || hit->id == ControlId::CanvasOverlayResize) {
        const uint32_t nextOv = static_cast<uint32_t>(hit->argInt);
        if (m_selectedLayer != CanvasLayer::Overlay || m_selectedOverlayId != nextOv) m_inspAnim = 0.01f;
        m_selectedLayer = CanvasLayer::Overlay;
        m_selectedOverlayId = nextOv;
        if (OverlayItem* ov = FindSelectedOverlay()) {
            SetCapture(m_hwnd);
            m_canvasDrag = (hit->id == ControlId::CanvasOverlayDrag) ? CanvasDragMode::MoveOverlay : CanvasDragMode::ResizeOverlay;
            m_dragStartDip = dip;
            m_dragOrigXNorm = ov->xNorm;
            m_dragOrigYNorm = ov->yNorm;
            m_dragOrigWNorm = ov->boxWidthNorm;
            m_dragOrigHNorm = ov->boxHeightNorm;
            m_dragOrigFont  = ov->fontSize;
            InvalidateView();
        }
        return;
    }

    if (hit->id == ControlId::CanvasCamDrag || hit->id == ControlId::CanvasCamResize) {
        if (m_selectedLayer != CanvasLayer::Camera) m_inspAnim = 0.01f;
        m_selectedLayer = CanvasLayer::Camera;
        SetCapture(m_hwnd);
        m_canvasDrag = (hit->id == ControlId::CanvasCamDrag) ? CanvasDragMode::MoveCamera : CanvasDragMode::ResizeCamera;
        m_dragStartDip = dip;
        m_dragOrigCamX = m_config.camXNorm;
        m_dragOrigCamY = m_config.camYNorm;
        m_dragOrigCamW = m_config.camWidthNorm;
        InvalidateView();
        return;
    }

    if (hit->id == ControlId::CanvasSourceDrag || hit->id == ControlId::CanvasSourceResize) {
        if (m_selectedLayer != CanvasLayer::MainSource) m_inspAnim = 0.01f;
        m_selectedLayer = CanvasLayer::MainSource;
        SetCapture(m_hwnd);
        m_canvasDrag = (hit->id == ControlId::CanvasSourceDrag) ? CanvasDragMode::MoveSource : CanvasDragMode::ResizeSource;
        m_dragResizeCorner = hit->argInt;
        m_dragStartDip = dip;
        m_dragOrigSrcX = m_config.srcPosX;
        m_dragOrigSrcY = m_config.srcPosY;
        m_dragOrigSrcW = m_config.srcScaleW;
        m_dragOrigSrcH = m_config.srcScaleH;
        InvalidateView();
        return;
    }

    if (hit->id == ControlId::CanvasCropDrag || hit->id == ControlId::CanvasCropResize) {
        SetCapture(m_hwnd);
        m_canvasDrag = (hit->id == ControlId::CanvasCropDrag) ? CanvasDragMode::MoveCrop : CanvasDragMode::ResizeCrop;
        m_dragStartDip = dip;
        m_dragOrigCropX = m_config.cropX;
        m_dragOrigCropY = m_config.cropY;
        m_dragOrigCropW = m_config.cropW;
        m_dragOrigCropH = m_config.cropH;
        InvalidateView();
        return;
    }

    InvalidateView();
}

void AppWindow::OnLButtonUp(int px, int py) {
    const D2D1_POINT_2F dip = PtToDip(px, py);

    if (m_canvasDrag != CanvasDragMode::None) {
        ReleaseCapture();
        m_canvasDrag = CanvasDragMode::None;
        m_pressId = ControlId::None;
        SaveAndApplyConfig();
        return;
    }

    if (m_draggingSliderId != ControlId::None) {
        ReleaseCapture();
        m_draggingSliderId = ControlId::None;
        m_pressId = ControlId::None;
        SaveAndApplyConfig();
        return;
    }

    const ControlId pressedId = m_pressId;
    const int pressedArgInt = m_pressArgInt;
    const std::wstring pressedArgStr = m_pressArgStr;
    m_pressId = ControlId::None;
    m_pressArgInt = 0;
    m_pressArgStr.clear();

    const HitControl* hit = HitTestDip(dip);
    if (hit && pressedId == hit->id && pressedArgInt == hit->argInt && pressedArgStr == hit->argStr) {
        ActivateControl(*hit);
    }
    KickUiAnimation();
    InvalidateView();
}

void AppWindow::OnMouseWheel(int px, int py, short delta) {
    const D2D1_POINT_2F dip = PtToDip(px, py);
    const float step = -static_cast<float>(delta) / 120.0f * 32.0f;

    if (m_openPopup != PopupKind::None && PointInRectF(m_popupRc, dip) && m_popupMaxScroll > 0.0f) {
        m_popupScroll = std::clamp(m_popupScroll + step, 0.0f, m_popupMaxScroll);
        InvalidateView();
        return;
    }
    if (!m_showingSettings && PointInRectF(m_overlayListRc, dip) && m_overlayListMaxScroll > 0.0f) {
        m_overlayListScroll = std::clamp(m_overlayListScroll + step, 0.0f, m_overlayListMaxScroll);
        InvalidateView();
    }
}

void AppWindow::OnKeyDown(WPARAM vk) {
    if (vk == VK_ESCAPE) {
        if (m_openPopup != PopupKind::None) TogglePopup(PopupKind::None);
        else if (m_activeEditField != EditFieldId::None) CommitEditField(false);
        return;
    }
    if (m_activeEditField != EditFieldId::None) {
        if (vk == VK_RETURN) {
            CommitEditField(true);
        } else if (vk == VK_BACK) {
            if (m_editSelectAll) {
                m_editBuffer.clear();
                m_editSelectAll = false;
            } else if (!m_editBuffer.empty()) {
                m_editBuffer.pop_back();
            }
            if (m_activeEditField == EditFieldId::OverlayText) {
                if (OverlayItem* sel = FindSelectedOverlay()) {
                    sel->text = m_editBuffer.empty() ? L" " : m_editBuffer;
                    m_overlays.SetOverlays(m_config.overlays);
                }
            }
            InvalidateView();
        } else if (vk == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            m_editSelectAll = true;
            InvalidateView();
        }
    }
}

void AppWindow::OnChar(wchar_t ch) {
    if (m_activeEditField == EditFieldId::None) return;
    if (ch < 32 || ch == 127) return;

    const bool numericOnly = (m_activeEditField != EditFieldId::OverlayText);
    if (numericOnly && (ch < L'0' || ch > L'9')) return;

    if (m_editSelectAll) {
        m_editBuffer.clear();
        m_editSelectAll = false;
    }
    const size_t maxLen = numericOnly ? 4u : 64u;
    if (m_editBuffer.size() < maxLen) {
        m_editBuffer.push_back(ch);
        if (m_activeEditField == EditFieldId::OverlayText) {
            if (OverlayItem* sel = FindSelectedOverlay()) {
                sel->text = m_editBuffer;
                m_overlays.SetOverlays(m_config.overlays);
            }
        }
        InvalidateView();
    }
}

void AppWindow::ApplySliderFromDipX(const HitControl& ctrl, float dipX) {
    const float tLeft = ctrl.rect.left + 7.0f;
    const float tRight = ctrl.rect.right - 7.0f;
    const float span = std::max(1.0f, tRight - tLeft);
    const double ratio = std::clamp(static_cast<double>(dipX - tLeft) / static_cast<double>(span), 0.0, 1.0);
    double raw = ctrl.sliderMin + ratio * (ctrl.sliderMax - ctrl.sliderMin);
    if (ctrl.sliderStep > 0.0) {
        raw = ctrl.sliderMin + std::round((raw - ctrl.sliderMin) / ctrl.sliderStep) * ctrl.sliderStep;
    }
    const int val = static_cast<int>(std::lround(std::clamp(raw, ctrl.sliderMin, ctrl.sliderMax)));

    if (ctrl.id == ControlId::DeskVolSlider) {
        m_config.desktopVolume = val;
        if (val > 0 && m_config.desktopAudioMuted) m_config.desktopAudioMuted = false;
        m_audio.Configure(m_config.desktopDeviceId,
                          static_cast<float>(m_config.desktopVolume) / 100.0f,
                          m_config.desktopAudioMuted,
                          m_config.micDeviceId,
                          static_cast<float>(m_config.micVolume) / 100.0f,
                          m_config.micMuted);
    } else if (ctrl.id == ControlId::MicVolSlider) {
        m_config.micVolume = val;
        if (val > 0 && m_config.micMuted) m_config.micMuted = false;
        m_audio.Configure(m_config.desktopDeviceId,
                          static_cast<float>(m_config.desktopVolume) / 100.0f,
                          m_config.desktopAudioMuted,
                          m_config.micDeviceId,
                          static_cast<float>(m_config.micVolume) / 100.0f,
                          m_config.micMuted);
    } else if (ctrl.id == ControlId::FilterSaturationSlider) {
        m_config.filterSaturation = static_cast<float>(std::clamp(raw, -1.0, 2.0));
        m_previewScaledSeq = 0;
        m_previewSeqRendered = 0;
    } else if (ctrl.id == ControlId::FilterGammaSlider) {
        m_config.filterGamma = static_cast<float>(std::clamp(raw, -1.0, 1.0));
        m_previewScaledSeq = 0;
        m_previewSeqRendered = 0;
    } else if (ctrl.id == ControlId::FilterContrastSlider) {
        m_config.filterContrast = static_cast<float>(std::clamp(raw, -1.0, 1.0));
        m_previewScaledSeq = 0;
        m_previewSeqRendered = 0;
    } else if (ctrl.id == ControlId::FilterBrightnessSlider) {
        m_config.filterBrightness = static_cast<float>(std::clamp(raw, -1.0, 1.0));
        m_previewScaledSeq = 0;
        m_previewSeqRendered = 0;
    }
    InvalidateView();
}

void AppWindow::ActivateControl(const HitControl& ctrl) {
    const bool isRec = m_recorder.GetStats().isRecording;

    switch (ctrl.id) {
    case ControlId::ModalBackdrop:     TogglePopup(PopupKind::None); break;
    case ControlId::WinMinimize:
        if (m_config.minimizeToTray) HideToTray(); else MinimizeWindow();
        break;
    case ControlId::WinMaximize:       ToggleMaximizeRestore(); break;
    case ControlId::WinClose:          PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;

    case ControlId::AddSourceBtn:
        RefreshAvailableLists();
        TogglePopup(m_openPopup == PopupKind::AddSourceMenu ? PopupKind::None : PopupKind::AddSourceMenu,
                    ctrl.rect, ctrl.rect.top > (m_clientH * 0.55f));
        break;
    case ControlId::AddSourceItem:
        m_config.hasCaptureSource = true;
        m_config.sourceVisible = true;
        if (ctrl.argInt == -2) {
            m_config.sourceType = 2;
        } else if (ctrl.argInt == -1) {
            m_config.sourceType = 1;
            m_config.windowTitleFilter = ctrl.argStr;
        } else {
            m_config.sourceType = 0;
            m_config.monitorIndex = ctrl.argInt;
        }
        if (m_selectedLayer != CanvasLayer::MainSource) m_inspAnim = 0.01f;
        m_selectedLayer = CanvasLayer::MainSource;
        TogglePopup(PopupKind::None);
        SaveAndApplyConfig();
        SetActionStatus(m_config.sourceType == 1 ? (L"Added Window: " + m_config.windowTitleFilter)
                                               : L"Added Screen Capture Source", false);
        KickUiAnimation();
        break;
    case ControlId::ToggleSourceVisBtn:
        m_config.sourceVisible = !m_config.sourceVisible;
        SaveAndApplyConfig();
        KickUiAnimation();
        break;
    case ControlId::RemoveSourceBtn:
        m_config.hasCaptureSource = false;
        m_config.sourceVisible = true;
        if (m_selectedLayer == CanvasLayer::MainSource) {
            m_inspAnim = 0.01f;
            if (m_config.camAdded) m_selectedLayer = CanvasLayer::Camera;
            else if (!m_config.overlays.empty()) {
                m_selectedLayer = CanvasLayer::Overlay;
                m_selectedOverlayId = m_config.overlays.front().id;
            } else {
                m_selectedLayer = CanvasLayer::None;
            }
        }
        SaveAndApplyConfig();
        SetActionStatus(L"Capture source removed from scene", false);
        KickUiAnimation();
        break;
    case ControlId::AddComponentBtn:
        m_cameras = CameraEngine::EnumerateCameras();
        TogglePopup(m_openPopup == PopupKind::AddComponentMenu ? PopupKind::None : PopupKind::AddComponentMenu,
                    ctrl.rect, ctrl.rect.top > (m_clientH * 0.55f));
        break;
    case ControlId::AddComponentItem:
        TogglePopup(PopupKind::None);
        if (ctrl.argInt == 0) {
            m_cameras = CameraEngine::EnumerateCameras();
            m_config.camAdded = true;
            m_config.camEnabled = true;
            if (m_selectedLayer != CanvasLayer::Camera) m_inspAnim = 0.01f;
            m_selectedLayer = CanvasLayer::Camera;
            SaveAndApplyConfig();
            SetActionStatus(m_cameras.empty() ? L"On-Cam component added (waiting for camera)"
                                              : L"On-Cam added: " + m_cameras[0].name, false);
        } else if (ctrl.argInt == 1) {
            AddOverlay(OverlayKind::Text);
            m_inspAnim = 0.01f;
        } else if (ctrl.argInt == 2) {
            AddOverlay(OverlayKind::Clock);
            m_inspAnim = 0.01f;
        } else if (ctrl.argInt == 3) {
            AddOverlay(OverlayKind::Image);
            m_inspAnim = 0.01f;
        } else if (ctrl.argInt == 4) {
            AddOverlay(OverlayKind::Box);
            m_inspAnim = 0.01f;
        }
        KickUiAnimation();
        break;
    case ControlId::RemoveCamBtn:
        m_config.camAdded = false;
        m_config.camEnabled = false;
        if (m_selectedLayer == CanvasLayer::Camera) {
            m_inspAnim = 0.01f;
            if (m_config.hasCaptureSource) m_selectedLayer = CanvasLayer::MainSource;
            else if (!m_config.overlays.empty()) {
                m_selectedLayer = CanvasLayer::Overlay;
                m_selectedOverlayId = m_config.overlays.front().id;
            } else {
                m_selectedLayer = CanvasLayer::None;
            }
        }
        SaveAndApplyConfig();
        SetActionStatus(L"On-Cam removed from scene", false);
        KickUiAnimation();
        break;

    case ControlId::SrcTypeDisplay:
        m_config.hasCaptureSource = true; m_config.sourceType = 0; m_selectedLayer = CanvasLayer::MainSource;
        TogglePopup(PopupKind::None); RefreshAvailableLists(); SaveAndApplyConfig();
        break;
    case ControlId::SrcTypeWindow:
        m_config.hasCaptureSource = true; m_config.sourceType = 1; m_selectedLayer = CanvasLayer::MainSource;
        TogglePopup(PopupKind::None); RefreshAvailableLists(); SaveAndApplyConfig();
        if (m_config.windowTitleFilter.empty()) {
            SetActionStatus(L"Select an application window to capture", false);
        }
        break;
    case ControlId::SrcTypeRegion:
        m_config.hasCaptureSource = true; m_config.sourceType = 2; m_selectedLayer = CanvasLayer::MainSource;
        TogglePopup(PopupKind::None); RefreshAvailableLists(); SaveAndApplyConfig();
        break;
    case ControlId::SourcePickerBtn:
    case ControlId::CanvasPickWindowBtn:
        RefreshAvailableLists();
        TogglePopup(m_openPopup == PopupKind::SourcePicker ? PopupKind::None : PopupKind::SourcePicker,
                    ctrl.rect, ctrl.rect.top > (m_clientH * 0.55f));
        break;
    case ControlId::SourcePickerItem:
        if (m_config.sourceType == 0) m_config.monitorIndex = ctrl.argInt;
        else m_config.windowTitleFilter = ctrl.argStr;
        m_selectedLayer = CanvasLayer::MainSource;
        TogglePopup(PopupKind::None);
        SaveAndApplyConfig();
        break;
    case ControlId::RefreshWindowsBtn:
        RefreshAvailableLists(); SetActionStatus(L"Window list refreshed", false);
        m_spinningRefreshId = ctrl.id; m_refreshSpinAngle = 0.01f; KickUiAnimation();
        break;
    case ControlId::ToggleShowCursor:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.showCursor ? 1.0f : 0.0f);
        m_config.showCursor = !m_config.showCursor; SaveAndApplyConfig(); KickUiAnimation();
        break;
    case ControlId::SrcTabGeneral:
        if (m_srcTab != 0) {
            m_srcTab = 0;
            m_srcTabAnim = 0.0f;
            KickUiAnimation();
            InvalidateView();
        }
        break;
    case ControlId::SrcTabFilter:
        if (m_srcTab != 1) {
            m_srcTab = 1;
            m_srcTabAnim = 0.0f;
            KickUiAnimation();
            InvalidateView();
        }
        break;
    case ControlId::ToggleColorFilter:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.colorFilterEnabled ? 1.0f : 0.0f);
        m_config.colorFilterEnabled = !m_config.colorFilterEnabled;
        m_previewScaledSeq = 0;
        m_previewSeqRendered = 0;
        SaveAndApplyConfig();
        KickUiAnimation();
        InvalidateView();
        break;
    case ControlId::FilterResetBtn:
        m_config.filterGamma = 0.0f;
        m_config.filterContrast = 0.0f;
        m_config.filterBrightness = 0.0f;
        m_config.filterSaturation = 0.0f;
        m_previewScaledSeq = 0;
        m_previewSeqRendered = 0;
        SaveAndApplyConfig();
        KickUiAnimation();
        InvalidateView();
        break;
    case ControlId::InputCropX: BeginEditField(EditFieldId::CropX, std::to_wstring(m_config.cropX)); break;
    case ControlId::InputCropY: BeginEditField(EditFieldId::CropY, std::to_wstring(m_config.cropY)); break;
    case ControlId::InputCropW: BeginEditField(EditFieldId::CropW, std::to_wstring(m_config.cropW)); break;
    case ControlId::InputCropH: BeginEditField(EditFieldId::CropH, std::to_wstring(m_config.cropH)); break;
    case ControlId::CropPreset1080:
        m_config.cropX = 0; m_config.cropY = 0; m_config.cropW = 1920; m_config.cropH = 1080; SaveAndApplyConfig(); break;
    case ControlId::CropPreset720:
        m_config.cropX = 0; m_config.cropY = 0; m_config.cropW = 1280; m_config.cropH = 720; SaveAndApplyConfig(); break;
    case ControlId::CropPresetVert:
        m_config.cropX = 320; m_config.cropY = 0; m_config.cropW = 608; m_config.cropH = 1080; SaveAndApplyConfig(); break;
    case ControlId::CropPresetSq:
        m_config.cropX = 200; m_config.cropY = 100; m_config.cropW = 720; m_config.cropH = 720; SaveAndApplyConfig(); break;

    case ControlId::LayerSelectSource:
        if (m_selectedLayer != CanvasLayer::MainSource) {
            m_selectedLayer = CanvasLayer::MainSource;
            m_inspAnim = 0.01f;
            KickUiAnimation();
        }
        break;
    case ControlId::SrcFitBtn:
    case ControlId::SrcResetBtn:
        m_config.srcPosX = 0.0f; m_config.srcPosY = 0.0f;
        m_config.srcScaleW = 1.0f; m_config.srcScaleH = 1.0f;
        m_config.srcKeepAspect = true;
        SaveAndApplyConfig();
        SetActionStatus(ctrl.id == ControlId::SrcResetBtn ? L"Source layout reset" : L"Source fitted to canvas", false);
        break;
    case ControlId::SrcFillBtn:
        m_config.srcPosX = 0.0f; m_config.srcPosY = 0.0f;
        m_config.srcScaleW = 1.0f; m_config.srcScaleH = 1.0f;
        m_config.srcKeepAspect = false;
        SaveAndApplyConfig();
        SetActionStatus(L"Source stretched to fill canvas", false);
        break;
    case ControlId::SrcCenterBtn:
        m_config.srcPosX = std::max(0.0f, (1.0f - m_config.srcScaleW) * 0.5f);
        m_config.srcPosY = std::max(0.0f, (1.0f - m_config.srcScaleH) * 0.5f);
        SaveAndApplyConfig();
        break;
    case ControlId::SrcPipBtn:
        m_config.srcScaleW = 0.76f; m_config.srcScaleH = 0.76f;
        m_config.srcPosX = 0.03f; m_config.srcPosY = 0.12f;
        m_config.srcKeepAspect = true;
        SaveAndApplyConfig();
        break;
    case ControlId::SrcKeepAspectToggle:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.srcKeepAspect ? 1.0f : 0.0f);
        m_config.srcKeepAspect = !m_config.srcKeepAspect; SaveAndApplyConfig(); KickUiAnimation(); break;
    case ControlId::InputSrcX: BeginEditField(EditFieldId::SrcX, std::to_wstring(static_cast<int>(std::lround(m_config.srcPosX * 100.0f)))); break;
    case ControlId::InputSrcY: BeginEditField(EditFieldId::SrcY, std::to_wstring(static_cast<int>(std::lround(m_config.srcPosY * 100.0f)))); break;
    case ControlId::InputSrcW: BeginEditField(EditFieldId::SrcW, std::to_wstring(static_cast<int>(std::lround(m_config.srcScaleW * 100.0f)))); break;
    case ControlId::InputSrcH: BeginEditField(EditFieldId::SrcH, std::to_wstring(static_cast<int>(std::lround(m_config.srcScaleH * 100.0f)))); break;

    case ControlId::LayerSelectCamera:
        if (m_selectedLayer != CanvasLayer::Camera) {
            m_selectedLayer = CanvasLayer::Camera;
            m_inspAnim = 0.01f;
            KickUiAnimation();
        }
        break;
    case ControlId::ToggleCamBtn:
        m_cameras = CameraEngine::EnumerateCameras();
        m_config.camAdded = true;
        m_config.camEnabled = !m_config.camEnabled;
        if (m_config.camEnabled) {
            if (m_selectedLayer != CanvasLayer::Camera) m_inspAnim = 0.01f;
            m_selectedLayer = CanvasLayer::Camera;
        }
        SaveAndApplyConfig();
        SetActionStatus(m_config.camEnabled
                            ? (m_cameras.empty() ? L"On-Cam enabled (waiting for camera device)" : L"On-Cam live: " + m_cameras[0].name)
                            : L"Camera hidden", false);
        KickUiAnimation();
        break;
    case ControlId::CamPickerBtn:
        m_cameras = CameraEngine::EnumerateCameras();
        TogglePopup(m_openPopup == PopupKind::CameraPicker ? PopupKind::None : PopupKind::CameraPicker,
                    ctrl.rect, ctrl.rect.top > (m_clientH * 0.55f));
        break;
    case ControlId::CamPickerItem:
        m_config.camDeviceId = ctrl.argStr;
        m_config.camEnabled = true;
        if (m_selectedLayer != CanvasLayer::Camera) m_inspAnim = 0.01f;
        m_selectedLayer = CanvasLayer::Camera;
        TogglePopup(PopupKind::None);
        SaveAndApplyConfig();
        KickUiAnimation();
        break;
    case ControlId::RefreshCamerasBtn:
        m_cameras = CameraEngine::EnumerateCameras();
        SetActionStatus(L"Detected " + std::to_wstring(m_cameras.size()) + L" Camera(s)", false);
        m_spinningRefreshId = ctrl.id; m_refreshSpinAngle = 0.01f; KickUiAnimation();
        break;
    case ControlId::CamPosTL: m_config.camXNorm = 0.03f; m_config.camYNorm = 0.04f; SaveAndApplyConfig(); break;
    case ControlId::CamPosTR: m_config.camXNorm = std::max(0.03f, 0.97f - m_config.camWidthNorm); m_config.camYNorm = 0.04f; SaveAndApplyConfig(); break;
    case ControlId::CamPosBL: m_config.camXNorm = 0.03f; m_config.camYNorm = 0.68f; SaveAndApplyConfig(); break;
    case ControlId::CamPosBR: m_config.camXNorm = std::max(0.03f, 0.97f - m_config.camWidthNorm); m_config.camYNorm = 0.68f; SaveAndApplyConfig(); break;
    case ControlId::CamShapeRect:   m_config.camShape = 0; SaveAndApplyConfig(); break;
    case ControlId::CamShapeRound:  m_config.camShape = 1; SaveAndApplyConfig(); break;
    case ControlId::CamShapeCircle: m_config.camShape = 2; SaveAndApplyConfig(); break;
    case ControlId::CamSizeMinusBtn: m_config.camWidthNorm = std::clamp(m_config.camWidthNorm - 0.03f, 0.08f, 0.70f); SaveAndApplyConfig(); break;
    case ControlId::CamSizePlusBtn:  m_config.camWidthNorm = std::clamp(m_config.camWidthNorm + 0.03f, 0.08f, 0.70f); SaveAndApplyConfig(); break;
    case ControlId::CamMirrorToggle:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.camMirror ? 1.0f : 0.0f);
        m_config.camMirror = !m_config.camMirror; SaveAndApplyConfig(); KickUiAnimation(); break;
    case ControlId::CamBorderToggle:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.camBorder ? 1.0f : 0.0f);
        m_config.camBorder = !m_config.camBorder; SaveAndApplyConfig(); KickUiAnimation(); break;

    case ControlId::AddOverlayTextBtn:  AddOverlay(OverlayKind::Text); break;
    case ControlId::AddOverlayClockBtn: AddOverlay(OverlayKind::Clock); break;
    case ControlId::AddOverlayBoxBtn:   AddOverlay(OverlayKind::Box); break;
    case ControlId::AddOverlayImageBtn: AddOverlay(OverlayKind::Image); break;
    case ControlId::OverlayListSelect: {
        const uint32_t nextOvId = static_cast<uint32_t>(ctrl.argInt);
        if (m_selectedLayer != CanvasLayer::Overlay || m_selectedOverlayId != nextOvId) {
            m_selectedOverlayId = nextOvId;
            m_selectedLayer = CanvasLayer::Overlay;
            m_inspAnim = 0.01f;
            KickUiAnimation();
        }
        break;
    }
    case ControlId::OverlayListToggleVis:
        for (auto& ov : m_config.overlays) {
            if (ov.id == static_cast<uint32_t>(ctrl.argInt)) { ov.visible = !ov.visible; break; }
        }
        SaveAndApplyConfig();
        break;
    case ControlId::OverlayListDelete:
        DeleteOverlay(static_cast<uint32_t>(ctrl.argInt));
        break;
    case ControlId::InputOverlayText:
        if (OverlayItem* sel = FindSelectedOverlay()) BeginEditField(EditFieldId::OverlayText, sel->text);
        break;
    case ControlId::OverlayBrowseImgBtn:
        BrowseOverlayImage();
        break;
    case ControlId::OverlaySizeMinusBtn:
        if (OverlayItem* sel = FindSelectedOverlay()) {
            if (sel->kind == OverlayKind::Text || sel->kind == OverlayKind::Clock) {
                sel->fontSize = std::clamp(sel->fontSize - 4, 12, 96);
            } else {
                sel->boxWidthNorm = std::clamp(sel->boxWidthNorm - 0.04f, 0.05f, 0.95f);
                sel->boxHeightNorm = std::clamp(sel->boxHeightNorm - 0.03f, 0.04f, 0.95f);
            }
            SaveAndApplyConfig();
        }
        break;
    case ControlId::OverlaySizePlusBtn:
        if (OverlayItem* sel = FindSelectedOverlay()) {
            if (sel->kind == OverlayKind::Text || sel->kind == OverlayKind::Clock) {
                sel->fontSize = std::clamp(sel->fontSize + 4, 12, 96);
            } else {
                sel->boxWidthNorm = std::clamp(sel->boxWidthNorm + 0.04f, 0.05f, 0.95f);
                sel->boxHeightNorm = std::clamp(sel->boxHeightNorm + 0.03f, 0.04f, 0.95f);
            }
            SaveAndApplyConfig();
        }
        break;
    case ControlId::OverlayColorBtn:
        if (OverlayItem* sel = FindSelectedOverlay()) {
            sel->colorArgb = kOverlayColors[static_cast<size_t>(std::clamp(ctrl.argInt, 0, 4))];
            SaveAndApplyConfig();
        }
        break;
    case ControlId::OverlayBgToggleBtn:
        if (OverlayItem* sel = FindSelectedOverlay()) {
            m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), sel->bgEnabled ? 1.0f : 0.0f);
            sel->bgEnabled = !sel->bgEnabled;
            SaveAndApplyConfig();
            KickUiAnimation();
        }
        break;

    case ControlId::DeskMuteBtn: m_config.desktopAudioMuted = !m_config.desktopAudioMuted; SaveAndApplyConfig(); break;
    case ControlId::MicMuteBtn:  m_config.micMuted = !m_config.micMuted; SaveAndApplyConfig(); break;

    case ControlId::EncoderPickerBtn:
    case ControlId::SettingsEncPickerBtn:
        if (!isRec) {
            m_encoders = EncoderManager::EnumerateAvailableEncoders();
            TogglePopup(m_openPopup == PopupKind::EncoderPicker ? PopupKind::None : PopupKind::EncoderPicker,
                        ctrl.rect, ctrl.rect.top > (m_clientH * 0.5f));
        }
        break;
    case ControlId::EncoderPickerItem:
        if (!isRec) {
            m_config.encoderId = ctrl.argStr;
            const DetectedEncoder nextEnc = EncoderManager::ResolveSelection(m_encoders, m_config.encoderId);
            m_config.encoderPreset = EncoderManager::ResolvePreset(nextEnc, m_config.encoderPreset).id;
            TogglePopup(PopupKind::None);
            SaveAndApplyConfig();
        }
        break;
    case ControlId::PresetPickerBtn:
        if (!isRec) {
            TogglePopup(m_openPopup == PopupKind::EncoderPresetPicker ? PopupKind::None : PopupKind::EncoderPresetPicker,
                        ctrl.rect, ctrl.rect.top > (m_clientH * 0.5f));
        }
        break;
    case ControlId::PresetPickerItem:
        if (!isRec) {
            m_config.encoderPreset = ctrl.argStr;
            TogglePopup(PopupKind::None);
            SaveAndApplyConfig();
        }
        break;
    case ControlId::RefreshEncodersBtn:
        m_encoders = EncoderManager::EnumerateAvailableEncoders();
        SetActionStatus(L"Detected " + std::to_wstring(m_encoders.size() > 1 ? m_encoders.size() - 1 : 0) + L" Encoders", false);
        m_spinningRefreshId = ctrl.id; m_refreshSpinAngle = 0.01f; KickUiAnimation();
        break;
    case ControlId::ResPickerBtn:
        if (!isRec) {
            TogglePopup(m_openPopup == PopupKind::ResPicker ? PopupKind::None : PopupKind::ResPicker,
                        ctrl.rect, ctrl.rect.top > (m_clientH * 0.5f));
        }
        break;
    case ControlId::ResPickerItem:
        if (!isRec) {
            m_config.resolution = ctrl.argStr;
            uint32_t w = 1920, h = 1080;
            m_recorder.ResolveOutputDimensions(m_config, w, h);
            m_config.customWidth = static_cast<int>(w);
            m_config.customHeight = static_cast<int>(h);
            TogglePopup(PopupKind::None);
            SaveAndApplyConfig();
        }
        break;
    case ControlId::InputOutWidth:
        if (!isRec) {
            uint32_t w = 1920, h = 1080;
            m_recorder.ResolveOutputDimensions(m_config, w, h);
            BeginEditField(EditFieldId::OutWidth, std::to_wstring(w));
        }
        break;
    case ControlId::InputOutHeight:
        if (!isRec) {
            uint32_t w = 1920, h = 1080;
            m_recorder.ResolveOutputDimensions(m_config, w, h);
            BeginEditField(EditFieldId::OutHeight, std::to_wstring(h));
        }
        break;
    case ControlId::CycleFpsBtn:
        if (!isRec) {
            m_config.fps = (m_config.fps == 30) ? 60 : ((m_config.fps == 60) ? 120 : 30);
            SaveAndApplyConfig();
        }
        break;

    case ControlId::RecordToggleBtn: ToggleStartStopRecording(); break;
    case ControlId::PauseToggleBtn:  TogglePauseRecording(); break;
    case ControlId::OpenFolderBtn:
    case ControlId::OpenOutputSettingsBtn: OpenRecordingsFolder(); break;

    case ControlId::SettingsBtn:      SwitchToSettings(true); break;
    case ControlId::SettingsBackBtn:  SwitchToSettings(false); break;
    case ControlId::MinimizeTrayBtn:  HideToTray(); break;
    case ControlId::QuickThemeWhiteBtn:
    case ControlId::ThemeWhiteBtn:    m_config.theme = L"white"; SaveAndApplyConfig(); break;
    case ControlId::QuickThemeBlackBtn:
    case ControlId::ThemeBlackBtn:    m_config.theme = L"black"; SaveAndApplyConfig(); break;

    case ControlId::ToggleAlwaysOnTop:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.alwaysOnTop ? 1.0f : 0.0f);
        m_config.alwaysOnTop = !m_config.alwaysOnTop; SaveAndApplyConfig(); KickUiAnimation(); break;
    case ControlId::ToggleMinToTray:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.minimizeToTray ? 1.0f : 0.0f);
        m_config.minimizeToTray = !m_config.minimizeToTray; SaveAndApplyConfig(); KickUiAnimation(); break;
    case ControlId::ToggleCloseToTray:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.closeToTray ? 1.0f : 0.0f);
        m_config.closeToTray = !m_config.closeToTray; SaveAndApplyConfig(); KickUiAnimation(); break;
    case ControlId::ToggleNotifications:
        m_toggleAnims.try_emplace(static_cast<int>(ctrl.id), m_config.showNotifications ? 1.0f : 0.0f);
        m_config.showNotifications = !m_config.showNotifications; SaveAndApplyConfig(); KickUiAnimation(); break;

    case ControlId::ResNativeBtn: if (!isRec) { m_config.resolution = L"native"; SaveAndApplyConfig(); } break;
    case ControlId::Res1440Btn:   if (!isRec) { m_config.resolution = L"1440p";  SaveAndApplyConfig(); } break;
    case ControlId::Res1080Btn:   if (!isRec) { m_config.resolution = L"1080p";  SaveAndApplyConfig(); } break;
    case ControlId::Res720Btn:    if (!isRec) { m_config.resolution = L"720p";   SaveAndApplyConfig(); } break;
    case ControlId::ResVertBtn:   if (!isRec) { m_config.resolution = L"vertical"; SaveAndApplyConfig(); } break;
    case ControlId::ResCustomBtn: if (!isRec) { m_config.resolution = L"custom"; SaveAndApplyConfig(); } break;
    case ControlId::Fps30Btn:     if (!isRec) { m_config.fps = 30;  SaveAndApplyConfig(); } break;
    case ControlId::Fps60Btn:     if (!isRec) { m_config.fps = 60;  SaveAndApplyConfig(); } break;
    case ControlId::Fps120Btn:    if (!isRec) { m_config.fps = 120; SaveAndApplyConfig(); } break;
    case ControlId::Bitrate8Btn:  if (!isRec) { m_config.bitrateMbps = 8;  SaveAndApplyConfig(); } break;
    case ControlId::Bitrate18Btn: if (!isRec) { m_config.bitrateMbps = 18; SaveAndApplyConfig(); } break;
    case ControlId::Bitrate28Btn: if (!isRec) { m_config.bitrateMbps = 28; SaveAndApplyConfig(); } break;
    case ControlId::Bitrate45Btn: if (!isRec) { m_config.bitrateMbps = 45; SaveAndApplyConfig(); } break;
    case ControlId::HotkeyF8Btn:  m_config.recordHotkey = L"F8";   SaveAndApplyConfig(); break;
    case ControlId::HotkeyF9Btn:  m_config.recordHotkey = L"F9";   SaveAndApplyConfig(); break;
    case ControlId::HotkeyF10Btn: m_config.recordHotkey = L"F10";  SaveAndApplyConfig(); break;
    case ControlId::HotkeyNoneBtn:m_config.recordHotkey = L"None"; SaveAndApplyConfig(); break;
    case ControlId::BrowseOutputBtn: BrowseOutputFolder(); break;
    case ControlId::DeskDeviceBtn:
        RefreshAvailableLists();
        TogglePopup(m_openPopup == PopupKind::DesktopAudioPicker ? PopupKind::None : PopupKind::DesktopAudioPicker,
                    ctrl.rect, ctrl.rect.top > (m_clientH * 0.5f));
        break;
    case ControlId::MicDeviceBtn:
        RefreshAvailableLists();
        TogglePopup(m_openPopup == PopupKind::MicAudioPicker ? PopupKind::None : PopupKind::MicAudioPicker,
                    ctrl.rect, ctrl.rect.top > (m_clientH * 0.5f));
        break;
    case ControlId::AudioDevItem:
        if (m_openPopup == PopupKind::DesktopAudioPicker) m_config.desktopDeviceId = ctrl.argStr;
        else if (m_openPopup == PopupKind::MicAudioPicker) m_config.micDeviceId = ctrl.argStr;
        TogglePopup(PopupKind::None);
        SaveAndApplyConfig();
        break;
    case ControlId::Audio192Btn: if (!isRec) { m_config.audioBitrateKbps = 192; SaveAndApplyConfig(); } break;
    case ControlId::Audio320Btn: if (!isRec) { m_config.audioBitrateKbps = 320; SaveAndApplyConfig(); } break;
    case ControlId::SupportDevBtn:
        ShellExecuteW(nullptr, L"open", L"https://sociabuzz.com/idnvinnmalay", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    default: break;
    }
}

void AppWindow::KickUiAnimation() {
    if (!m_hwnd) return;
    if (m_animLastQpc == 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        m_animLastQpc = now.QuadPart;
    }
    SetTimer(m_hwnd, IDT_UI_ANIM, kUiAnimMs, nullptr);
}

void AppWindow::StepUiAnimations(float dtSec) {
    const float dt = std::clamp(dtSec, 0.001f, 0.05f);
    auto stepToward = [dt](float& cur, float target, float speed, float eps = 0.003f) -> bool {
        const float diff = target - cur;
        if (std::fabs(diff) <= eps) {
            cur = target;
            return false;
        }
        cur += diff * (1.0f - std::exp(-speed * dt));
        return true;
    };

    bool anyActive = false;
    anyActive |= stepToward(m_introAnim, 1.0f, 11.5f);
    anyActive |= stepToward(m_settingsAnim, m_showingSettings ? 1.0f : 0.0f, 13.5f);
    anyActive |= stepToward(m_inspAnim, 1.0f, 15.5f);
    anyActive |= stepToward(m_srcTabAnim, 1.0f, 18.0f);
    anyActive |= stepToward(m_popupAnim, (m_openPopup != PopupKind::None) ? 1.0f : 0.0f, 20.0f);
    if (m_openPopup == PopupKind::None && m_popupAnim <= 0.004f) {
        m_lastPopup = PopupKind::None;
    }

    auto stepMeterPhysics = [dt](float& cur, float target, float& peak, float& holdTimer) -> bool {
        bool active = false;
        if (target > cur) {
            cur += (target - cur) * (1.0f - std::exp(-28.0f * dt));
            active = true;
        } else {
            cur += (target - cur) * (1.0f - std::exp(-12.0f * dt));
            if (cur > 0.004f) active = true;
        }
        if (cur > peak) {
            peak = cur;
            holdTimer = 0.45f;
            active = true;
        } else if (holdTimer > 0.0f) {
            holdTimer -= dt;
            active = true;
        } else if (peak > 0.005f) {
            peak += (0.0f - peak) * (1.0f - std::exp(-7.5f * dt));
            active = true;
        }
        return active;
    };
    anyActive |= stepMeterPhysics(m_uiDeskMeter, m_targetDeskMeter, m_uiDeskPeak, m_uiDeskPeakHoldSec);
    anyActive |= stepMeterPhysics(m_uiMicMeter, m_targetMicMeter, m_uiMicPeak, m_uiMicPeakHoldSec);

    if (m_statusFadingOut) {
        const bool moving = stepToward(m_statusAnim, 0.0f, 11.0f);
        if (!moving) {
            m_statusFadingOut = false;
            m_actionStatusText.clear();
        } else {
            anyActive = true;
        }
    } else {
        anyActive |= stepToward(m_statusAnim, m_actionStatusText.empty() ? 0.0f : 1.0f, 15.0f);
    }

    const float gearTarget = (!m_showingSettings && IsControlHovered(ControlId::SettingsBtn)) ? 45.0f : 0.0f;
    anyActive |= stepToward(m_gearAngle, gearTarget, 14.0f, 0.35f);

    if (m_refreshSpinAngle > 0.0f) {
        m_refreshSpinAngle += dt * 760.0f;
        if (m_refreshSpinAngle >= 360.0f) {
            m_refreshSpinAngle = 0.0f;
            m_spinningRefreshId = ControlId::None;
        } else {
            anyActive = true;
        }
    }

    const RecorderStats st = m_recorder.GetStats();
    m_recPulsePhase = std::fmod(m_recPulsePhase + dt * (st.isRecording ? 5.5f : 2.5f), 6.2831853f);
    if (st.isRecording && !st.isPaused) anyActive = true;

    const uint64_t activeHoverKey = (m_hoverId != ControlId::None)
        ? MakeAnimKey(static_cast<uint32_t>(m_hoverId), m_hoverArgInt, m_hoverArgStr) : 0ULL;
    if (activeHoverKey != 0ULL) {
        m_hoverAnims.try_emplace(activeHoverKey, 0.0f);
    }
    for (auto it = m_hoverAnims.begin(); it != m_hoverAnims.end();) {
        const float tgt = (it->first == activeHoverKey && activeHoverKey != 0ULL) ? 1.0f : 0.0f;
        const bool moving = stepToward(it->second, tgt, 16.5f);
        if (!moving && tgt == 0.0f) {
            it = m_hoverAnims.erase(it);
        } else {
            anyActive |= moving;
            ++it;
        }
    }

    const OverlayItem* selOv = FindSelectedOverlay();
    auto toggleTargetFor = [&](int cid) -> std::pair<bool, bool> {
        switch (static_cast<ControlId>(cid)) {
        case ControlId::ToggleShowCursor:    return {true, m_config.showCursor};
        case ControlId::ToggleColorFilter:   return {true, m_config.colorFilterEnabled};
        case ControlId::SrcKeepAspectToggle: return {true, m_config.srcKeepAspect};
        case ControlId::CamMirrorToggle:     return {true, m_config.camMirror};
        case ControlId::CamBorderToggle:     return {true, m_config.camBorder};
        case ControlId::OverlayBgToggleBtn:  return {selOv != nullptr, selOv ? selOv->bgEnabled : false};
        case ControlId::ToggleAlwaysOnTop:   return {true, m_config.alwaysOnTop};
        case ControlId::ToggleMinToTray:     return {true, m_config.minimizeToTray};
        case ControlId::ToggleCloseToTray:   return {true, m_config.closeToTray};
        case ControlId::ToggleNotifications: return {true, m_config.showNotifications};
        default:                             return {false, false};
        }
    };
    for (auto it = m_toggleAnims.begin(); it != m_toggleAnims.end();) {
        const auto [valid, isOn] = toggleTargetFor(it->first);
        if (!valid) {
            it = m_toggleAnims.erase(it);
            continue;
        }
        anyActive |= stepToward(it->second, isOn ? 1.0f : 0.0f, 18.0f);
        ++it;
    }

    for (auto& kv : m_floatAnims) {
        FloatAnim& fa = kv.second;
        const float eps = (std::fabs(fa.target) > 5.0f || std::fabs(fa.cur) > 5.0f) ? 0.25f : 0.002f;
        anyActive |= stepToward(fa.cur, fa.target, fa.speed, eps);
    }

    if (!anyActive && m_hwnd) {
        KillTimer(m_hwnd, IDT_UI_ANIM);
        m_animLastQpc = 0;
    }
}

float AppWindow::GetHoverAnim(ControlId id, int argInt, const std::wstring& argStr) const {
    const uint64_t key = MakeAnimKey(static_cast<uint32_t>(id), argInt, argStr);
    auto it = m_hoverAnims.find(key);
    if (it != m_hoverAnims.end()) return it->second;
    return IsControlHovered(id, argInt, argStr) ? 1.0f : 0.0f;
}

float AppWindow::GetToggleAnim(ControlId id, bool isOn) const {
    auto it = m_toggleAnims.find(static_cast<int>(id));
    if (it != m_toggleAnims.end()) return it->second;
    return isOn ? 1.0f : 0.0f;
}

float AppWindow::AnimSlot(int slotKey, float target, float speed) const {
    auto [it, inserted] = m_floatAnims.try_emplace(slotKey, FloatAnim{target, target, speed});
    if (!inserted) {
        it->second.target = target;
        it->second.speed = speed;
        const float eps = (std::fabs(target) > 5.0f || std::fabs(it->second.cur) > 5.0f) ? 0.25f : 0.002f;
        if (std::fabs(it->second.cur - target) > eps) {
            m_animNeedTimer = true;
        }
    }
    return it->second.cur;
}

ID2D1Layer* AppWindow::EnsureAnimLayer() {
    if (!m_animLayer && m_rt) {
        m_rt->CreateLayer(m_animLayer.GetAddressOf());
    }
    return m_animLayer.Get();
}

void AppWindow::DrawAnimatedPickerBox(const ThemePalette& pal, ControlId id, const D2D1_RECT_F& rc,
                                      const std::wstring& text, PopupKind popupKind, bool enabled,
                                      bool warnAccent, float radius) {
    RegisterHit(id, rc, enabled);
    const bool isOpen = (m_openPopup == popupKind && popupKind != PopupKind::None);
    const float hAmt = enabled ? EaseOutCubic(GetHoverAnim(id)) : 0.0f;
    const float oAmt = enabled ? AnimSlot(42500 + static_cast<int>(id), isOpen ? 1.0f : 0.0f, 18.0f) : 0.0f;
    const float mix = std::max(hAmt, oAmt);

    const D2D1_COLOR_F idleBg = warnAccent ? pal.accentTint : pal.surface;
    const D2D1_COLOR_F idleBrd = warnAccent ? pal.accent : pal.borderStrong;
    DrawRoundedBox(rc, radius, ColorLerp(idleBg, pal.accentTint, mix),
                   ColorLerp(idleBrd, pal.accent, mix), 1.0f);
    DrawTextStrict(text, warnAccent ? m_fmtBold105.Get() : m_fmtSemi10.Get(),
                   {rc.left + 8.0f, rc.top, rc.right - 20.0f, rc.bottom},
                   !enabled ? pal.textMuted : (warnAccent ? pal.accent : ColorLerp(pal.text, pal.accent, oAmt * 0.55f)));

    const float chevDeg = AnimSlot(43000 + static_cast<int>(id), isOpen ? 180.0f : 0.0f, 18.0f);
    m_brush->SetColor(!enabled ? pal.textMuted : ColorLerp(pal.textMuted, pal.accent, mix));
    const float cy = (rc.top + rc.bottom) * 0.5f;
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                    icons::IconKind::ChevronDown,
                    {rc.right - 16.0f, cy - 4.5f, rc.right - 7.0f, cy + 4.5f}, 1.55f, chevDeg);
}

void AppWindow::DrawSegmentedControl(const ThemePalette& pal, const D2D1_RECT_F& boxRc, int animGroupId,
                                     const std::vector<SegmentedOption>& options, bool enabled,
                                     float boxRadius, float pillRadius, float fontSize) {
    DrawRoundedBox(boxRc, boxRadius, pal.surfaceAlt, pal.border, 1.0f);
    if (options.empty()) return;

    const SegmentedOption* activeOpt = &options.front();
    for (const auto& o : options) {
        if (o.selected) { activeOpt = &o; break; }
    }

    const float pillX = AnimSlot(animGroupId * 2,     activeOpt->x, 20.0f);
    const float pillW = AnimSlot(animGroupId * 2 + 1, activeOpt->w, 20.0f);

    for (const auto& o : options) {
        const D2D1_RECT_F r{o.x, boxRc.top + 3.0f, o.x + o.w, boxRc.bottom - 3.0f};
        RegisterHit(o.id, r, enabled);
        const float hAmt = enabled ? EaseOutCubic(GetHoverAnim(o.id)) : 0.0f;
        if (!o.selected && hAmt > 0.01f) {
            DrawRoundedBox(r, pillRadius, ColorLerp(D2dColor(0, 0, 0, 0), pal.accentTint, hAmt), D2dColor(0, 0, 0, 0), 0.0f);
        }
    }

    const D2D1_RECT_F sliderRc{pillX, boxRc.top + 3.0f, pillX + pillW, boxRc.bottom - 3.0f};
    DrawRoundedBox(sliderRc, pillRadius, enabled ? pal.accent : pal.borderStrong, D2dColor(0, 0, 0, 0), 0.0f);

    IDWriteTextFormat* fmt = (fontSize <= 9.5f) ? m_fmtBold9.Get() : m_fmtSemi10.Get();
    for (const auto& o : options) {
        const D2D1_RECT_F r{o.x, boxRc.top + 3.0f, o.x + o.w, boxRc.bottom - 3.0f};
        const float sAmt = AnimSlot(30000 + static_cast<int>(o.id), o.selected ? 1.0f : 0.0f, 18.0f);
        const float hAmt = enabled ? EaseOutCubic(GetHoverAnim(o.id)) : 0.0f;
        const D2D1_COLOR_F baseFg = enabled ? ColorLerp(pal.textSub, pal.accent, hAmt * 0.65f) : pal.textMuted;
        const D2D1_COLOR_F fg = ColorLerp(baseFg, D2dColor(255, 255, 255), sAmt);
        m_brush->SetColor(fg);
        if (o.iconId >= 0) {
            const float cy = (r.top + r.bottom) * 0.5f;
            const float icoSz = (o.w >= 72.0f) ? 12.0f : 11.0f;
            const float icoPad = (o.w >= 72.0f) ? 9.0f : 6.0f;
            icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                            static_cast<icons::IconKind>(o.iconId),
                            {r.left + icoPad, cy - icoSz * 0.5f, r.left + icoPad + icoSz, cy + icoSz * 0.5f}, 1.5f);
            DrawTextStrict(o.label, fmt, {r.left + icoPad + icoSz + 3.5f, r.top, r.right - 3.0f, r.bottom}, fg);
        } else {
            DrawTextStrict(o.label, fmt, r, fg, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

void AppWindow::TogglePopup(PopupKind kind, D2D1_RECT_F anchorRc, bool openUp) {
    if (kind != PopupKind::None) {
        m_lastPopup = kind;
        m_openPopup = kind;
        m_popupAnchorRc = anchorRc;
        m_popupOpenUp = openUp;
        m_popupScroll = 0.0f;
        m_popupAnim = 0.01f;
    } else if (m_openPopup != PopupKind::None) {
        m_lastPopup = m_openPopup;
        m_openPopup = PopupKind::None;
    }
    KickUiAnimation();
    InvalidateView();
}

void AppWindow::SwitchToSettings(bool showSettings) {
    if (m_showingSettings == showSettings) return;
    CommitEditField(true);
    TogglePopup(PopupKind::None);
    RefreshAvailableLists();
    m_showingSettings = showSettings;
    if (showSettings) {
        m_gearAngle = 0.0f;
    }
    SyncEnginesFromConfig();
    KickUiAnimation();
    InvalidateView();
}

void AppWindow::StartWindowFade(FadeMode mode) {
    if (!m_hwnd) return;
    KillTimer(m_hwnd, IDT_WINDOW_FADE);
    m_fadeMode = FadeMode::None;
    ClearLayeredStyle();

    if (mode == FadeMode::In) {
        if (IsIconic(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
        InvalidateView();
    } else if (mode == FadeMode::OutHide) {
        ShowWindow(m_hwnd, SW_HIDE);
    } else if (mode == FadeMode::OutMinimize) {
        ShowWindow(m_hwnd, SW_MINIMIZE);
    } else if (mode == FadeMode::OutClose) {
        m_closingForReal = true;
        DestroyWindow(m_hwnd);
    }
}

void AppWindow::ApplyLayeredAlpha(BYTE alpha) { SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA); }
void AppWindow::ClearLayeredStyle() {
    LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_LAYERED) SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
}

void AppWindow::ShowMainWindow() { StartWindowFade(FadeMode::In); }
void AppWindow::HideToTray()     { StartWindowFade(FadeMode::OutHide); }
void AppWindow::MinimizeWindow() { StartWindowFade(FadeMode::OutMinimize); }

void AppWindow::DrawTextStrict(const std::wstring& text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rc,
                               D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT hAlign,
                               DWRITE_PARAGRAPH_ALIGNMENT vAlign, bool wrap) {
    if (!m_rt || !m_brush || !fmt || text.empty()) return;
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(hAlign);
    fmt->SetParagraphAlignment(vAlign);
    m_brush->SetColor(color);
    m_rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, rc, m_brush.Get());
    if (wrap) fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

float AppWindow::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* fmt) {
    if (!m_dwFactory || !fmt || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(m_dwFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                             fmt, 2000.0f, 100.0f, layout.GetAddressOf()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm))) return 0.0f;
    return tm.widthIncludingTrailingWhitespace;
}

void AppWindow::DrawRoundedBox(const D2D1_RECT_F& rc, float radius, D2D1_COLOR_F fill,
                               D2D1_COLOR_F border, float borderW) {
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rc, radius, radius);
    if (fill.a > 0.0f) {
        m_brush->SetColor(fill);
        m_rt->FillRoundedRectangle(rr, m_brush.Get());
    }
    if (border.a > 0.0f && borderW > 0.0f) {
        m_brush->SetColor(border);
        m_rt->DrawRoundedRectangle(rr, m_brush.Get(), borderW);
    }
}

void AppWindow::DrawButton(const ThemePalette& pal, ControlId id, const D2D1_RECT_F& rc,
                           const std::wstring& text, bool primaryRed, bool selectedRedTint,
                           bool enabled, float fontSize, bool bold, float radius,
                           int argInt, std::wstring argStr) {
    RegisterHit(id, rc, enabled, argInt, argStr);
    const float hAmt = enabled ? EaseOutCubic(GetHoverAnim(id, argInt, argStr)) : 0.0f;
    const bool prs = enabled && IsControlPressed(id, argInt, argStr);
    const float prsAmt = enabled ? AnimSlot(36000 + static_cast<int>(id) + (argInt * 256), prs ? 1.0f : 0.0f, 32.0f) : 0.0f;
    const float selAmt = AnimSlot(35000 + static_cast<int>(id) + (argInt * 256), selectedRedTint ? 1.0f : 0.0f, 18.0f);

    D2D1_RECT_F drawRc = rc;
    if (prsAmt > 0.02f) {
        const float inset = 0.85f * prsAmt;
        drawRc = {rc.left + inset, rc.top + inset * 1.5f, rc.right - inset, rc.bottom - inset * 0.5f};
    }

    D2D1_COLOR_F fill = pal.card;
    D2D1_COLOR_F border = pal.borderStrong;
    D2D1_COLOR_F fg = pal.text;

    if (primaryRed) {
        fill = !enabled ? pal.borderStrong : ColorLerp(ColorLerp(pal.accent, pal.accentHover, hAmt), pal.accentPress, prsAmt);
        border = fill;
        fg = D2dColor(255, 255, 255);
    } else {
        const D2D1_COLOR_F idleFill   = ColorLerp(pal.card, pal.surface, hAmt);
        const D2D1_COLOR_F idleBorder = ColorLerp(pal.border, pal.accentBorder, hAmt * 0.6f);
        const D2D1_COLOR_F selFill    = ColorLerp(pal.accentTint, pal.surface, hAmt * 0.25f);
        fill   = ColorLerp(ColorLerp(idleFill, selFill, selAmt), pal.surfaceAlt, prsAmt * 0.65f);
        border = ColorLerp(idleBorder, pal.accent, selAmt);
        fg     = enabled ? ColorLerp(pal.text, pal.accent, selAmt) : pal.textMuted;
    }

    DrawRoundedBox(drawRc, radius, fill, border, 1.0f);

    IDWriteTextFormat* fmt = m_fmtSemi11.Get();
    if (fontSize <= 10.2f) fmt = bold ? m_fmtBold9.Get() : m_fmtSemi10.Get();
    else if (fontSize <= 10.8f) fmt = bold ? m_fmtBold105.Get() : m_fmtSemi11.Get();
    else fmt = bold ? m_fmtBold115.Get() : m_fmtSemi11.Get();

    DrawTextStrict(text, fmt, drawRc, fg, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void AppWindow::DrawToggleSwitch(const ThemePalette& pal, ControlId id, float rightX, float centerY,
                                 bool isOn, bool enabled) {
    const D2D1_RECT_F rc{rightX - 38.0f, centerY - 10.0f, rightX, centerY + 10.0f};
    RegisterHit(id, rc, enabled);
    const float hAmt = enabled ? EaseOutCubic(GetHoverAnim(id)) : 0.0f;
    const float tVal = EaseOutCubic(GetToggleAnim(id, isOn));

    const D2D1_COLOR_F offBg = enabled ? ColorLerp(pal.border, pal.borderStrong, hAmt) : pal.surfaceAlt;
    const D2D1_COLOR_F onBg  = enabled ? ColorLerp(pal.accent, pal.accentHover, hAmt) : pal.accentBorder;
    const D2D1_COLOR_F trackBg  = ColorLerp(offBg, onBg, tVal);
    const D2D1_COLOR_F trackBrd = ColorLerp(pal.borderStrong, onBg, tVal);

    if (tVal > 0.02f) {
        D2D1_COLOR_F halo = pal.accent;
        halo.a = 0.16f * tVal;
        DrawRoundedBox({rc.left - 1.5f, rc.top - 1.5f, rc.right + 1.5f, rc.bottom + 1.5f}, 11.5f, halo, D2dColor(0,0,0,0), 0.0f);
    }
    DrawRoundedBox(rc, 10.0f, trackBg, trackBrd, 1.0f);

    const float knobX = (rc.left + 10.0f) + ((rc.right - 10.0f) - (rc.left + 10.0f)) * tVal;
    const float transitVel = std::sinf(tVal * 3.14159265f);
    const float knobRx = 6.4f + (7.2f - 6.4f) * tVal + 1.2f * transitVel;
    const float knobRy = 6.4f + (7.2f - 6.4f) * tVal;
    const D2D1_COLOR_F knobCol = ColorLerp(enabled ? pal.text : pal.textMuted, D2dColor(255, 255, 255), tVal);
    m_brush->SetColor(knobCol);
    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobRx, knobRy), m_brush.Get());
}

void AppWindow::DrawRedSlider(const ThemePalette& pal, ControlId id, float leftX, float rightX, float centerY,
                              double val, double minVal, double maxVal, double stepVal, bool enabled) {
    const D2D1_RECT_F hitRc{leftX, centerY - 10.0f, rightX, centerY + 10.0f};
    HitControl hc{};
    hc.id = id;
    hc.rect = hitRc;
    hc.enabled = enabled;
    hc.isSlider = true;
    hc.sliderMin = minVal;
    hc.sliderMax = maxVal;
    hc.sliderStep = stepVal;
    m_hitControls.push_back(hc);

    const float tLeft = leftX + 8.0f;
    const float tRight = rightX - 8.0f;
    const double span = std::max(1.0, maxVal - minVal);
    const float targetRatio = static_cast<float>(std::clamp((val - minVal) / span, 0.0, 1.0));
    const bool isDragging = (m_draggingSliderId == id);
    if (isDragging) {
        auto& anim = m_floatAnims[40000 + static_cast<int>(id)];
        anim.cur = targetRatio;
        anim.target = targetRatio;
    }
    const float dispRatio = isDragging
        ? targetRatio
        : AnimSlot(40000 + static_cast<int>(id), targetRatio, 24.0f);
    const float thumbX = tLeft + dispRatio * (tRight - tLeft);

    const bool isHovered = enabled && IsControlHovered(id);
    const float hAmt = enabled ? EaseOutCubic(AnimSlot(41000 + static_cast<int>(id), (isHovered || isDragging) ? 1.0f : 0.0f, 20.0f)) : 0.0f;
    const float dragAmt = AnimSlot(41500 + static_cast<int>(id), isDragging ? 1.0f : 0.0f, 26.0f);

    const float trackHalfH = 2.0f + 0.5f * hAmt;
    DrawRoundedBox(D2D1::RectF(tLeft, centerY - trackHalfH, tRight, centerY + trackHalfH), 2.5f,
                   pal.border, D2dColor(0, 0, 0, 0), 0.0f);
    if (thumbX > tLeft + 0.5f) {
        if (hAmt > 0.01f) {
            D2D1_COLOR_F trackHalo = pal.accent;
            trackHalo.a = 0.22f * hAmt;
            DrawRoundedBox(D2D1::RectF(tLeft - 1.0f, centerY - trackHalfH - 1.0f, thumbX + 1.0f, centerY + trackHalfH + 1.0f),
                           3.0f, trackHalo, D2dColor(0, 0, 0, 0), 0.0f);
        }
        DrawRoundedBox(D2D1::RectF(tLeft, centerY - trackHalfH, thumbX, centerY + trackHalfH), 2.5f,
                       enabled ? pal.accent : pal.borderStrong, D2dColor(0, 0, 0, 0), 0.0f);
    }

    if (hAmt > 0.02f) {
        D2D1_COLOR_F halo = pal.accent;
        halo.a = (0.18f + 0.16f * dragAmt) * hAmt;
        const float haloR = 9.0f + 5.5f * hAmt + 3.0f * dragAmt;
        m_brush->SetColor(halo);
        m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, centerY), haloR, haloR), m_brush.Get());
    }

    const float knobR = 6.2f + 2.0f * hAmt + 0.8f * dragAmt;
    m_brush->SetColor(pal.card);
    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, centerY), knobR, knobR), m_brush.Get());
    m_brush->SetColor(enabled ? ColorLerp(pal.accent, pal.accentHover, hAmt) : pal.textMuted);
    m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, centerY), knobR, knobR), m_brush.Get(), 2.2f + 0.4f * dragAmt);

    if (dragAmt > 0.05f) {
        D2D1_COLOR_F pipCol = pal.accent;
        pipCol.a = dragAmt;
        m_brush->SetColor(pipCol);
        m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, centerY), 2.2f * dragAmt, 2.2f * dragAmt), m_brush.Get());
    }
}

void AppWindow::DrawVuMeter(const ThemePalette& pal, const D2D1_RECT_F& rc, float level, float peakLevel, bool muted) {
    DrawRoundedBox(rc, 3.0f, pal.surfaceAlt, pal.border, 1.0f);
    if (muted || (level <= 0.005f && peakLevel <= 0.005f)) return;

    const float dbScaled = std::clamp(std::pow(level, 0.45f), 0.0f, 1.0f);
    const float peakScaled = std::clamp(std::pow(peakLevel, 0.45f), 0.0f, 1.0f);
    const float innerW = (rc.right - rc.left) - 2.0f;
    const float fillW = innerW * dbScaled;
    const float peakX = (rc.left + 1.0f) + innerW * peakScaled;

    const float x0 = rc.left + 1.0f;
    const float xEnd = x0 + fillW;
    const float splitAmber = x0 + innerW * 0.70f;
    const float splitRed   = x0 + innerW * 0.88f;

    if (fillW >= 1.0f) {
        const float gEnd = std::min(xEnd, splitAmber);
        if (gEnd > x0) {
            m_brush->SetColor(pal.green);
            m_rt->FillRectangle(D2D1::RectF(x0, rc.top + 1.0f, gEnd, rc.bottom - 1.0f), m_brush.Get());
        }
        if (xEnd > splitAmber) {
            const float aEnd = std::min(xEnd, splitRed);
            m_brush->SetColor(pal.amber);
            m_rt->FillRectangle(D2D1::RectF(splitAmber, rc.top + 1.0f, aEnd, rc.bottom - 1.0f), m_brush.Get());
        }
        if (xEnd > splitRed) {
            m_brush->SetColor(pal.accent);
            m_rt->FillRectangle(D2D1::RectF(splitRed, rc.top + 1.0f, xEnd, rc.bottom - 1.0f), m_brush.Get());
        }
    }

    const int numTicks = 16;
    m_brush->SetColor(D2dColor(15, 15, 18, 140));
    for (int t = 1; t < numTicks; ++t) {
        const float tx = x0 + (innerW / static_cast<float>(numTicks)) * static_cast<float>(t);
        if (tx < xEnd) {
            m_rt->DrawLine({tx, rc.top + 1.0f}, {tx, rc.bottom - 1.0f}, m_brush.Get(), 1.0f);
        }
    }

    if (peakX > x0 + 1.5f && peakX < rc.right - 1.0f) {
        D2D1_COLOR_F peakCol = (peakScaled > 0.88f) ? pal.accent : ((peakScaled > 0.70f) ? pal.amber : pal.green);
        m_brush->SetColor(peakCol);
        m_rt->FillRectangle(D2D1::RectF(peakX - 1.5f, rc.top + 1.0f, peakX + 0.5f, rc.bottom - 1.0f), m_brush.Get());
    }
}

void AppWindow::DrawInlineInputBox(const ThemePalette& pal, ControlId id, EditFieldId field,
                                   const D2D1_RECT_F& rc, const std::wstring& displayVal, bool enabled) {
    RegisterHit(id, rc, enabled);
    const bool active = (m_activeEditField == field);
    const float hAmt = enabled ? EaseOutCubic(GetHoverAnim(id)) : 0.0f;
    const float aAmt = AnimSlot(49000 + static_cast<int>(id), active ? 1.0f : 0.0f, 20.0f);
    const D2D1_COLOR_F bg  = ColorLerp(pal.surface, (active && m_editSelectAll) ? pal.accentTint : pal.card, std::max(aAmt, hAmt * 0.4f));
    const D2D1_COLOR_F brd = ColorLerp(ColorLerp(pal.border, pal.borderStrong, hAmt), pal.accent, aAmt);
    DrawRoundedBox(rc, 5.0f, bg, brd, 1.0f + 0.5f * aAmt);

    std::wstring txt = active ? m_editBuffer : displayVal;
    const D2D1_RECT_F txtRc{rc.left + 6.0f, rc.top, rc.right - 6.0f, rc.bottom};
    DrawTextStrict(txt, m_fmtSemi10.Get(), txtRc, enabled ? pal.text : pal.textMuted);

    if (active && m_caretVisible) {
        const float tw = std::min((rc.right - rc.left) - 14.0f, MeasureTextWidth(txt, m_fmtSemi10.Get()));
        const float cx = txtRc.left + tw + 1.0f;
        m_brush->SetColor(pal.accent);
        m_rt->DrawLine({cx, rc.top + 5.0f}, {cx, rc.bottom - 5.0f}, m_brush.Get(), 1.5f);
    }
}

ID2D1Bitmap* AppWindow::GetOrLoadOverlayD2DBitmap(const std::wstring& imagePath) {
    if (imagePath.empty() || !m_rt) return nullptr;
    auto it = m_overlayD2DBmps.find(imagePath);
    if (it != m_overlayD2DBmps.end()) return it->second.Get();

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!compat::DecodeImageFileToPbgra(imagePath, w, h, px) || w == 0 || h == 0) return nullptr;

    ComPtr<ID2D1Bitmap> bmp;
    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    if (FAILED(m_rt->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, bmp.GetAddressOf())) || !bmp) {
        return nullptr;
    }
    auto [insIt, ok] = m_overlayD2DBmps.emplace(imagePath, std::move(bmp));
    return insIt->second.Get();
}

D2D1_RECT_F AppWindow::ComputeOverlayCanvasRect(const OverlayItem& ov, const D2D1_RECT_F& drawRc) {
    const float cw = drawRc.right - drawRc.left;
    const float ch = drawRc.bottom - drawRc.top;
    const float left = drawRc.left + ov.xNorm * cw;
    const float top  = drawRc.top  + ov.yNorm * ch;

    if (ov.kind == OverlayKind::Image) {
        float w = std::max(24.0f, ov.boxWidthNorm * cw);
        float h = w * 0.65f;
        if (auto* bmp = GetOrLoadOverlayD2DBitmap(ov.imagePath)) {
            const D2D1_SIZE_F sz = bmp->GetSize();
            if (sz.width > 0.0f) h = w * (sz.height / sz.width);
        }
        return D2D1::RectF(left, top, left + w, top + h);
    }
    if (ov.kind == OverlayKind::Box) {
        const float w = std::max(28.0f, ov.boxWidthNorm * cw);
        const float h = std::max(20.0f, ov.boxHeightNorm * ch);
        return D2D1::RectF(left, top, left + w, top + h);
    }

    const std::wstring txt = (ov.kind == OverlayKind::Clock) ? OverlayCompositor::FormatCurrentClockText()
                                                             : (ov.text.empty() ? L"Text" : ov.text);
    const float scale = ch / 1080.0f;
    const float uiFont = std::clamp(static_cast<float>(ov.fontSize) * scale, 10.0f, 64.0f);

    ComPtr<IDWriteTextFormat> tmpFmt;
    ComPtr<IDWriteTextLayout> tmpLay;
    float w = 80.0f, h = uiFont * 1.4f;
    if (m_dwFactory && SUCCEEDED(m_dwFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, uiFont, L"en-US", tmpFmt.GetAddressOf()))) {
        tmpFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (SUCCEEDED(m_dwFactory->CreateTextLayout(txt.c_str(), static_cast<UINT32>(txt.size()),
                                                    tmpFmt.Get(), cw, ch, tmpLay.GetAddressOf()))) {
            DWRITE_TEXT_METRICS tm{};
            tmpLay->GetMetrics(&tm);
            const float padX = std::max(6.0f, uiFont * 0.45f);
            const float padY = std::max(3.0f, uiFont * 0.22f);
            w = tm.widthIncludingTrailingWhitespace + padX * 2.0f;
            h = tm.height + padY * 2.0f;
        }
    }
    return D2D1::RectF(left, top, left + w, top + h);
}

void AppWindow::DrawObsSelectionFrame(const ThemePalette& pal, const D2D1_RECT_F& rc,
                                      ControlId resizeId, int argInt, bool fourCorners) {
    const D2D1_RECT_F frame{rc.left + 1.0f, rc.top + 1.0f, rc.right - 1.0f, rc.bottom - 1.0f};
    if (frame.right <= frame.left + 4.0f || frame.bottom <= frame.top + 4.0f) return;

    const bool softCamOutline = (resizeId == ControlId::CanvasCamResize && m_config.camBorder);
    DrawRoundedBox(frame, 1.5f, D2dColor(0, 0, 0, 0),
                   softCamOutline ? D2dColor(255, 255, 255, 165) : pal.accent,
                   softCamOutline ? 1.1f : 1.8f);

    if (fourCorners) {
        const D2D1_POINT_2F corners[4] = {
            {frame.left, frame.top},
            {frame.right, frame.top},
            {frame.left, frame.bottom},
            {frame.right, frame.bottom}
        };
        for (int c = 0; c < 4; ++c) {
            const float cx = corners[c].x;
            const float cy = corners[c].y;
            const D2D1_RECT_F hitHnd{cx - 6.0f, cy - 6.0f, cx + 6.0f, cy + 6.0f};
            RegisterHit(resizeId, hitHnd, true, c);
            const float hAmt = EaseOutCubic(GetHoverAnim(resizeId, c));
            const float ext = 5.0f + 1.4f * hAmt;
            DrawRoundedBox({cx - ext, cy - ext, cx + ext, cy + ext}, 2.0f,
                           ColorLerp(D2dColor(255, 255, 255), pal.accent, hAmt), pal.accent, 1.8f);
        }
    } else {
        const D2D1_RECT_F hnd{frame.right - 9.0f, frame.bottom - 9.0f, frame.right + 1.0f, frame.bottom + 1.0f};
        RegisterHit(resizeId, hnd, true, argInt);
        const float hAmt = EaseOutCubic(GetHoverAnim(resizeId, argInt));
        const float ext = 1.4f * hAmt;
        DrawRoundedBox({hnd.left - ext, hnd.top - ext, hnd.right + ext * 0.4f, hnd.bottom + ext * 0.4f},
                       2.0f, ColorLerp(D2dColor(255, 255, 255), pal.accent, hAmt), pal.accent, 1.6f);
    }
}

ID2D1Bitmap* AppWindow::GetOrCreateLogoBitmap() {
    if (!m_rt) return nullptr;
    if (m_logoBitmap) return m_logoBitmap.Get();
    if (!m_logoTriedLoad) {
        m_logoTriedLoad = true;
        HINSTANCE hMod = m_hInst ? m_hInst : GetModuleHandleW(nullptr);
        compat::DecodeImageResourceToPbgra(hMod, IDB_VR_LOGO, m_logoW, m_logoH, m_logoPx);
        if (m_logoPx.empty()) {
            compat::DecodeImageResourceToPbgra(GetModuleHandleW(nullptr), IDB_VR_LOGO, m_logoW, m_logoH, m_logoPx);
        }
        if (m_logoPx.empty()) {
            compat::DecodeImageFileToPbgra(L"assets\\vrecorder.png", m_logoW, m_logoH, m_logoPx);
        }
        if (m_logoPx.empty()) {
            compat::DecodeImageFileToPbgra(L"..\\assets\\vrecorder.png", m_logoW, m_logoH, m_logoPx);
        }
    }
    if (m_logoW == 0 || m_logoH == 0 || m_logoPx.empty()) return nullptr;
    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    m_rt->CreateBitmap(D2D1::SizeU(m_logoW, m_logoH), m_logoPx.data(), m_logoW * 4, &bp, m_logoBitmap.GetAddressOf());
    return m_logoBitmap.Get();
}

void AppWindow::DrawTitleBar(const ThemePalette& pal) {
    float textLeft = 44.0f;
    if (ID2D1Bitmap* logo = GetOrCreateLogoBitmap()) {
        const D2D1_RECT_F srcRc = D2D1::RectF(24.0f, 98.0f, 360.0f, 314.0f);
        m_rt->DrawBitmap(logo, D2D1::RectF(12.0f, 8.0f, 52.0f, 34.0f), 1.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcRc);
        textLeft = 58.0f;
    }

    const float brandW = std::max(68.0f, MeasureTextWidth(L"VRecorder", m_fmtBold135.Get()));
    DrawTextStrict(L"VRecorder", m_fmtBold135.Get(), D2D1::RectF(textLeft, 0.0f, textLeft + brandW + 4.0f, 42.0f), pal.text);

    const RecorderStats st = m_recorder.GetStats();
    if (st.isRecording) {
        wchar_t badgeBuf[80]{};
        D2D1_COLOR_F badgeFill = pal.accent;
        D2D1_COLOR_F badgeBrd  = pal.accent;
        D2D1_COLOR_F badgeTxt  = D2dColor(255, 255, 255);
        const std::wstring hms = FormatDurationHms(st.elapsedSec);
        if (st.isPaused) {
            std::swprintf(badgeBuf, 80, L"PAUSED · %s", hms.c_str());
            badgeFill = pal.accentTint; badgeBrd = pal.accentBorder; badgeTxt = pal.accent;
        } else {
            const float pulse = 0.5f + 0.5f * std::sinf(m_recPulsePhase);
            std::swprintf(badgeBuf, 80, L"REC · %s · %.0f FPS", hms.c_str(),
                          st.actualFps > 0 ? st.actualFps : static_cast<double>(m_config.fps));
            badgeFill = ColorLerp(pal.accent, pal.accentHover, pulse * 0.55f);
            badgeBrd = badgeFill;
        }
        const float bw = std::max(116.0f, MeasureTextWidth(badgeBuf, m_fmtBold9.Get()) + 20.0f);
        const float badgeLeft = textLeft + brandW + 12.0f;
        const D2D1_RECT_F badgeRc{badgeLeft, 12.0f, badgeLeft + bw, 30.0f};
        DrawRoundedBox(badgeRc, 9.0f, badgeFill, badgeBrd, 1.0f);
        DrawTextStrict(badgeBuf, m_fmtBold9.Get(), badgeRc, badgeTxt,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    const float minX0 = m_clientW - 108.0f;
    const float maxX0 = m_clientW - 74.0f;
    const float clsX0 = m_clientW - 40.0f;

    auto drawCapBtn = [&](ControlId id, float x0, icons::IconKind ico, bool isClose) {
        const D2D1_RECT_F rc{x0, 7.0f, x0 + 30.0f, 35.0f};
        RegisterHit(id, rc, true);
        const float hAmt = EaseOutCubic(GetHoverAnim(id));
        const bool prs = IsControlPressed(id);
        if (hAmt > 0.01f || prs) {
            D2D1_COLOR_F bg = isClose
                ? (prs ? pal.accent : ColorLerp(D2dColor(0, 0, 0, 0), pal.accentTint, hAmt))
                : (prs ? pal.border : ColorLerp(D2dColor(0, 0, 0, 0), pal.surfaceAlt, hAmt));
            D2D1_COLOR_F brd = isClose ? ColorLerp(D2dColor(0, 0, 0, 0), pal.accentBorder, hAmt) : D2dColor(0, 0, 0, 0);
            DrawRoundedBox(rc, 6.0f, bg, brd, isClose ? 1.0f : 0.0f);
        }
        m_brush->SetColor(isClose ? (prs ? D2dColor(255, 255, 255) : ColorLerp(pal.text, pal.accent, hAmt)) : pal.text);
        icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), ico,
                        D2D1::RectF(x0 + 10.0f, 16.0f, x0 + 20.0f, 26.0f), 1.6f);
    };

    drawCapBtn(ControlId::WinMinimize, minX0, icons::IconKind::Minimize, false);
    drawCapBtn(ControlId::WinMaximize, maxX0, IsZoomed(m_hwnd) ? icons::IconKind::RestoreWindow : icons::IconKind::Maximize, false);
    drawCapBtn(ControlId::WinClose,    clsX0, icons::IconKind::Close, true);

    m_brush->SetColor(pal.border);
    m_rt->DrawLine({0.0f, 42.5f}, {m_clientW, 42.5f}, m_brush.Get(), 1.0f);
}

void AppWindow::DrawMainView(const ThemePalette& pal) {
    const float padX = 14.0f;
    const float footerH = 30.0f;
    const float dockH = 166.0f;
    const float dockBottom = m_clientH - footerH - 6.0f;
    const float dockTop = dockBottom - dockH;
    const float stageTop = 48.0f;
    const float stageBottom = dockTop - 10.0f;

    const float sideW = 318.0f;
    const float stageLeft = padX;
    const float stageRight = m_clientW - padX - sideW - 10.0f;
    const float sideLeft = stageRight + 10.0f;
    const float sideRight = m_clientW - padX;

    DrawPreviewStage(pal, {stageLeft, stageTop, stageRight, stageBottom});
    DrawSceneLayersSidebar(pal, {sideLeft, stageTop, sideRight, stageBottom});

    const float totalW = (m_clientW - padX * 2.0f) - 20.0f;
    const float w1 = std::floor(totalW * 0.28f);
    const float w2 = std::floor(totalW * 0.46f);
    const float w3 = totalW - w1 - w2;

    const D2D1_RECT_F d1{padX, dockTop, padX + w1, dockBottom};
    const D2D1_RECT_F d2{d1.right + 10.0f, dockTop, d1.right + 10.0f + w2, dockBottom};
    const D2D1_RECT_F d3{d2.right + 10.0f, dockTop, d2.right + 10.0f + w3, dockBottom};

    DrawDockAudioMixer(pal, d1);
    DrawDockOutputAndEncoder(pal, d2);
    DrawDockControls(pal, d3);
    DrawBottomBar(pal);
}

void AppWindow::DrawPreviewStage(const ThemePalette& pal, const D2D1_RECT_F& stageRc) {
    DrawRoundedBox(stageRc, 10.0f, pal.card, pal.border, 1.0f);

    const D2D1_RECT_F barRc{stageRc.left + 10.0f, stageRc.top + 6.0f, stageRc.right - 10.0f, stageRc.top + 32.0f};
    const bool hasSrc = m_config.hasCaptureSource;
    const bool needsWinPick = (hasSrc && m_config.sourceType == 1 && m_config.windowTitleFilter.empty());
    std::wstring srcTag = L"Empty Scene";
    if (!hasSrc) {
        if (m_config.camAdded || !m_config.overlays.empty()) {
            srcTag = L"Scene Components Only";
        }
    } else if (m_config.sourceType == 1) {
        srcTag = needsWinPick ? L"Window: Select Application" : (L"Window: " + m_config.windowTitleFilter);
    } else if (m_config.sourceType == 2) {
        srcTag = L"Region Crop \u00B7 " + std::to_wstring(m_config.cropW) + L"\u00D7" + std::to_wstring(m_config.cropH);
    } else if (!m_monitors.empty()) {
        const size_t idx = std::min<size_t>(static_cast<size_t>(std::max(0, m_config.monitorIndex)), m_monitors.size() - 1);
        srcTag = m_monitors[idx].label;
    }
    const std::wstring backend = hasSrc && m_config.sourceVisible ? m_capture.GetBackendName() : L"";
    if (!backend.empty() && !needsWinPick) srcTag += L" \u00B7 " + backend;

    const float toolsW = hasSrc ? 240.0f : 0.0f;
    const float tagMaxW = std::max(120.0f, (barRc.right - barRc.left) - toolsW - 8.0f);
    const float badgeW = std::min(tagMaxW, std::max(120.0f, MeasureTextWidth(srcTag, m_fmtBold9.Get()) + 30.0f));
    const D2D1_RECT_F tagRc{barRc.left, barRc.top + 1.0f, barRc.left + badgeW, barRc.bottom - 1.0f};
    DrawRoundedBox(tagRc, 6.0f, needsWinPick ? pal.accentTint : pal.surface,
                   needsWinPick ? pal.accentBorder : pal.border, 1.0f);
    m_brush->SetColor(needsWinPick ? pal.accent : pal.accent);
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                    icons::IconKind::Monitor,
                    {tagRc.left + 7.0f, tagRc.top + 4.5f, tagRc.left + 19.0f, tagRc.bottom - 4.5f}, 1.4f);
    DrawTextStrict(srcTag, m_fmtBold9.Get(), {tagRc.left + 23.0f, tagRc.top, tagRc.right - 8.0f, tagRc.bottom},
                   needsWinPick ? pal.accent : pal.textSub);

    if (hasSrc) {

        struct QuickT { ControlId id; const wchar_t* lbl; bool active; };
        const bool isFit = m_config.srcKeepAspect && std::fabs(m_config.srcScaleW - 1.0f) < 0.02f && std::fabs(m_config.srcPosX) < 0.02f;
        const bool isFill = !m_config.srcKeepAspect && std::fabs(m_config.srcScaleW - 1.0f) < 0.02f && std::fabs(m_config.srcPosX) < 0.02f;
        const QuickT qts[5] = {
            {ControlId::SrcFitBtn,    L"Fit",    isFit},
            {ControlId::SrcFillBtn,   L"Fill",   isFill},
            {ControlId::SrcCenterBtn, L"Center", false},
            {ControlId::SrcPipBtn,    L"75%",    m_config.srcScaleW < 0.85f},
            {ControlId::SrcResetBtn,  L"Reset",  false}
        };
        float tx = barRc.right - 236.0f;
        for (const auto& q : qts) {
            DrawButton(pal, q.id, {tx, barRc.top + 1.0f, tx + 44.0f, barRc.bottom - 1.0f},
                       q.lbl, false, q.active, true, 9.2f, true, 5.0f, 1);
            tx += 48.0f;
        }
    }

    const D2D1_RECT_F vpRc{stageRc.left + 8.0f, stageRc.top + 36.0f, stageRc.right - 8.0f, stageRc.bottom - 8.0f};
    DrawRoundedBox(vpRc, 8.0f, pal.canvasBg, pal.border, 1.0f);

    uint32_t outW = 1920, outH = 1080;
    m_recorder.ResolveOutputDimensions(m_config, outW, outH);
    const float targetAspect = static_cast<float>(outW) / static_cast<float>(std::max<uint32_t>(1, outH));

    const float padVp = 8.0f;
    const float availW = std::max(10.0f, (vpRc.right - vpRc.left) - padVp * 2.0f);
    const float availH = std::max(10.0f, (vpRc.bottom - vpRc.top) - padVp * 2.0f);
    float drawW = availW;
    float drawH = drawW / std::max(0.1f, targetAspect);
    if (drawH > availH) {
        drawH = availH;
        drawW = drawH * targetAspect;
    }
    const D2D1_RECT_F drawRc{
        vpRc.left + padVp + (availW - drawW) * 0.5f,
        vpRc.top  + padVp + (availH - drawH) * 0.5f,
        vpRc.left + padVp + (availW - drawW) * 0.5f + drawW,
        vpRc.top  + padVp + (availH - drawH) * 0.5f + drawH
    };
    m_activeDrawRc = drawRc;

    DrawRoundedBox(drawRc, 2.0f, D2dColor(9, 9, 11), D2dColor(56, 56, 66), 1.0f);
    m_rt->PushAxisAlignedClip(drawRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const bool completelyEmpty = !m_config.hasCaptureSource && !(m_config.camAdded && m_config.camEnabled) && m_config.overlays.empty();
    if (completelyEmpty) {
        const float cx = (drawRc.left + drawRc.right) * 0.5f;
        const float cy = (drawRc.top + drawRc.bottom) * 0.5f;
        m_brush->SetColor(D2dColor(161, 161, 170));
        icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                        icons::IconKind::Layers, D2D1::RectF(cx - 18.0f, cy - 58.0f, cx + 18.0f, cy - 22.0f), 1.7f);
        DrawTextStrict(L"Scene Canvas is Empty", m_fmtBold135.Get(),
                       D2D1::RectF(drawRc.left + 20.0f, cy - 14.0f, drawRc.right - 20.0f, cy + 12.0f),
                       D2dColor(244, 244, 245), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawTextStrict(L"Add a Display / Application Window or Component to build your studio layout.", m_fmtReg105.Get(),
                       D2D1::RectF(drawRc.left + 20.0f, cy + 14.0f, drawRc.right - 20.0f, cy + 36.0f),
                       D2dColor(161, 161, 170), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const D2D1_RECT_F b1{cx - 160.0f, cy + 48.0f, cx - 6.0f, cy + 78.0f};
        const D2D1_RECT_F b2{cx + 6.0f, cy + 48.0f, cx + 160.0f, cy + 78.0f};
        DrawButton(pal, ControlId::AddSourceBtn, b1, L"+ Display / Window", true, false, true, 10.5f);
        DrawButton(pal, ControlId::AddComponentBtn, b2, L"+ Add Component", false, false, true, 10.5f);
        m_rt->PopAxisAlignedClip();
        return;
    }

    if (m_config.hasCaptureSource && m_config.sourceVisible) {
        const uint32_t srcW = m_previewFramePtr ? m_previewFramePtr->width : 0u;
        const uint32_t srcH = m_previewFramePtr ? m_previewFramePtr->height : 0u;
        uint32_t contentW = (srcW > 0) ? srcW : outW;
        uint32_t contentH = (srcH > 0) ? srcH : outH;
        D2D1_RECT_F srcCropSubRc{};
        if (srcW > 0 && srcH > 0) {
            if (m_config.sourceType == 2) {
                const uint32_t cx = static_cast<uint32_t>(std::clamp(m_config.cropX, 0, static_cast<int>(srcW - 1)));
                const uint32_t cy = static_cast<uint32_t>(std::clamp(m_config.cropY, 0, static_cast<int>(srcH - 1)));
                const uint32_t cw = static_cast<uint32_t>(std::clamp(m_config.cropW, 2, static_cast<int>(srcW - cx)));
                const uint32_t ch = static_cast<uint32_t>(std::clamp(m_config.cropH, 2, static_cast<int>(srcH - cy)));
                contentW = cw;
                contentH = ch;
                srcCropSubRc = D2D1::RectF(static_cast<float>(cx), static_cast<float>(cy),
                                           static_cast<float>(cx + cw), static_cast<float>(cy + ch));
            } else {
                srcCropSubRc = D2D1::RectF(0.0f, 0.0f, static_cast<float>(srcW), static_cast<float>(srcH));
            }
        }

        float plX = 0.0f, plY = 0.0f, plW = drawW, plH = drawH;
        RecorderEngine::ComputeSourcePlacement(
            static_cast<uint32_t>(std::max<long>(1L, std::lround(drawW))),
            static_cast<uint32_t>(std::max<long>(1L, std::lround(drawH))),
            contentW, contentH, m_config, plX, plY, plW, plH);
        const D2D1_RECT_F placedSrcRc{drawRc.left + plX, drawRc.top + plY, drawRc.left + plX + plW, drawRc.top + plY + plH};
        m_activeSourceRc = placedSrcRc;

        if (needsWinPick) {
            const float cx = (drawRc.left + drawRc.right) * 0.5f;
            const float cy = (drawRc.top + drawRc.bottom) * 0.5f;
            m_brush->SetColor(D2dColor(228, 228, 231));
            icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                            icons::IconKind::AppWindow, D2D1::RectF(cx - 18.0f, cy - 52.0f, cx + 18.0f, cy - 16.0f), 1.8f);
            DrawTextStrict(L"No Application Window Selected", m_fmtBold135.Get(),
                           D2D1::RectF(drawRc.left + 20.0f, cy - 10.0f, drawRc.right - 20.0f, cy + 14.0f),
                           D2dColor(244, 244, 245), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextStrict(L"Choose a running game, browser, or app window before recording.", m_fmtReg105.Get(),
                           D2D1::RectF(drawRc.left + 20.0f, cy + 14.0f, drawRc.right - 20.0f, cy + 34.0f),
                           D2dColor(161, 161, 170), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const D2D1_RECT_F pickCta{cx - 105.0f, cy + 44.0f, cx + 105.0f, cy + 74.0f};
            DrawButton(pal, ControlId::CanvasPickWindowBtn, pickCta, L"Select Application Window", true, false, true, 10.5f);
        } else if (srcW > 0 && srcH > 0 && m_previewFramePtr &&
                   (!m_previewFramePtr->bgra.empty() || !m_previewFramePtr->nv12.empty())) {
            const uint8_t* uploadPtr = nullptr;
            uint32_t bmpW = srcW;
            uint32_t bmpH = srcH;
            if (!m_previewFramePtr->bgra.empty()) {
                uploadPtr = m_previewFramePtr->bgra.data();
                if (m_config.sourceType != 2 && srcW > 1280) {
                    bmpW = std::max<uint32_t>(2, srcW >> 1);
                    bmpH = std::max<uint32_t>(2, srcH >> 1);
                    if (m_previewScaledSeq != m_previewFramePtr->sequence || m_previewScaledW != bmpW || m_previewScaledH != bmpH) {
                        const size_t outBytes = static_cast<size_t>(bmpW) * bmpH * 4;
                        if (m_previewScaledBuf.size() != outBytes) m_previewScaledBuf.resize(outBytes);
                        const auto* s32 = reinterpret_cast<const uint32_t*>(m_previewFramePtr->bgra.data());
                        auto* d32 = reinterpret_cast<uint32_t*>(m_previewScaledBuf.data());
                        for (uint32_t y = 0; y < bmpH; ++y) {
                            const uint32_t* sRow = s32 + static_cast<size_t>(y * 2) * srcW;
                            uint32_t* dRow = d32 + static_cast<size_t>(y) * bmpW;
                            for (uint32_t x = 0; x < bmpW; ++x) {
                                dRow[x] = sRow[x * 2];
                            }
                        }
                        m_previewScaledW = bmpW;
                        m_previewScaledH = bmpH;
                        m_previewScaledSeq = m_previewFramePtr->sequence;
                    }
                    uploadPtr = m_previewScaledBuf.data();
                    srcCropSubRc = D2D1::RectF(0.0f, 0.0f, static_cast<float>(bmpW), static_cast<float>(bmpH));
                }
            } else {

                const uint32_t nw = m_previewFramePtr->nv12Width;
                const uint32_t nh = m_previewFramePtr->nv12Height;
                bmpW = std::max<uint32_t>(2, nw >> 1);
                bmpH = std::max<uint32_t>(2, nh >> 1);
                if (m_previewScaledSeq != m_previewFramePtr->sequence || m_previewScaledW != bmpW || m_previewScaledH != bmpH) {
                    const size_t outBytes = static_cast<size_t>(bmpW) * bmpH * 4;
                    if (m_previewScaledBuf.size() != outBytes) m_previewScaledBuf.resize(outBytes);
                    const uint8_t* yp = m_previewFramePtr->nv12.data();
                    const uint8_t* uvp = yp + static_cast<size_t>(nw) * nh;
                    auto* dst32 = reinterpret_cast<uint32_t*>(m_previewScaledBuf.data());
                    for (uint32_t y = 0; y < bmpH; ++y) {
                        const uint32_t sy = y * 2;
                        const uint8_t* yRow = yp + static_cast<size_t>(sy) * nw;
                        const uint8_t* uvRow = uvp + static_cast<size_t>(sy >> 1) * nw;
                        uint32_t* dRow = dst32 + static_cast<size_t>(y) * bmpW;
                        for (uint32_t x = 0; x < bmpW; ++x) {
                            const uint32_t sx = x * 2;
                            const int c = static_cast<int>(yRow[sx]) - 16;
                            const int d = static_cast<int>(uvRow[sx & ~1u]) - 128;
                            const int e = static_cast<int>(uvRow[(sx & ~1u) + 1]) - 128;
                            const uint32_t r = static_cast<uint32_t>(std::clamp((298 * c + 459 * e + 128) >> 8, 0, 255));
                            const uint32_t g = static_cast<uint32_t>(std::clamp((298 * c - 55 * d - 136 * e + 128) >> 8, 0, 255));
                            const uint32_t b = static_cast<uint32_t>(std::clamp((298 * c + 541 * d + 128) >> 8, 0, 255));
                            dRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                        }
                    }
                    m_previewScaledW = bmpW;
                    m_previewScaledH = bmpH;
                    m_previewScaledSeq = m_previewFramePtr->sequence;
                }
                uploadPtr = m_previewScaledBuf.data();
                srcCropSubRc = D2D1::RectF(0.0f, 0.0f, static_cast<float>(bmpW), static_cast<float>(bmpH));
            }
            if (uploadPtr) {
                const ColorFilterParams colorParams{
                    m_config.colorFilterEnabled,
                    m_config.filterGamma,
                    m_config.filterContrast,
                    m_config.filterBrightness,
                    m_config.filterSaturation
                };
                if (colorParams.HasEffect()) {
                    if (uploadPtr != m_previewScaledBuf.data()) {
                        const size_t outBytes = static_cast<size_t>(bmpW) * bmpH * 4;
                        if (m_previewScaledBuf.size() != outBytes) m_previewScaledBuf.resize(outBytes);
                        std::memcpy(m_previewScaledBuf.data(), uploadPtr, outBytes);
                        uploadPtr = m_previewScaledBuf.data();
                    }
                    ColorCorrection::ApplyBgra(m_previewScaledBuf.data(), static_cast<size_t>(bmpW) * bmpH, colorParams);
                }

                if (!m_previewBitmap || m_previewBmpW != bmpW || m_previewBmpH != bmpH) {
                    m_previewBitmap.Reset();
                    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
                    );
                    m_rt->CreateBitmap(D2D1::SizeU(bmpW, bmpH), uploadPtr,
                                       bmpW * 4, &bp, m_previewBitmap.GetAddressOf());
                    m_previewBmpW = bmpW;
                    m_previewBmpH = bmpH;
                } else if (m_previewSeqRendered != m_previewFramePtr->sequence) {
                    m_previewBitmap->CopyFromMemory(nullptr, uploadPtr, bmpW * 4);
                }
                m_previewSeqRendered = m_previewFramePtr->sequence;
            }

            RegisterHit(ControlId::CanvasSourceDrag, placedSrcRc, true);
            if (m_previewBitmap) {
                if (m_config.sourceType == 2 && m_config.srcPosX == 0.0f && m_config.srcPosY == 0.0f &&
                    m_config.srcScaleW == 1.0f && m_config.srcScaleH == 1.0f) {
                    const float fullAsp = static_cast<float>(srcW) / static_cast<float>(srcH);
                    float fw = drawW, fh = fw / std::max(0.1f, fullAsp);
                    if (fh > drawH) { fh = drawH; fw = fh * fullAsp; }
                    const D2D1_RECT_F fullRc{drawRc.left + (drawW - fw) * 0.5f, drawRc.top + (drawH - fh) * 0.5f,
                                             drawRc.left + (drawW - fw) * 0.5f + fw, drawRc.top + (drawH - fh) * 0.5f + fh};
                    m_rt->DrawBitmap(m_previewBitmap.Get(), fullRc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

                    const float fsrcW = static_cast<float>(srcW), fsrcH = static_cast<float>(srcH);
                    const float cx0 = fullRc.left + (static_cast<float>(m_config.cropX) / fsrcW) * fw;
                    const float cy0 = fullRc.top  + (static_cast<float>(m_config.cropY) / fsrcH) * fh;
                    const float cw0 = std::max(24.0f, (static_cast<float>(m_config.cropW) / fsrcW) * fw);
                    const float ch0 = std::max(24.0f, (static_cast<float>(m_config.cropH) / fsrcH) * fh);
                    const D2D1_RECT_F cropRc{cx0, cy0, std::min(fullRc.right, cx0 + cw0), std::min(fullRc.bottom, cy0 + ch0)};

                    RegisterHit(ControlId::CanvasCropDrag, cropRc, true);
                    DrawRoundedBox(cropRc, 2.0f, D2dColor(239, 68, 68, 20), pal.accent, 2.0f);
                    if ((cropRc.bottom - cropRc.top) >= 34.0f && (cropRc.right - cropRc.left) >= 136.0f) {
                        wchar_t crLbl[48]{};
                        std::swprintf(crLbl, 48, L"REGION %dx%d", m_config.cropW, m_config.cropH);
                        const D2D1_RECT_F crTag{cropRc.left + 8.0f, cropRc.bottom - 24.0f,
                                                std::min(cropRc.right - 18.0f, cropRc.left + 134.0f), cropRc.bottom - 6.0f};
                        DrawRoundedBox(crTag, 4.0f, pal.accent, pal.accent, 1.0f);
                        DrawTextStrict(crLbl, m_fmtBold9.Get(), crTag, D2dColor(255, 255, 255),
                                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                    const D2D1_RECT_F hndRc{cropRc.right - 11.0f, cropRc.bottom - 11.0f, cropRc.right + 1.0f, cropRc.bottom + 1.0f};
                    RegisterHit(ControlId::CanvasCropResize, hndRc, true);
                    DrawRoundedBox(hndRc, 2.0f, D2dColor(255, 255, 255), pal.accent, 1.8f);
                } else {
                    m_rt->DrawBitmap(m_previewBitmap.Get(), placedSrcRc, 1.0f,
                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcCropSubRc);
                }
            }
        } else {
            RegisterHit(ControlId::CanvasSourceDrag, placedSrcRc, true);
            DrawRoundedBox(placedSrcRc, 2.0f, D2dColor(14, 15, 20), D2dColor(42, 42, 52), 1.0f);
            const float cx = (placedSrcRc.left + placedSrcRc.right) * 0.5f;
            const float cy = (placedSrcRc.top + placedSrcRc.bottom) * 0.5f;
            m_brush->SetColor(D2dColor(161, 161, 170));
            icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                            icons::IconKind::Monitor,
                            D2D1::RectF(cx - 18.0f, cy - 26.0f, cx + 18.0f, cy + 10.0f), 1.8f);
            const std::wstring waitTxt = (m_config.sourceType == 1)
                ? L"Window minimized or inactive — restore to capture"
                : L"Display capture active on desktop";
            DrawTextStrict(waitTxt, m_fmtSemi11.Get(),
                           D2D1::RectF(placedSrcRc.left + 20.0f, cy + 16.0f, placedSrcRc.right - 20.0f, cy + 38.0f),
                           D2dColor(161, 161, 170), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        if (!needsWinPick && m_selectedLayer == CanvasLayer::MainSource && m_config.sourceType != 2) {
            DrawObsSelectionFrame(pal, placedSrcRc, ControlId::CanvasSourceResize, 0, true);
        }
    }

    if (m_config.camAdded && m_config.camEnabled) {
        const uint32_t cwPx = m_camPreviewFrame.width;
        const uint32_t chPx = m_camPreviewFrame.height;
        const float camAsp = (m_config.camShape == 2) ? 1.0f
                           : ((cwPx > 0 && chPx > 0) ? (static_cast<float>(chPx) / static_cast<float>(cwPx)) : 0.5625f);
        const float camW = std::max(36.0f, m_config.camWidthNorm * drawW);
        const float camH = std::max(36.0f, camW * camAsp);
        const float camX = drawRc.left + std::clamp(m_config.camXNorm * drawW, 0.0f, std::max(0.0f, drawW - camW));
        const float camY = drawRc.top  + std::clamp(m_config.camYNorm * drawH, 0.0f, std::max(0.0f, drawH - camH));
        const D2D1_RECT_F camRc{camX, camY, camX + camW, camY + camH};
        m_activeCamRc = camRc;

        RegisterHit(ControlId::CanvasCamDrag, camRc, true);
        const float cornerR = (m_config.camShape == 0) ? 2.0f
                            : ((m_config.camShape == 1) ? std::min(camW, camH) * 0.08f : std::min(camW, camH) * 0.5f);

        if (cwPx > 0 && chPx > 0 && !m_camPreviewFrame.bgra.empty()) {
            if (!m_camBitmap || m_camBmpW != cwPx || m_camBmpH != chPx) {
                m_camBitmap.Reset();
                const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
                );
                m_rt->CreateBitmap(D2D1::SizeU(cwPx, chPx), m_camPreviewFrame.bgra.data(),
                                   cwPx * 4, &bp, m_camBitmap.GetAddressOf());
                m_camBmpW = cwPx;
                m_camBmpH = chPx;
            } else if (m_camSeqRendered != m_camPreviewFrame.sequence) {
                m_camBitmap->CopyFromMemory(nullptr, m_camPreviewFrame.bgra.data(), cwPx * 4);
            }
            m_camSeqRendered = m_camPreviewFrame.sequence;

            if (m_camBitmap) {
                D2D1_RECT_F camSrcSub{0.0f, 0.0f, static_cast<float>(cwPx), static_cast<float>(chPx)};
                if (m_config.camShape == 2 && cwPx != chPx) {
                    const float side = static_cast<float>(std::min(cwPx, chPx));
                    const float ox = (cwPx - side) * 0.5f;
                    const float oy = (chPx - side) * 0.5f;
                    camSrcSub = {ox, oy, ox + side, oy + side};
                }

                bool pushedLayer = false;
                ComPtr<ID2D1Layer> clipLyr;
                ComPtr<ID2D1RoundedRectangleGeometry> rrGeo;
                if (cornerR > 2.5f &&
                    SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(camRc, cornerR, cornerR), &rrGeo)) &&
                    SUCCEEDED(m_rt->CreateLayer(&clipLyr))) {
                    m_rt->PushLayer(D2D1::LayerParameters(camRc, rrGeo.Get()), clipLyr.Get());
                    pushedLayer = true;
                }
                m_rt->DrawBitmap(m_camBitmap.Get(), camRc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &camSrcSub);
                if (pushedLayer) m_rt->PopLayer();
            }
        } else {
            DrawRoundedBox(camRc, cornerR, D2dColor(24, 24, 30, 235), D2dColor(63, 63, 70), 1.0f);
            m_brush->SetColor(pal.accent);
            icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::Camera,
                            {camRc.left + camW * 0.5f - 12.0f, camRc.top + camH * 0.5f - 18.0f,
                             camRc.left + camW * 0.5f + 12.0f, camRc.top + camH * 0.5f + 6.0f}, 1.7f);
            DrawTextStrict(m_camera.GetStatusText(), m_fmtBold9.Get(),
                           {camRc.left + 6.0f, camRc.top + camH * 0.5f + 8.0f, camRc.right - 6.0f, camRc.bottom - 4.0f},
                           D2dColor(228, 228, 231), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        if (m_config.camBorder) {
            m_brush->SetColor(pal.accent);
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(camRc, cornerR, cornerR), m_brush.Get(), 2.2f);
        }
        if (m_selectedLayer == CanvasLayer::Camera) {
            DrawObsSelectionFrame(pal, camRc, ControlId::CanvasCamResize, 0, false);
        }
    }

    for (const auto& ov : m_config.overlays) {
        if (!ov.visible) continue;
        const D2D1_RECT_F ovRc = ComputeOverlayCanvasRect(ov, drawRc);
        RegisterHit(ControlId::CanvasOverlayDrag, ovRc, true, static_cast<int>(ov.id));

        if (ov.kind == OverlayKind::Image) {
            if (auto* bmp = GetOrLoadOverlayD2DBitmap(ov.imagePath)) {
                m_rt->DrawBitmap(bmp, ovRc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        } else if (ov.kind == OverlayKind::Box) {
            const float bw = std::max(1.5f, 3.0f * (drawH / 1080.0f));
            DrawRoundedBox(ovRc, 5.0f, ov.bgEnabled ? ArgbToD2d(ov.bgArgb) : D2dColor(0, 0, 0, 0),
                           ArgbToD2d(ov.colorArgb), bw);
        } else {
            const std::wstring txt = (ov.kind == OverlayKind::Clock) ? OverlayCompositor::FormatCurrentClockText()
                                                                     : (ov.text.empty() ? L"Text" : ov.text);
            const float uiFont = std::clamp(static_cast<float>(ov.fontSize) * (drawH / 1080.0f), 10.0f, 64.0f);
            if (ov.bgEnabled) {
                DrawRoundedBox(ovRc, std::max(3.0f, uiFont * 0.28f), ArgbToD2d(ov.bgArgb), D2dColor(0, 0, 0, 0), 0.0f);
            }
            ComPtr<IDWriteTextFormat> fmt;
            if (m_dwFactory && SUCCEEDED(m_dwFactory->CreateTextFormat(
                    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, uiFont, L"en-US", fmt.GetAddressOf()))) {
                DrawTextStrict(txt, fmt.Get(), ovRc, ArgbToD2d(ov.colorArgb),
                               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }

        if (m_selectedLayer == CanvasLayer::Overlay && ov.id == m_selectedOverlayId) {
            DrawObsSelectionFrame(pal, ovRc, ControlId::CanvasOverlayResize, static_cast<int>(ov.id), false);
        }
    }

    m_rt->PopAxisAlignedClip();
}

void AppWindow::DrawSceneLayersSidebar(const ThemePalette& pal, const D2D1_RECT_F& panelRc) {
    DrawRoundedBox(panelRc, 10.0f, pal.card, pal.border, 1.0f);
    const float iLeft = panelRc.left + 12.0f;
    const float iRight = panelRc.right - 12.0f;
    float y = panelRc.top + 10.0f;

    DrawTextStrict(L"SCENE COMPONENTS", m_fmtBold9.Get(), {iLeft, y, iRight, y + 14.0f}, pal.textMuted);
    y += 18.0f;

    const float gap = 6.0f;
    const float totalBarW = iRight - iLeft;
    const float wSrc = std::floor((totalBarW - gap) * 0.54f);
    const D2D1_RECT_F addSrcRc{iLeft, y, iLeft + wSrc, y + 28.0f};
    const D2D1_RECT_F addCmpRc{addSrcRc.right + gap, y, iRight, y + 28.0f};

    RegisterHit(ControlId::AddSourceBtn, addSrcRc, true);
    const float sHovAmt = std::max(EaseOutCubic(GetHoverAnim(ControlId::AddSourceBtn)),
                                   AnimSlot(43501, (m_openPopup == PopupKind::AddSourceMenu) ? 1.0f : 0.0f, 18.0f));
    DrawRoundedBox(addSrcRc, 6.0f, ColorLerp(pal.accentTint, pal.accent, sHovAmt), pal.accentBorder, 1.0f);
    m_brush->SetColor(ColorLerp(pal.accent, D2dColor(255, 255, 255), sHovAmt));
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::Monitor,
                    {addSrcRc.left + 8.0f, y + 8.0f, addSrcRc.left + 20.0f, y + 20.0f}, 1.5f);
    DrawTextStrict(L"+ Display / Window", m_fmtBold9.Get(), {addSrcRc.left + 23.0f, y, addSrcRc.right - 4.0f, y + 28.0f},
                   ColorLerp(pal.accent, D2dColor(255, 255, 255), sHovAmt));

    RegisterHit(ControlId::AddComponentBtn, addCmpRc, true);
    const float cHovAmt = std::max(EaseOutCubic(GetHoverAnim(ControlId::AddComponentBtn)),
                                   AnimSlot(43502, (m_openPopup == PopupKind::AddComponentMenu) ? 1.0f : 0.0f, 18.0f));
    DrawRoundedBox(addCmpRc, 6.0f, ColorLerp(pal.surface, pal.accentTint, cHovAmt),
                   ColorLerp(pal.borderStrong, pal.accent, cHovAmt), 1.0f);
    m_brush->SetColor(ColorLerp(pal.text, pal.accent, cHovAmt));
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::Plus,
                    {addCmpRc.left + 8.0f, y + 8.0f, addCmpRc.left + 19.0f, y + 19.0f}, 1.7f);
    DrawTextStrict(L"Add Component", m_fmtBold9.Get(), {addCmpRc.left + 22.0f, y, addCmpRc.right - 4.0f, y + 28.0f},
                   ColorLerp(pal.text, pal.accent, cHovAmt));
    y += 35.0f;

    const float targetInspH = (m_selectedLayer == CanvasLayer::MainSource && m_srcTab == 1) ? 318.0f : 252.0f;
    const float inspH = AnimSlot(10008, targetInspH, 22.0f);
    const float listBottom = std::max(y + 64.0f, panelRc.bottom - 10.0f - inspH - 8.0f);
    const D2D1_RECT_F listBox{iLeft, y, iRight, listBottom};
    DrawRoundedBox(listBox, 7.0f, pal.surface, pal.border, 1.0f);
    m_overlayListRc = {listBox.left + 5.0f, listBox.top + 5.0f, listBox.right - 5.0f, listBox.bottom - 5.0f};

    const float rowH = 28.0f, rg = 4.0f;
    const size_t totalRows = (m_config.hasCaptureSource ? 1u : 0u) + (m_config.camAdded ? 1u : 0u) + m_config.overlays.size();
    const float totalH = totalRows > 0 ? (static_cast<float>(totalRows) * (rowH + rg) - rg) : 0.0f;
    const float viewH = m_overlayListRc.bottom - m_overlayListRc.top;
    m_overlayListMaxScroll = std::max(0.0f, totalH - viewH);
    m_overlayListScroll = std::clamp(m_overlayListScroll, 0.0f, m_overlayListMaxScroll);

    m_rt->PushAxisAlignedClip(m_overlayListRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (totalRows == 0) {
        DrawTextStrict(L"No active sources or components.\nUse + Display / Window or Add Component.",
                       m_fmtReg105.Get(), m_overlayListRc, pal.textMuted,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    } else {
        float ry = m_overlayListRc.top - m_overlayListScroll;

        auto drawEyeAndTrash = [&](float rowY, float rrRight, ControlId visId, int visArg, bool isVis, ControlId delId, int delArg) {
            const D2D1_RECT_F visRc{rrRight - 48.0f, rowY + 3.0f, rrRight - 26.0f, rowY + 25.0f};
            RegisterHit(visId, visRc, true, visArg);
            const float vHov = EaseOutCubic(GetHoverAnim(visId, visArg));
            const float vOn = AnimSlot(46000 + static_cast<int>(visId) * 64 + (visArg & 63), isVis ? 1.0f : 0.0f, 18.0f);
            if (vHov > 0.01f) DrawRoundedBox(visRc, 4.5f, ColorLerp(D2dColor(0, 0, 0, 0), pal.accentTint, vHov), D2dColor(0, 0, 0, 0), 0.0f);
            m_brush->SetColor(ColorLerp(pal.textMuted, pal.accent, std::max(vOn, vHov * 0.7f)));
            icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                            isVis ? icons::IconKind::Eye : icons::IconKind::EyeOff,
                            {visRc.left + 4.0f, visRc.top + 4.0f, visRc.right - 4.0f, visRc.bottom - 4.0f}, 1.5f);

            const D2D1_RECT_F delRc{rrRight - 24.0f, rowY + 3.0f, rrRight - 2.0f, rowY + 25.0f};
            RegisterHit(delId, delRc, true, delArg);
            const float dHov = EaseOutCubic(GetHoverAnim(delId, delArg));
            if (dHov > 0.01f) DrawRoundedBox(delRc, 4.5f, ColorLerp(D2dColor(0, 0, 0, 0), pal.accentTint, dHov), D2dColor(0, 0, 0, 0), 0.0f);
            m_brush->SetColor(ColorLerp(pal.textMuted, pal.accent, dHov));
            icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                            icons::IconKind::Trash,
                            {delRc.left + 4.0f, delRc.top + 4.0f, delRc.right - 4.0f, delRc.bottom - 4.0f}, 1.5f);
        };

        if (m_config.hasCaptureSource) {
            const bool sel = (m_selectedLayer == CanvasLayer::MainSource);
            const D2D1_RECT_F rr{m_overlayListRc.left, ry, m_overlayListRc.right, ry + rowH};
            RegisterHit(ControlId::LayerSelectSource, rr, true);
            const float rSel = AnimSlot(44001, sel ? 1.0f : 0.0f, 18.0f);
            const float rHov = EaseOutCubic(GetHoverAnim(ControlId::LayerSelectSource));
            DrawRoundedBox(rr, 5.0f, ColorLerp(ColorLerp(pal.card, pal.surfaceAlt, rHov), pal.accentTint, rSel),
                           ColorLerp(ColorLerp(pal.border, pal.borderStrong, rHov), pal.accent, rSel), 1.0f);
            const icons::IconKind sIco = (m_config.sourceType == 0) ? icons::IconKind::Monitor
                                       : ((m_config.sourceType == 1) ? icons::IconKind::AppWindow : icons::IconKind::CropRegion);
            m_brush->SetColor(m_config.sourceVisible ? ColorLerp(pal.textSub, pal.accent, rSel) : pal.textMuted);
            icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), sIco,
                            {rr.left + 7.0f, ry + 7.0f, rr.left + 19.0f, ry + 19.0f}, 1.5f);
            std::wstring sLbl = (m_config.sourceType == 0) ? L"Display Capture"
                              : ((m_config.sourceType == 1)
                                    ? (m_config.windowTitleFilter.empty() ? L"Window (Select App)" : (L"Window: " + m_config.windowTitleFilter))
                                    : L"Region Crop");
            DrawTextStrict(sLbl, sel ? m_fmtBold105.Get() : m_fmtSemi10.Get(),
                           {rr.left + 24.0f, ry, rr.right - 52.0f, ry + rowH},
                           m_config.sourceVisible ? pal.text : pal.textMuted);
            drawEyeAndTrash(ry, rr.right, ControlId::ToggleSourceVisBtn, 0, m_config.sourceVisible, ControlId::RemoveSourceBtn, 0);
            ry += rowH + rg;
        }

        if (m_config.camAdded) {
            const bool sel = (m_selectedLayer == CanvasLayer::Camera);
            const D2D1_RECT_F rr{m_overlayListRc.left, ry, m_overlayListRc.right, ry + rowH};
            RegisterHit(ControlId::LayerSelectCamera, rr, true);
            const float rSel = AnimSlot(44002, sel ? 1.0f : 0.0f, 18.0f);
            const float rHov = EaseOutCubic(GetHoverAnim(ControlId::LayerSelectCamera));
            DrawRoundedBox(rr, 5.0f, ColorLerp(ColorLerp(pal.card, pal.surfaceAlt, rHov), pal.accentTint, rSel),
                           ColorLerp(ColorLerp(pal.border, pal.borderStrong, rHov), pal.accent, rSel), 1.0f);
            m_brush->SetColor(m_config.camEnabled ? pal.accent : pal.textMuted);
            icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                            m_config.camEnabled ? icons::IconKind::Camera : icons::IconKind::CameraOff,
                            {rr.left + 7.0f, ry + 7.0f, rr.left + 19.0f, ry + 19.0f}, 1.5f);
            DrawTextStrict(L"Webcam (On-Cam)", sel ? m_fmtBold105.Get() : m_fmtSemi10.Get(),
                           {rr.left + 24.0f, ry, rr.right - 52.0f, ry + rowH},
                           m_config.camEnabled ? pal.text : pal.textMuted);
            drawEyeAndTrash(ry, rr.right, ControlId::ToggleCamBtn, 0, m_config.camEnabled, ControlId::RemoveCamBtn, 0);
            ry += rowH + rg;
        }

        for (const auto& ov : m_config.overlays) {
            if (ry + rowH >= m_overlayListRc.top && ry <= m_overlayListRc.bottom) {
                const bool sel = (m_selectedLayer == CanvasLayer::Overlay && ov.id == m_selectedOverlayId);
                const D2D1_RECT_F rr{m_overlayListRc.left, ry, m_overlayListRc.right, ry + rowH};
                RegisterHit(ControlId::OverlayListSelect, rr, true, static_cast<int>(ov.id));
                const float rSel = AnimSlot(45000 + static_cast<int>(ov.id), sel ? 1.0f : 0.0f, 18.0f);
                const float rHov = EaseOutCubic(GetHoverAnim(ControlId::OverlayListSelect, static_cast<int>(ov.id)));
                DrawRoundedBox(rr, 5.0f, ColorLerp(ColorLerp(pal.card, pal.surfaceAlt, rHov), pal.accentTint, rSel),
                               ColorLerp(ColorLerp(pal.border, pal.borderStrong, rHov), pal.accent, rSel), 1.0f);

                icons::IconKind kIco = icons::IconKind::TextType;
                if (ov.kind == OverlayKind::Clock) kIco = icons::IconKind::Clock;
                else if (ov.kind == OverlayKind::Box) kIco = icons::IconKind::BoxShape;
                else if (ov.kind == OverlayKind::Image) kIco = icons::IconKind::ImagePic;

                m_brush->SetColor(ColorLerp(pal.textSub, pal.accent, rSel));
                icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), kIco,
                                {rr.left + 7.0f, ry + 7.0f, rr.left + 19.0f, ry + 19.0f}, 1.5f);

                const std::wstring lbl = (ov.kind == OverlayKind::Clock) ? L"Live Time" : ov.text;
                DrawTextStrict(lbl, sel ? m_fmtBold105.Get() : m_fmtSemi10.Get(),
                               {rr.left + 24.0f, ry, rr.right - 52.0f, ry + rowH},
                               ov.visible ? pal.text : pal.textMuted);
                drawEyeAndTrash(ry, rr.right, ControlId::OverlayListToggleVis, static_cast<int>(ov.id), ov.visible,
                                ControlId::OverlayListDelete, static_cast<int>(ov.id));
            }
            ry += rowH + rg;
        }
    }
    m_rt->PopAxisAlignedClip();

    const D2D1_RECT_F inspRc{iLeft, listBottom + 8.0f, iRight, panelRc.bottom - 10.0f};
    DrawRoundedBox(inspRc, 7.0f, pal.surface, pal.border, 1.0f);
    float iy = inspRc.top + 9.0f;
    const float ix0 = inspRc.left + 10.0f, ix1 = inspRc.right - 10.0f;

    const float inspEase = EaseOutCubic(m_inspAnim);
    const bool inspAnim = (inspEase < 0.998f);
    D2D1_MATRIX_3X2_F prevInspXform{};
    if (inspAnim) {
        m_rt->GetTransform(&prevInspXform);
        m_rt->PushAxisAlignedClip(inspRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, 10.0f * (1.0f - inspEase)) * prevInspXform);
    }

    if (m_selectedLayer == CanvasLayer::MainSource && m_config.hasCaptureSource) {
        DrawTextStrict(L"SOURCE PROPERTIES", m_fmtBold9.Get(), {ix0, iy, ix0 + 130.0f, iy + 16.0f}, pal.accent);

        const D2D1_RECT_F tabBox{ix1 - 156.0f, iy - 2.0f, ix1, iy + 22.0f};
        DrawSegmentedControl(pal, tabBox, 10006, {
            {ControlId::SrcTabGeneral, tabBox.left + 2.0f, 74.0f, L"Source", static_cast<int>(icons::IconKind::Monitor), m_srcTab == 0},
            {ControlId::SrcTabFilter,  tabBox.left + 78.0f, 74.0f, L"Filters", static_cast<int>(icons::IconKind::Sun), m_srcTab == 1}
        }, true, 6.0f, 4.0f, 9.5f);
        iy += 26.0f;

        const float tabEase = EaseOutCubic(m_srcTabAnim);
        const bool tabAnimActive = (tabEase < 0.998f);
        D2D1_MATRIX_3X2_F prevTabXform{};
        const D2D1_RECT_F tabContentRc{ix0 - 2.0f, iy, ix1 + 2.0f, inspRc.bottom - 4.0f};
        if (tabAnimActive) {
            m_rt->GetTransform(&prevTabXform);
            m_rt->PushAxisAlignedClip(tabContentRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const float slideY = (1.0f - tabEase) * (m_srcTab == 0 ? -6.0f : 6.0f);
            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, slideY) * prevTabXform);
        }

        if (m_srcTab == 0) {
            DrawTextStrict(L"Cursor", m_fmtSemi10.Get(), {ix0, iy, ix0 + 60.0f, iy + 18.0f}, pal.textSub);
            DrawToggleSwitch(pal, ControlId::ToggleShowCursor, ix1, iy + 8.0f, m_config.showCursor);
            iy += 22.0f;

            const D2D1_RECT_F modeBox{ix0, iy, ix1, iy + 25.0f};
            const float mW = ((ix1 - ix0) - 10.0f) / 3.0f;
            const float mx0 = ix0 + 3.0f;
            DrawSegmentedControl(pal, modeBox, 10003, {
                {ControlId::SrcTypeDisplay, mx0,                mW, L"Display", static_cast<int>(icons::IconKind::Monitor),    m_config.sourceType == 0},
                {ControlId::SrcTypeWindow,  mx0 + (mW + 2.0f),  mW, L"Window",  static_cast<int>(icons::IconKind::AppWindow),  m_config.sourceType == 1},
                {ControlId::SrcTypeRegion,  mx0 + (mW + 2.0f)*2,mW, L"Region",  static_cast<int>(icons::IconKind::CropRegion), m_config.sourceType == 2}
            }, true, 6.0f, 4.5f, 10.0f);
            iy += 31.0f;

            if (m_config.sourceType != 2) {
                const float pRight = (m_config.sourceType == 1) ? (ix1 - 28.0f) : ix1;
                const D2D1_RECT_F pickRc{ix0, iy, pRight, iy + 25.0f};
                const bool needApp = (m_config.sourceType == 1 && m_config.windowTitleFilter.empty());
                std::wstring lbl = L"Default Display";
                if (m_config.sourceType == 0 && !m_monitors.empty()) {
                    const size_t idx = std::min<size_t>(static_cast<size_t>(std::max(0, m_config.monitorIndex)), m_monitors.size() - 1);
                    lbl = m_monitors[idx].label;
                } else if (m_config.sourceType == 1) {
                    lbl = needApp ? L"Select Application Window..." : m_config.windowTitleFilter;
                }
                DrawAnimatedPickerBox(pal, ControlId::SourcePickerBtn, pickRc, lbl, PopupKind::SourcePicker, true, needApp, 6.0f);
                if (m_config.sourceType == 1) {
                    const D2D1_RECT_F refRc{ix1 - 24.0f, iy, ix1, iy + 25.0f};
                    RegisterHit(ControlId::RefreshWindowsBtn, refRc, true);
                    const float rHov = EaseOutCubic(GetHoverAnim(ControlId::RefreshWindowsBtn));
                    DrawRoundedBox(refRc, 6.0f, ColorLerp(pal.card, pal.accentTint, rHov),
                                   ColorLerp(pal.borderStrong, pal.accent, rHov), 1.0f);
                    m_brush->SetColor(ColorLerp(pal.textSub, pal.accent, rHov));
                    const float rSpin = (m_spinningRefreshId == ControlId::RefreshWindowsBtn) ? m_refreshSpinAngle : 0.0f;
                    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::Refresh,
                                    {refRc.left + 5.0f, iy + 5.0f, refRc.right - 5.0f, iy + 20.0f}, 1.5f, rSpin);
                }
                iy += 32.0f;
            } else {
                const float c4 = ((ix1 - ix0) - 15.0f) * 0.25f;
                auto drawCropBox = [&](float x0, const wchar_t* tag, ControlId cid, EditFieldId fid, int val) {
                    DrawTextStrict(tag, m_fmtBold9.Get(), {x0, iy, x0 + 14.0f, iy + 22.0f}, pal.textMuted);
                    DrawInlineInputBox(pal, cid, fid, {x0 + 14.0f, iy, x0 + c4, iy + 22.0f}, std::to_wstring(val));
                };
                drawCropBox(ix0,                L"X", ControlId::InputCropX, EditFieldId::CropX, m_config.cropX);
                drawCropBox(ix0 + (c4 + 5.0f),  L"Y", ControlId::InputCropY, EditFieldId::CropY, m_config.cropY);
                drawCropBox(ix0 + (c4 + 5.0f)*2,L"W", ControlId::InputCropW, EditFieldId::CropW, m_config.cropW);
                drawCropBox(ix0 + (c4 + 5.0f)*3,L"H", ControlId::InputCropH, EditFieldId::CropH, m_config.cropH);
                iy += 28.0f;
            }

            m_brush->SetColor(pal.divider);
            m_rt->DrawLine({ix0, iy}, {ix1, iy}, m_brush.Get(), 1.0f);
            iy += 7.0f;

            const float qW = ((ix1 - ix0) - 18.0f) * 0.25f;
            auto drawPct = [&](float x0, const wchar_t* tag, ControlId cid, EditFieldId fid, float valNorm) {
                DrawTextStrict(tag, m_fmtBold9.Get(), {x0, iy, x0 + 18.0f, iy + 23.0f}, pal.textMuted);
                DrawInlineInputBox(pal, cid, fid, {x0 + 18.0f, iy, x0 + qW, iy + 23.0f},
                                   std::to_wstring(static_cast<int>(std::lround(valNorm * 100.0f))));
            };
            drawPct(ix0,                   L"X%", ControlId::InputSrcX, EditFieldId::SrcX, m_config.srcPosX);
            drawPct(ix0 + (qW + 6.0f),     L"Y%", ControlId::InputSrcY, EditFieldId::SrcY, m_config.srcPosY);
            drawPct(ix0 + (qW + 6.0f) * 2, L"W%", ControlId::InputSrcW, EditFieldId::SrcW, m_config.srcScaleW);
            drawPct(ix0 + (qW + 6.0f) * 3, L"H%", ControlId::InputSrcH, EditFieldId::SrcH, m_config.srcScaleH);
            iy += 29.0f;

            DrawTextStrict(L"Layout", m_fmtSemi10.Get(), {ix0, iy, ix0 + 44.0f, iy + 23.0f}, pal.textSub);
            const float lW = ((ix1 - (ix0 + 46.0f)) - 12.0f) * 0.25f;
            float lx = ix0 + 46.0f;
            DrawButton(pal, ControlId::SrcFitBtn,    {lx, iy, lx + lW, iy + 23.0f}, L"Fit",    false, m_config.srcKeepAspect && m_config.srcScaleW > 0.98f, true, 9.5f, true, 6.0f, 2); lx += lW + 4.0f;
            DrawButton(pal, ControlId::SrcFillBtn,   {lx, iy, lx + lW, iy + 23.0f}, L"Fill",   false, !m_config.srcKeepAspect, true, 9.5f, true, 6.0f, 2); lx += lW + 4.0f;
            DrawButton(pal, ControlId::SrcCenterBtn, {lx, iy, lx + lW, iy + 23.0f}, L"Center", false, false, true, 9.5f, true, 6.0f, 2); lx += lW + 4.0f;
            DrawButton(pal, ControlId::SrcPipBtn,    {lx, iy, ix1,     iy + 23.0f}, L"75%",    false, m_config.srcScaleW < 0.85f, true, 9.5f, true, 6.0f, 2);
            iy += 29.0f;

            DrawTextStrict(L"Lock Aspect Ratio", m_fmtSemi10.Get(), {ix0, iy, ix1 - 46.0f, iy + 22.0f}, pal.textSub);
            DrawToggleSwitch(pal, ControlId::SrcKeepAspectToggle, ix1, iy + 11.0f, m_config.srcKeepAspect);
        } else {

            DrawTextStrict(L"COLOR CORRECTION", m_fmtBold9.Get(), {ix0, iy, ix1 - 60.0f, iy + 16.0f}, pal.text);
            DrawTextStrict(m_config.colorFilterEnabled ? L"Active" : L"Off", m_fmtSemi10.Get(),
                           {ix1 - 92.0f, iy, ix1 - 42.0f, iy + 16.0f},
                           m_config.colorFilterEnabled ? pal.accent : pal.textMuted,
                           DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawToggleSwitch(pal, ControlId::ToggleColorFilter, ix1, iy + 8.0f, m_config.colorFilterEnabled);
            iy += 26.0f;

            const bool fEn = m_config.colorFilterEnabled;

            {
                wchar_t sBuf[24]{};
                const int sPct = static_cast<int>(std::lround(m_config.filterSaturation * 100.0f));
                std::swprintf(sBuf, 24, L"%+d%%", sPct);
                DrawTextStrict(L"Saturation", m_fmtSemi10.Get(), {ix0, iy, ix0 + 90.0f, iy + 17.0f}, fEn ? pal.text : pal.textMuted);
                DrawTextStrict(sBuf, m_fmtBold9.Get(), {ix1 - 50.0f, iy, ix1, iy + 17.0f}, fEn ? pal.accent : pal.textMuted,
                               DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                iy += 19.0f;
                DrawRedSlider(pal, ControlId::FilterSaturationSlider, ix0, ix1, iy + 6.0f,
                              m_config.filterSaturation, -1.0, 2.0, 0.02, fEn);
                iy += 25.0f;
            }

            {
                wchar_t gBuf[24]{};
                std::swprintf(gBuf, 24, L"%+.2f", m_config.filterGamma);
                DrawTextStrict(L"Gamma", m_fmtSemi10.Get(), {ix0, iy, ix0 + 90.0f, iy + 17.0f}, fEn ? pal.text : pal.textMuted);
                DrawTextStrict(gBuf, m_fmtBold9.Get(), {ix1 - 50.0f, iy, ix1, iy + 17.0f}, fEn ? pal.accent : pal.textMuted,
                               DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                iy += 19.0f;
                DrawRedSlider(pal, ControlId::FilterGammaSlider, ix0, ix1, iy + 6.0f,
                              m_config.filterGamma, -1.0, 1.0, 0.02, fEn);
                iy += 25.0f;
            }

            {
                wchar_t cBuf[24]{};
                const int cPct = static_cast<int>(std::lround(m_config.filterContrast * 100.0f));
                std::swprintf(cBuf, 24, L"%+d%%", cPct);
                DrawTextStrict(L"Contrast", m_fmtSemi10.Get(), {ix0, iy, ix0 + 90.0f, iy + 17.0f}, fEn ? pal.text : pal.textMuted);
                DrawTextStrict(cBuf, m_fmtBold9.Get(), {ix1 - 50.0f, iy, ix1, iy + 17.0f}, fEn ? pal.accent : pal.textMuted,
                               DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                iy += 19.0f;
                DrawRedSlider(pal, ControlId::FilterContrastSlider, ix0, ix1, iy + 6.0f,
                              m_config.filterContrast, -1.0, 1.0, 0.02, fEn);
                iy += 25.0f;
            }

            {
                wchar_t bBuf[24]{};
                const int bPct = static_cast<int>(std::lround(m_config.filterBrightness * 100.0f));
                std::swprintf(bBuf, 24, L"%+d%%", bPct);
                DrawTextStrict(L"Brightness", m_fmtSemi10.Get(), {ix0, iy, ix0 + 90.0f, iy + 17.0f}, fEn ? pal.text : pal.textMuted);
                DrawTextStrict(bBuf, m_fmtBold9.Get(), {ix1 - 50.0f, iy, ix1, iy + 17.0f}, fEn ? pal.accent : pal.textMuted,
                               DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                iy += 19.0f;
                DrawRedSlider(pal, ControlId::FilterBrightnessSlider, ix0, ix1, iy + 6.0f,
                              m_config.filterBrightness, -1.0, 1.0, 0.02, fEn);
                iy += 27.0f;
            }

            const D2D1_RECT_F rstRc{ix0, iy, ix1, iy + 25.0f};
            DrawButton(pal, ControlId::FilterResetBtn, rstRc, L"Reset Filters to Defaults", false, false, fEn, 9.5f, true, 6.0f);
        }

        if (tabAnimActive) {
            m_rt->SetTransform(prevTabXform);
            m_rt->PopAxisAlignedClip();
        }
    } else if (m_selectedLayer == CanvasLayer::Camera && m_config.camAdded) {
        DrawTextStrict(L"ON-CAM WEBCAM SETTINGS", m_fmtBold9.Get(), {ix0, iy, ix1 - 46.0f, iy + 16.0f}, pal.accent);
        DrawToggleSwitch(pal, ControlId::ToggleCamBtn, ix1, iy + 8.0f, m_config.camEnabled);
        iy += 22.0f;

        const D2D1_RECT_F camPickRc{ix0, iy, ix1 - 28.0f, iy + 25.0f};
        std::wstring camLabel = m_cameras.empty() ? L"No Webcam Detected" : m_cameras[0].name;
        if (!m_config.camDeviceId.empty()) {
            for (const auto& c : m_cameras) {
                if (_wcsicmp(c.id.c_str(), m_config.camDeviceId.c_str()) == 0) {
                    camLabel = c.name;
                    break;
                }
            }
        }
        DrawAnimatedPickerBox(pal, ControlId::CamPickerBtn, camPickRc, camLabel, PopupKind::CameraPicker, !m_cameras.empty(), false, 6.0f);
        const D2D1_RECT_F refCamRc{ix1 - 24.0f, iy, ix1, iy + 25.0f};
        RegisterHit(ControlId::RefreshCamerasBtn, refCamRc, true);
        const float rcHov = EaseOutCubic(GetHoverAnim(ControlId::RefreshCamerasBtn));
        DrawRoundedBox(refCamRc, 6.0f, ColorLerp(pal.card, pal.accentTint, rcHov), ColorLerp(pal.borderStrong, pal.accent, rcHov), 1.0f);
        m_brush->SetColor(ColorLerp(pal.textSub, pal.accent, rcHov));
        const float rcSpin = (m_spinningRefreshId == ControlId::RefreshCamerasBtn) ? m_refreshSpinAngle : 0.0f;
        icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::Refresh,
                        {refCamRc.left + 5.0f, iy + 5.0f, refCamRc.right - 5.0f, iy + 20.0f}, 1.5f, rcSpin);
        iy += 31.0f;

        DrawTextStrict(L"Corner", m_fmtSemi10.Get(), {ix0, iy, ix0 + 46.0f, iy + 23.0f}, pal.textSub);
        const float cW = ((ix1 - (ix0 + 48.0f)) - 12.0f) * 0.25f;
        float cx = ix0 + 48.0f;
        DrawButton(pal, ControlId::CamPosTL, {cx, iy, cx + cW, iy + 23.0f}, L"TL", false, m_config.camXNorm < 0.4f && m_config.camYNorm < 0.4f, true, 9.5f); cx += cW + 4.0f;
        DrawButton(pal, ControlId::CamPosTR, {cx, iy, cx + cW, iy + 23.0f}, L"TR", false, m_config.camXNorm >= 0.4f && m_config.camYNorm < 0.4f, true, 9.5f); cx += cW + 4.0f;
        DrawButton(pal, ControlId::CamPosBL, {cx, iy, cx + cW, iy + 23.0f}, L"BL", false, m_config.camXNorm < 0.4f && m_config.camYNorm >= 0.4f, true, 9.5f); cx += cW + 4.0f;
        DrawButton(pal, ControlId::CamPosBR, {cx, iy, ix1,     iy + 23.0f}, L"BR", false, m_config.camXNorm >= 0.4f && m_config.camYNorm >= 0.4f, true, 9.5f);
        iy += 29.0f;

        DrawTextStrict(L"Shape", m_fmtSemi10.Get(), {ix0, iy, ix0 + 46.0f, iy + 23.0f}, pal.textSub);
        const float sW = ((ix1 - (ix0 + 48.0f)) - 8.0f) / 3.0f;
        float sx = ix0 + 48.0f;
        DrawButton(pal, ControlId::CamShapeRect,   {sx, iy, sx + sW, iy + 23.0f}, L"Rect",   false, m_config.camShape == 0, true, 9.5f); sx += sW + 4.0f;
        DrawButton(pal, ControlId::CamShapeRound,  {sx, iy, sx + sW, iy + 23.0f}, L"Round",  false, m_config.camShape == 1, true, 9.5f); sx += sW + 4.0f;
        DrawButton(pal, ControlId::CamShapeCircle, {sx, iy, ix1,     iy + 23.0f}, L"Circle", false, m_config.camShape == 2, true, 9.5f);
        iy += 29.0f;

        DrawTextStrict(L"Width", m_fmtSemi10.Get(), {ix0, iy, ix0 + 46.0f, iy + 23.0f}, pal.textSub);
        DrawButton(pal, ControlId::CamSizeMinusBtn, {ix0 + 48.0f, iy, ix0 + 76.0f, iy + 23.0f}, L"-", false, false);
        DrawTextStrict(std::to_wstring(static_cast<int>(std::lround(m_config.camWidthNorm * 100.0f))) + L"%",
                       m_fmtBold105.Get(), {ix0 + 78.0f, iy, ix0 + 120.0f, iy + 23.0f}, pal.text,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawButton(pal, ControlId::CamSizePlusBtn, {ix0 + 122.0f, iy, ix0 + 150.0f, iy + 23.0f}, L"+", false, false);
        iy += 30.0f;

        const float midX = (ix0 + ix1) * 0.5f;
        DrawTextStrict(L"Mirror", m_fmtSemi10.Get(), {ix0, iy, midX - 44.0f, iy + 22.0f}, pal.textSub);
        DrawToggleSwitch(pal, ControlId::CamMirrorToggle, midX - 4.0f, iy + 11.0f, m_config.camMirror);
        DrawTextStrict(L"Border", m_fmtSemi10.Get(), {midX + 6.0f, iy, ix1 - 42.0f, iy + 22.0f}, pal.textSub);
        DrawToggleSwitch(pal, ControlId::CamBorderToggle, ix1, iy + 11.0f, m_config.camBorder);
    } else if (m_selectedLayer == CanvasLayer::Overlay && FindSelectedOverlay() != nullptr) {
        OverlayItem* sel = FindSelectedOverlay();
        DrawTextStrict(L"COMPONENT PROPERTIES", m_fmtBold9.Get(), {ix0, iy, ix1 - 70.0f, iy + 14.0f}, pal.accent);
        wchar_t posBuf[32]{};
        std::swprintf(posBuf, 32, L"X %d%% \u00B7 Y %d%%",
                      static_cast<int>(std::lround(sel->xNorm * 100.0f)),
                      static_cast<int>(std::lround(sel->yNorm * 100.0f)));
        DrawTextStrict(posBuf, m_fmtReg9.Get(), {ix1 - 80.0f, iy, ix1, iy + 14.0f}, pal.textMuted,
                       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        iy += 22.0f;

        if (sel->kind == OverlayKind::Text) {
            DrawTextStrict(L"Text", m_fmtSemi10.Get(), {ix0, iy, ix0 + 38.0f, iy + 26.0f}, pal.textSub);
            DrawInlineInputBox(pal, ControlId::InputOverlayText, EditFieldId::OverlayText,
                               {ix0 + 40.0f, iy, ix1, iy + 26.0f}, sel->text);
        } else if (sel->kind == OverlayKind::Image) {
            DrawButton(pal, ControlId::OverlayBrowseImgBtn, {ix0, iy, ix1, iy + 26.0f},
                       L"Change Image (" + sel->text + L")", false, false, true, 10.0f, false, 5.0f);
        } else {
            DrawTextStrict(sel->kind == OverlayKind::Clock ? L"Format: YYYY-MM-DD HH:MM:SS" : L"Drag frame or handle on Preview to resize",
                           m_fmtReg105.Get(), {ix0, iy, ix1, iy + 26.0f}, pal.textSub);
        }
        iy += 34.0f;

        DrawTextStrict(L"Size", m_fmtSemi10.Get(), {ix0, iy, ix0 + 50.0f, iy + 24.0f}, pal.textSub);
        DrawButton(pal, ControlId::OverlaySizeMinusBtn, {ix1 - 116.0f, iy, ix1 - 90.0f, iy + 24.0f}, L"-", false, false);
        wchar_t szStr[24]{};
        if (sel->kind == OverlayKind::Text || sel->kind == OverlayKind::Clock) {
            std::swprintf(szStr, 24, L"%d pt", sel->fontSize);
        } else {
            std::swprintf(szStr, 24, L"%d%% W", static_cast<int>(std::lround(sel->boxWidthNorm * 100.0f)));
        }
        DrawTextStrict(szStr, m_fmtBold105.Get(), {ix1 - 88.0f, iy, ix1 - 28.0f, iy + 24.0f}, pal.text,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawButton(pal, ControlId::OverlaySizePlusBtn, {ix1 - 26.0f, iy, ix1, iy + 24.0f}, L"+", false, false);
        iy += 32.0f;

        DrawTextStrict(L"Color", m_fmtSemi10.Get(), {ix0, iy, ix0 + 50.0f, iy + 22.0f}, pal.textSub);
        if (sel->kind != OverlayKind::Image) {
            float sx = ix1 - 114.0f;
            for (int cIdx = 0; cIdx < 5; ++cIdx) {
                const D2D1_RECT_F sRc{sx, iy + 2.0f, sx + 18.0f, iy + 20.0f};
                RegisterHit(ControlId::OverlayColorBtn, sRc, true, cIdx);
                const bool activeClr = (sel->colorArgb == kOverlayColors[static_cast<size_t>(cIdx)]);
                const float cHov = EaseOutCubic(GetHoverAnim(ControlId::OverlayColorBtn, cIdx));
                const float cSel = AnimSlot(48000 + cIdx, activeClr ? 1.0f : 0.0f, 18.0f);
                const float ringAmt = std::max(cSel, cHov * 0.6f);
                m_brush->SetColor(ArgbToD2d(kOverlayColors[static_cast<size_t>(cIdx)]));
                m_rt->FillEllipse(D2D1::Ellipse({sx + 9.0f, iy + 11.0f}, 7.2f + 0.6f * cHov, 7.2f + 0.6f * cHov), m_brush.Get());
                m_brush->SetColor(ColorLerp(pal.borderStrong, pal.accent, ringAmt));
                m_rt->DrawEllipse(D2D1::Ellipse({sx + 9.0f, iy + 11.0f}, 7.8f + 1.5f * ringAmt, 7.8f + 1.5f * ringAmt),
                                  m_brush.Get(), 1.0f + 1.0f * cSel);
                sx += 24.0f;
            }
        }
        iy += 30.0f;

        if (sel->kind != OverlayKind::Image) {
            DrawTextStrict(L"Backdrop Fill", m_fmtSemi10.Get(), {ix0, iy, ix1 - 46.0f, iy + 22.0f}, pal.textSub);
            DrawToggleSwitch(pal, ControlId::OverlayBgToggleBtn, ix1, iy + 11.0f, sel->bgEnabled);
        }
    } else {
        DrawTextStrict(L"Select a Display/Window or Component above to configure its properties.",
                       m_fmtReg105.Get(), inspRc, pal.textMuted,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    }
    if (inspAnim) {
        m_rt->SetTransform(prevInspXform);
        m_rt->PopAxisAlignedClip();
    }
}

void AppWindow::DrawDockAudioMixer(const ThemePalette& pal, const D2D1_RECT_F& dockRc) {
    DrawRoundedBox(dockRc, 10.0f, pal.card, pal.border, 1.0f);
    const float iLeft = dockRc.left + 12.0f;
    const float iRight = dockRc.right - 12.0f;
    DrawTextStrict(L"AUDIO MIXER", m_fmtBold9.Get(), {iLeft, dockRc.top + 8.0f, iRight - 80.0f, dockRc.top + 22.0f}, pal.textMuted);
    DrawTextStrict(std::to_wstring(m_config.audioBitrateKbps) + L" kbps AAC", m_fmtReg9.Get(),
                   {iRight - 90.0f, dockRc.top + 8.0f, iRight, dockRc.top + 22.0f}, pal.textMuted,
                   DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    auto resolveAudioName = [](const std::vector<AudioDeviceInfo>& list, const std::wstring& id, const wchar_t* defLbl) -> std::wstring {
        if (id.empty()) {
            for (const auto& d : list) if (d.isDefault) return d.name;
            return list.empty() ? std::wstring(defLbl) : list.front().name;
        }
        if (_wcsicmp(id.c_str(), L"none") == 0) return L"Mic Off (Disabled)";
        for (const auto& d : list) if (d.id == id) return d.name;
        return defLbl;
    };

    auto drawCh = [&](float topY, ControlId muteId, icons::IconKind icoOn, icons::IconKind icoOff,
                      bool muted, ControlId devBtnId, PopupKind devPopKind, const std::wstring& devName,
                      ControlId sldId, int vol, float meter, float peak, bool avail) {
        const D2D1_RECT_F mRc{iLeft, topY, iLeft + 23.0f, topY + 23.0f};
        RegisterHit(muteId, mRc, avail);
        const float mHov = avail ? EaseOutCubic(GetHoverAnim(muteId)) : 0.0f;
        const float mOn  = AnimSlot(42000 + static_cast<int>(muteId), muted ? 1.0f : 0.0f, 18.0f);
        DrawRoundedBox(mRc, 5.0f, ColorLerp(ColorLerp(pal.surface, pal.surfaceAlt, mHov), pal.accentTint, mOn),
                       ColorLerp(ColorLerp(pal.border, pal.borderStrong, mHov), pal.accentBorder, mOn), 1.0f);
        m_brush->SetColor(avail ? ColorLerp(ColorLerp(pal.text, pal.accent, mHov * 0.55f), pal.accent, mOn) : pal.textMuted);
        icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                        muted ? icoOff : icoOn, {mRc.left + 4.0f, mRc.top + 4.0f, mRc.right - 4.0f, mRc.bottom - 4.0f}, 1.5f);

        const D2D1_RECT_F pickRc{iLeft + 27.0f, topY, iRight - 42.0f, topY + 23.0f};
        DrawAnimatedPickerBox(pal, devBtnId, pickRc, devName, devPopKind, true, false, 5.0f);

        wchar_t vb[16]{};
        if (!avail) wcscpy_s(vb, L"Off"); else if (muted) wcscpy_s(vb, L"Muted"); else std::swprintf(vb, 16, L"%d%%", vol);
        DrawTextStrict(vb, m_fmtBold105.Get(), {iRight - 40.0f, topY, iRight, topY + 23.0f},
                       (muted || !avail) ? pal.textMuted : pal.accent,
                       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        DrawVuMeter(pal, {iLeft, topY + 27.0f, iRight, topY + 33.5f}, meter, peak, muted || !avail);
        DrawRedSlider(pal, sldId, iLeft, iRight, topY + 45.0f, static_cast<double>(vol), 0.0, 100.0, 2.0, avail);
    };

    drawCh(dockRc.top + 28.0f,
           ControlId::DeskMuteBtn, icons::IconKind::Speaker, icons::IconKind::SpeakerMuted,
           m_config.desktopAudioMuted, ControlId::DeskDeviceBtn, PopupKind::DesktopAudioPicker,
           resolveAudioName(m_outAudioDevs, m_config.desktopDeviceId, L"Desktop (Default Out)"),
           ControlId::DeskVolSlider, m_config.desktopVolume, m_uiDeskMeter, m_uiDeskPeak, true);

    m_brush->SetColor(pal.divider);
    m_rt->DrawLine({iLeft, dockRc.top + 89.0f}, {iRight, dockRc.top + 89.0f}, m_brush.Get(), 1.0f);

    const bool micOn = (_wcsicmp(m_config.micDeviceId.c_str(), L"none") != 0);
    drawCh(dockRc.top + 97.0f,
           ControlId::MicMuteBtn, icons::IconKind::Microphone, icons::IconKind::MicMuted,
           m_config.micMuted, ControlId::MicDeviceBtn, PopupKind::MicAudioPicker,
           resolveAudioName(m_inAudioDevs, m_config.micDeviceId, L"Select Microphone..."),
           ControlId::MicVolSlider, m_config.micVolume, m_uiMicMeter, m_uiMicPeak, micOn);
}

void AppWindow::DrawDockOutputAndEncoder(const ThemePalette& pal, const D2D1_RECT_F& dockRc) {
    DrawRoundedBox(dockRc, 10.0f, pal.card, pal.border, 1.0f);
    const float iLeft = dockRc.left + 12.0f;
    const float iRight = dockRc.right - 12.0f;
    float y = dockRc.top + 8.0f;

    const RecorderStats st = m_recorder.GetStats();
    const bool lockVideo = st.isRecording;

    DrawTextStrict(L"VIDEO & ENCODER", m_fmtBold9.Get(), {iLeft, y, iRight, y + 14.0f}, pal.textMuted);
    y += 18.0f;

    const DetectedEncoder activeEnc = EncoderManager::ResolveSelection(m_encoders, m_config.encoderId);
    const EncoderPreset activePreset = EncoderManager::ResolvePreset(activeEnc, m_config.encoderPreset);

    DrawTextStrict(L"Encoder", m_fmtSemi10.Get(), {iLeft, y, iLeft + 52.0f, y + 24.0f}, pal.textSub);
    const D2D1_RECT_F encRc{iLeft + 54.0f, y, iRight - 28.0f, y + 24.0f};
    std::wstring encTxt = (m_config.encoderId == L"auto")
        ? (L"Auto \u00B7 " + activeEnc.friendlyName)
        : (activeEnc.friendlyName + L" [" + activeEnc.badge + L"]");
    DrawAnimatedPickerBox(pal, ControlId::EncoderPickerBtn, encRc, encTxt, PopupKind::EncoderPicker, !lockVideo, false, 6.0f);

    const D2D1_RECT_F refEncRc{iRight - 24.0f, y, iRight, y + 24.0f};
    RegisterHit(ControlId::RefreshEncodersBtn, refEncRc, true);
    const float reHov = EaseOutCubic(GetHoverAnim(ControlId::RefreshEncodersBtn));
    DrawRoundedBox(refEncRc, 6.0f, ColorLerp(pal.surface, pal.accentTint, reHov),
                   ColorLerp(pal.borderStrong, pal.accent, reHov), 1.0f);
    m_brush->SetColor(ColorLerp(pal.textSub, pal.accent, reHov));
    const float reSpin = (m_spinningRefreshId == ControlId::RefreshEncodersBtn) ? m_refreshSpinAngle : 0.0f;
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::Refresh,
                    {refEncRc.left + 5.0f, y + 5.0f, refEncRc.right - 5.0f, y + 19.0f}, 1.5f, reSpin);
    y += 30.0f;

    DrawTextStrict(L"Preset", m_fmtSemi10.Get(), {iLeft, y, iLeft + 52.0f, y + 24.0f}, pal.textSub);
    const D2D1_RECT_F preRc{iLeft + 54.0f, y, iRight, y + 24.0f};
    DrawAnimatedPickerBox(pal, ControlId::PresetPickerBtn, preRc, activePreset.name, PopupKind::EncoderPresetPicker, !lockVideo, false, 6.0f);
    y += 30.0f;

    uint32_t outW = 1920, outH = 1080;
    m_recorder.ResolveOutputDimensions(m_config, outW, outH);
    DrawTextStrict(L"Canvas", m_fmtSemi10.Get(), {iLeft, y, iLeft + 52.0f, y + 24.0f}, pal.textSub);

    const float fpsW = 68.0f;
    const float resDropW = std::clamp((iRight - (iLeft + 54.0f)) * 0.35f, 105.0f, 140.0f);
    const D2D1_RECT_F resPick{iLeft + 54.0f, y, iLeft + 54.0f + resDropW, y + 24.0f};
    DrawAnimatedPickerBox(pal, ControlId::ResPickerBtn, resPick, ResolutionShortLabel(m_config, outW, outH),
                          PopupKind::ResPicker, !lockVideo, false, 6.0f);

    const float dimStartX = resPick.right + 6.0f;
    const float dimAvail = (iRight - fpsW - 6.0f) - dimStartX;
    const float numW = std::max(42.0f, (dimAvail - 14.0f) * 0.5f);
    DrawInlineInputBox(pal, ControlId::InputOutWidth, EditFieldId::OutWidth,
                       {dimStartX, y, dimStartX + numW, y + 24.0f}, std::to_wstring(outW), !lockVideo);
    DrawTextStrict(L"\u00D7", m_fmtSemi10.Get(), {dimStartX + numW, y, dimStartX + numW + 14.0f, y + 24.0f},
                   pal.textMuted, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DrawInlineInputBox(pal, ControlId::InputOutHeight, EditFieldId::OutHeight,
                       {dimStartX + numW + 14.0f, y, iRight - fpsW - 6.0f, y + 24.0f}, std::to_wstring(outH), !lockVideo);
    DrawButton(pal, ControlId::CycleFpsBtn, {iRight - fpsW, y, iRight, y + 24.0f},
               std::to_wstring(m_config.fps) + L" FPS", false, false, !lockVideo, 10.0f, true, 6.0f);
    y += 31.0f;

    DrawTextStrict(L"Bitrate", m_fmtSemi10.Get(), {iLeft, y, iLeft + 52.0f, y + 24.0f}, pal.textSub);
    const D2D1_RECT_F brBox{iLeft + 54.0f, y, iRight, y + 24.0f};
    const float bw4 = std::floor((brBox.right - brBox.left - 6.0f) * 0.25f);
    const float bx0 = brBox.left + 2.0f;
    DrawSegmentedControl(pal, brBox, 10005, {
        {ControlId::Bitrate8Btn,  bx0,                     bw4, L"8M",  -1, m_config.bitrateMbps <= 12},
        {ControlId::Bitrate18Btn, bx0 + (bw4 + 1.0f),      bw4, L"18M", -1, m_config.bitrateMbps > 12 && m_config.bitrateMbps <= 22},
        {ControlId::Bitrate28Btn, bx0 + (bw4 + 1.0f)*2.0f, bw4, L"28M", -1, m_config.bitrateMbps > 22 && m_config.bitrateMbps <= 36},
        {ControlId::Bitrate45Btn, bx0 + (bw4 + 1.0f)*3.0f, bw4, L"45M", -1, m_config.bitrateMbps > 36}
    }, !lockVideo, 6.0f, 4.5f, 9.5f);
}

void AppWindow::DrawDockControls(const ThemePalette& pal, const D2D1_RECT_F& dockRc) {
    DrawRoundedBox(dockRc, 10.0f, pal.card, pal.border, 1.0f);
    const float iLeft = dockRc.left + 12.0f;
    const float iRight = dockRc.right - 12.0f;
    float y = dockRc.top + 8.0f;

    const RecorderStats st = m_recorder.GetStats();
    const DetectedEncoder activeEnc = EncoderManager::ResolveSelection(m_encoders, m_config.encoderId);
    const EncoderPreset activePreset = EncoderManager::ResolvePreset(activeEnc, m_config.encoderPreset);
    uint32_t outW = 1920, outH = 1080;
    m_recorder.ResolveOutputDimensions(m_config, outW, outH);

    DrawTextStrict(L"CONTROLS", m_fmtBold9.Get(), {iLeft, y, iRight - 56.0f, y + 14.0f}, pal.textMuted);
    const D2D1_RECT_F hkBadge{iRight - 46.0f, y - 1.0f, iRight, y + 15.0f};
    DrawRoundedBox(hkBadge, 4.0f, pal.surfaceAlt, pal.border, 1.0f);
    DrawTextStrict(m_config.recordHotkey, m_fmtBold9.Get(), hkBadge, pal.textSub,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    y += 18.0f;

    const D2D1_RECT_F statRc{iLeft, y, iRight, y + 44.0f};
    DrawRoundedBox(statRc, 7.0f, pal.surface, pal.border, 1.0f);
    DrawTextStrict(FormatDurationHms(st.elapsedSec), m_fmtBold145.Get(),
                   {statRc.left + 10.0f, y + 3.0f, statRc.left + 106.0f, y + 25.0f},
                   st.isRecording ? pal.accent : pal.text);
    const std::wstring idleMeta = activeEnc.friendlyName + L" \u00B7 " + activePreset.shortLabel;
    DrawTextStrict(st.isRecording ? st.encoderLabel : idleMeta, m_fmtReg9.Get(),
                   {statRc.left + 10.0f, y + 24.0f, statRc.right - 84.0f, y + 40.0f}, pal.textMuted);

    wchar_t rightMeta[48]{};
    if (st.isRecording) {
        std::swprintf(rightMeta, 48, L"%.0f FPS\n%s",
                      st.actualFps > 0.0 ? st.actualFps : static_cast<double>(m_config.fps),
                      FormatBytesHuman(st.fileBytes).c_str());
    } else {
        std::swprintf(rightMeta, 48, L"%ux%u\n%dM \u00B7 %d FPS", outW, outH, m_config.bitrateMbps, m_config.fps);
    }
    DrawTextStrict(rightMeta, m_fmtBold9.Get(), {statRc.right - 96.0f, y + 3.0f, statRc.right - 10.0f, y + 41.0f},
                   pal.textSub, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    y += 51.0f;

    const float btnH = 40.0f;
    if (!st.isRecording) {
        const D2D1_RECT_F recBtn{iLeft, y, iRight, y + btnH};
        RegisterHit(ControlId::RecordToggleBtn, recBtn, true);
        const float rHov = EaseOutCubic(GetHoverAnim(ControlId::RecordToggleBtn));
        const bool recPrs = IsControlPressed(ControlId::RecordToggleBtn);
        const float recPrsAmt = AnimSlot(38001, recPrs ? 1.0f : 0.0f, 32.0f);

        if (rHov > 0.01f) {
            D2D1_COLOR_F haloCol = pal.accent;
            haloCol.a = 0.22f * rHov;
            const float haloPad = 2.5f * rHov;
            DrawRoundedBox({recBtn.left - haloPad, recBtn.top - haloPad, recBtn.right + haloPad, recBtn.bottom + haloPad},
                           8.5f, haloCol, D2dColor(0, 0, 0, 0), 0.0f);
        }

        D2D1_RECT_F drawBtnRc = recBtn;
        if (recPrsAmt > 0.02f) {
            drawBtnRc = {recBtn.left + 1.0f * recPrsAmt, recBtn.top + 1.5f * recPrsAmt,
                         recBtn.right - 1.0f * recPrsAmt, recBtn.bottom - 0.5f * recPrsAmt};
        }

        const D2D1_COLOR_F btnBg = recPrs
            ? pal.accentPress
            : ColorLerp(pal.accent, pal.accentHover, rHov);
        DrawRoundedBox(drawBtnRc, 7.0f, btnBg, D2dColor(0, 0, 0, 0), 0.0f);

        DrawTextStrict(L"Start Recording", m_fmtBold115.Get(), drawBtnRc, D2dColor(255, 255, 255),
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        const float pauseW = 84.0f;
        const D2D1_RECT_F stopBtn{iLeft, y, iRight - pauseW - 6.0f, y + btnH};
        RegisterHit(ControlId::RecordToggleBtn, stopBtn, true);
        const float sHov = EaseOutCubic(GetHoverAnim(ControlId::RecordToggleBtn));
        const float pulse = st.isPaused ? 0.0f : (0.5f + 0.5f * std::sinf(m_recPulsePhase * 3.0f));
        if (!st.isPaused) {
            D2D1_COLOR_F halo = pal.accent;
            halo.a = 0.26f * (1.0f - pulse * 0.5f);
            const float ripple = 2.0f + 3.0f * pulse;
            DrawRoundedBox({stopBtn.left - ripple, stopBtn.top - ripple, stopBtn.right + ripple, stopBtn.bottom + ripple},
                           8.5f, halo, D2dColor(0, 0, 0, 0), 0.0f);
        }
        DrawRoundedBox(stopBtn, 7.0f, ColorLerp(pal.accent, pal.accentHover, std::max(sHov, pulse * 0.35f)), D2dColor(0, 0, 0, 0), 0.0f);
        const float midY = (stopBtn.top + stopBtn.bottom) * 0.5f;
        m_brush->SetColor(D2dColor(255, 255, 255));
        icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::StopSquare,
                        {stopBtn.left + 16.0f, midY - 7.5f, stopBtn.left + 31.0f, midY + 7.5f});
        DrawTextStrict(L"Stop Rec", m_fmtBold115.Get(), {stopBtn.left + 36.0f, y, stopBtn.right - 6.0f, y + btnH}, D2dColor(255, 255, 255));

        const D2D1_RECT_F pauseBtn{iRight - pauseW, y, iRight, y + btnH};
        RegisterHit(ControlId::PauseToggleBtn, pauseBtn, true);
        const float pHov = EaseOutCubic(GetHoverAnim(ControlId::PauseToggleBtn));
        DrawRoundedBox(pauseBtn, 7.0f, st.isPaused ? pal.accentTint : ColorLerp(pal.surface, pal.surfaceAlt, pHov),
                       st.isPaused ? pal.accent : ColorLerp(pal.borderStrong, pal.accentBorder, pHov * 0.5f), 1.0f);
        m_brush->SetColor(st.isPaused ? pal.accent : pal.text);
        icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                        st.isPaused ? icons::IconKind::Play : icons::IconKind::Pause,
                        {pauseBtn.left + 10.0f, midY - 6.5f, pauseBtn.left + 23.0f, midY + 6.5f}, 1.7f);
        DrawTextStrict(st.isPaused ? L"Resume" : L"Pause", m_fmtSemi10.Get(),
                        {pauseBtn.left + 26.0f, y, pauseBtn.right - 6.0f, y + btnH},
                        st.isPaused ? pal.accent : pal.text);
    }
    y += btnH + 7.0f;

    const float halfW = ((iRight - iLeft) - 6.0f) * 0.5f;
    const D2D1_RECT_F fldBtn{iLeft, y, iLeft + halfW, dockRc.bottom - 9.0f};
    RegisterHit(ControlId::OpenFolderBtn, fldBtn, true);
    const float fHov = EaseOutCubic(GetHoverAnim(ControlId::OpenFolderBtn));
    DrawRoundedBox(fldBtn, 6.0f, ColorLerp(pal.surface, pal.accentTint, fHov),
                   ColorLerp(pal.borderStrong, pal.accent, fHov), 1.0f);
    m_brush->SetColor(ColorLerp(pal.textSub, pal.accent, fHov));
    const float fMidY = (fldBtn.top + fldBtn.bottom) * 0.5f;
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::FolderOpen,
                    {fldBtn.left + 10.0f, fMidY - 6.5f, fldBtn.left + 23.0f, fMidY + 6.5f}, 1.5f);
    DrawTextStrict(L"Recordings", m_fmtSemi10.Get(), {fldBtn.left + 27.0f, fldBtn.top, fldBtn.right - 4.0f, fldBtn.bottom}, pal.text);

    const D2D1_RECT_F setBtn{fldBtn.right + 6.0f, y, iRight, dockRc.bottom - 9.0f};
    RegisterHit(ControlId::SettingsBtn, setBtn, true);
    const float sBtnHov = EaseOutCubic(GetHoverAnim(ControlId::SettingsBtn));
    DrawRoundedBox(setBtn, 6.0f, ColorLerp(pal.surface, pal.accentTint, sBtnHov),
                   ColorLerp(pal.borderStrong, pal.accent, sBtnHov), 1.0f);
    m_brush->SetColor(ColorLerp(pal.textSub, pal.accent, sBtnHov));
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::SettingsGear,
                    {setBtn.left + 12.0f, fMidY - 6.5f, setBtn.left + 25.0f, fMidY + 6.5f}, 1.5f, m_gearAngle);
    DrawTextStrict(L"Settings", m_fmtSemi10.Get(), {setBtn.left + 29.0f, setBtn.top, setBtn.right - 4.0f, setBtn.bottom}, pal.text);
}

void AppWindow::DrawBottomBar(const ThemePalette& pal) {
    const float barTop = m_clientH - 34.0f;
    const float barBot = m_clientH - 4.0f;

    DrawTextStrict(L"VRecorder", m_fmtBold9.Get(),
                   {16.0f, barTop, 210.0f, barBot}, pal.textMuted);

    if (!m_actionStatusText.empty() && m_statusAnim > 0.004f) {
        const float sEase = EaseOutCubic(m_statusAnim);
        D2D1_COLOR_F stCol = m_actionStatusError ? pal.accent : pal.textSub;
        stCol.a *= sEase;
        const float yOff = (1.0f - sEase) * 5.0f;
        DrawTextStrict(m_actionStatusText, m_fmtBold105.Get(),
                       {215.0f, barTop + yOff, m_clientW - 196.0f, barBot + yOff},
                       stCol,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    const D2D1_RECT_F qThemeBox{m_clientW - 186.0f, barTop + 3.0f, m_clientW - 52.0f, barBot - 3.0f};
    DrawSegmentedControl(pal, qThemeBox, 10001, {
        {ControlId::QuickThemeWhiteBtn, qThemeBox.left + 3.0f,  62.0f, L"White", static_cast<int>(icons::IconKind::Sun),  !pal.dark},
        {ControlId::QuickThemeBlackBtn, qThemeBox.left + 68.0f, 62.0f, L"Black", static_cast<int>(icons::IconKind::Moon), pal.dark}
    }, true, 6.0f, 4.0f, 9.0f);

    const D2D1_RECT_F trayRc{m_clientW - 44.0f, barTop, m_clientW - 14.0f, barBot};
    RegisterHit(ControlId::MinimizeTrayBtn, trayRc, true);
    const float trHov = EaseOutCubic(GetHoverAnim(ControlId::MinimizeTrayBtn));
    if (trHov > 0.01f || IsControlPressed(ControlId::MinimizeTrayBtn)) {
        DrawRoundedBox(trayRc, 6.0f, IsControlPressed(ControlId::MinimizeTrayBtn)
            ? pal.border : ColorLerp(D2dColor(0, 0, 0, 0), pal.surfaceAlt, trHov),
                       D2dColor(0, 0, 0, 0), 0.0f);
    }
    m_brush->SetColor(ColorLerp(pal.text, pal.accent, trHov));
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(),
                    icons::IconKind::TrayMinimize, {trayRc.left + 7.0f, barTop + 7.0f, trayRc.left + 23.0f, barTop + 23.0f}, 1.7f);
}

void AppWindow::DrawPopups(const ThemePalette& pal) {
    const PopupKind activePop = (m_openPopup != PopupKind::None) ? m_openPopup : m_lastPopup;
    if (activePop == PopupKind::None) return;
    const bool isClosing = (m_openPopup == PopupKind::None);
    const float pEase = EaseOutCubic(m_popupAnim);
    if (pEase <= 0.004f) return;

    if (!isClosing) {
        RegisterHit(ControlId::ModalBackdrop, {0.0f, 43.0f, m_clientW, m_clientH}, true);
    }
    m_brush->SetColor(D2dColor(0, 0, 0, static_cast<uint8_t>(std::lround(26.0f * pEase))));
    m_rt->FillRectangle(D2D1::RectF(0.0f, 43.0f, m_clientW, m_clientH), m_brush.Get());

    struct Entry { std::wstring label; std::wstring badge; int argInt; std::wstring argStr; bool selected; };
    std::vector<Entry> items;
    ControlId itemId = ControlId::SourcePickerItem;
    float popW = 340.0f;

    if (activePop == PopupKind::AddSourceMenu) {
        itemId = ControlId::AddSourceItem;
        popW = std::max(340.0f, m_popupAnchorRc.right - m_popupAnchorRc.left);
        for (const auto& m : m_monitors) {
            items.push_back({m.label, L"DISPLAY", m.index, L"",
                             m_config.hasCaptureSource && m_config.sourceType == 0 && m.index == m_config.monitorIndex});
        }
        items.push_back({L"Region Crop (Select Area)", L"REGION", -2, L"",
                         m_config.hasCaptureSource && m_config.sourceType == 2});
        for (const auto& w : m_windows) {
            std::wstring lbl = w.processName.empty() ? w.title : (L"[" + w.processName + L"] " + w.title);
            items.push_back({lbl, L"WINDOW", -1, w.title,
                             m_config.hasCaptureSource && m_config.sourceType == 1 &&
                             !m_config.windowTitleFilter.empty() &&
                             _wcsicmp(w.title.c_str(), m_config.windowTitleFilter.c_str()) == 0});
        }
    } else if (activePop == PopupKind::AddComponentMenu) {
        itemId = ControlId::AddComponentItem;
        popW = 280.0f;
        items.push_back({L"Webcam (On-Cam PiP)", m_config.camAdded ? L"ACTIVE" : L"CAM", 0, L"", m_config.camAdded});
        items.push_back({L"Text Overlay",        L"TEXT", 1, L"", false});
        items.push_back({L"Live Time / Clock",   L"TIME", 2, L"", false});
        items.push_back({L"Image / Logo File",   L"IMG",  3, L"", false});
        items.push_back({L"Frame / Banner Box",  L"BOX",  4, L"", false});
    } else if (activePop == PopupKind::SourcePicker) {
        itemId = ControlId::SourcePickerItem;
        popW = std::max(340.0f, m_popupAnchorRc.right - m_popupAnchorRc.left);
        if (m_config.sourceType == 0) {
            for (const auto& m : m_monitors) {
                items.push_back({m.label, m.isPrimary ? L"MAIN" : L"DISP", m.index, L"", m.index == m_config.monitorIndex});
            }
        } else {
            for (const auto& w : m_windows) {
                std::wstring lbl = w.processName.empty() ? w.title : (L"[" + w.processName + L"] " + w.title);
                items.push_back({lbl, std::to_wstring(w.width) + L"x" + std::to_wstring(w.height),
                                 0, w.title, !m_config.windowTitleFilter.empty() && _wcsicmp(w.title.c_str(), m_config.windowTitleFilter.c_str()) == 0});
            }
        }
    } else if (activePop == PopupKind::CameraPicker) {
        itemId = ControlId::CamPickerItem;
        popW = std::max(340.0f, m_popupAnchorRc.right - m_popupAnchorRc.left);
        for (const auto& c : m_cameras) {
            const bool sel = (m_config.camDeviceId.empty() && &c == &m_cameras.front()) ||
                             (_wcsicmp(c.id.c_str(), m_config.camDeviceId.c_str()) == 0);
            items.push_back({c.name, L"CAM", 0, c.id, sel});
        }
    } else if (activePop == PopupKind::EncoderPicker) {
        itemId = ControlId::EncoderPickerItem;
        popW = std::max(360.0f, m_popupAnchorRc.right - m_popupAnchorRc.left);
        for (const auto& e : m_encoders) {
            items.push_back({e.friendlyName, e.badge, 0, e.id, _wcsicmp(e.id.c_str(), m_config.encoderId.c_str()) == 0});
        }
    } else if (activePop == PopupKind::EncoderPresetPicker) {
        itemId = ControlId::PresetPickerItem;
        popW = std::max(320.0f, m_popupAnchorRc.right - m_popupAnchorRc.left);
        const DetectedEncoder activeEnc = EncoderManager::ResolveSelection(m_encoders, m_config.encoderId);
        const EncoderPreset activePre = EncoderManager::ResolvePreset(activeEnc, m_config.encoderPreset);
        const auto presets = EncoderManager::GetPresetsForEncoder(activeEnc);
        for (const auto& p : presets) {
            items.push_back({p.name, p.lowLatency ? L"FAST" : (p.bFrames > 0 ? L"HQ" : L"BAL"),
                             0, p.id, _wcsicmp(p.id.c_str(), activePre.id.c_str()) == 0});
        }
    } else if (activePop == PopupKind::ResPicker) {
        itemId = ControlId::ResPickerItem;
        popW = 260.0f;
        const struct { const wchar_t* id; const wchar_t* lbl; const wchar_t* tag; } rs[6] = {
            {L"native",   L"Native (Match Source)", L"ORIG"},
            {L"1440p",    L"2560 x 1440 (2K QHD)",  L"1440p"},
            {L"1080p",    L"1920 x 1080 (Full HD)", L"1080p"},
            {L"720p",     L"1280 x 720 (HD)",       L"720p"},
            {L"vertical", L"1080 x 1920 (Portrait)",L"9:16"},
            {L"custom",   L"Custom Width x Height", L"EDIT"}
        };
        for (const auto& r : rs) {
            items.push_back({r.lbl, r.tag, 0, r.id, _wcsicmp(m_config.resolution.c_str(), r.id) == 0});
        }
    } else if (activePop == PopupKind::DesktopAudioPicker) {
        itemId = ControlId::AudioDevItem;
        popW = std::max(350.0f, m_popupAnchorRc.right - m_popupAnchorRc.left);
        items.push_back({L"Default Windows Audio Output", L"SYS", 0, L"", m_config.desktopDeviceId.empty()});
        for (const auto& d : m_outAudioDevs) {
            items.push_back({d.name, d.isDefault ? L"DEF" : L"OUT", 0, d.id, m_config.desktopDeviceId == d.id});
        }
    } else if (activePop == PopupKind::MicAudioPicker) {
        itemId = ControlId::AudioDevItem;
        popW = std::max(350.0f, m_popupAnchorRc.right - m_popupAnchorRc.left);
        items.push_back({L"Default Microphone", L"MIC", 0, L"", m_config.micDeviceId.empty()});
        items.push_back({L"Disabled (No Microphone)", L"OFF", 0, L"none", _wcsicmp(m_config.micDeviceId.c_str(), L"none") == 0});
        for (const auto& d : m_inAudioDevs) {
            items.push_back({d.name, d.isDefault ? L"DEF" : L"IN", 0, d.id, m_config.micDeviceId == d.id});
        }
    }

    const float itemH = 28.0f, gap = 3.0f;
    const float contentH = items.empty() ? 36.0f : static_cast<float>(items.size()) * (itemH + gap) - gap;
    const float popupH = std::min(264.0f, contentH + 12.0f);

    float px = std::min(m_popupAnchorRc.left, m_clientW - popW - 14.0f);
    px = std::max(14.0f, px);
    float pyTop = m_popupOpenUp ? (m_popupAnchorRc.top - 4.0f - popupH) : (m_popupAnchorRc.bottom + 4.0f);
    pyTop = std::clamp(pyTop, 50.0f, std::max(50.0f, m_clientH - 14.0f - popupH));
    m_popupRc = {px, pyTop, px + popW, pyTop + popupH};

    const bool pAnimActive = (pEase < 0.998f);
    D2D1_MATRIX_3X2_F prevPopXform{};
    if (pAnimActive) {
        m_rt->GetTransform(&prevPopXform);
        const float slideY = (m_popupOpenUp ? 12.0f : -12.0f) * (1.0f - pEase);
        const float sc = 0.96f + 0.04f * pEase;
        const D2D1_POINT_2F piv{(m_popupRc.left + m_popupRc.right) * 0.5f,
                                m_popupOpenUp ? m_popupRc.bottom : m_popupRc.top};
        m_rt->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, piv) *
                           D2D1::Matrix3x2F::Translation(0.0f, slideY) * prevPopXform);
        m_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                              D2D1::IdentityMatrix(), pEase), nullptr);
    }

    DrawRoundedBox({m_popupRc.left - 1.0f, m_popupRc.top + 2.0f, m_popupRc.right + 1.0f, m_popupRc.bottom + 4.0f},
                   9.0f, D2dColor(0, 0, 0, 56), D2dColor(0, 0, 0, 0), 0.0f);
    DrawRoundedBox(m_popupRc, 8.0f, pal.card, pal.accentBorder, 1.0f);

    const D2D1_RECT_F clipRc{m_popupRc.left + 6.0f, m_popupRc.top + 6.0f, m_popupRc.right - 6.0f, m_popupRc.bottom - 6.0f};
    const float viewH = clipRc.bottom - clipRc.top;
    m_popupMaxScroll = std::max(0.0f, contentH - viewH);
    m_popupScroll = std::clamp(m_popupScroll, 0.0f, m_popupMaxScroll);

    m_rt->PushAxisAlignedClip(clipRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (items.empty()) {
        DrawTextStrict(L"No entries detected", m_fmtSemi11.Get(), clipRc, pal.textMuted,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        float y = clipRc.top - m_popupScroll;
        for (const auto& it : items) {
            if (y + itemH >= clipRc.top && y <= clipRc.bottom) {
                const D2D1_RECT_F rowRc{clipRc.left, y, clipRc.right, y + itemH};
                if (!isClosing) RegisterHit(itemId, rowRc, true, it.argInt, it.argStr);
                const float rHov = isClosing ? 0.0f : EaseOutCubic(GetHoverAnim(itemId, it.argInt, it.argStr));
                DrawRoundedBox(rowRc, 5.0f, it.selected ? pal.accentTint : ColorLerp(pal.card, pal.surfaceAlt, rHov),
                               it.selected ? pal.accentBorder : D2dColor(0, 0, 0, 0), 1.0f);
                m_brush->SetColor(it.selected ? pal.accent : ColorLerp(pal.borderStrong, pal.accentBorder, rHov));
                m_rt->FillEllipse(D2D1::Ellipse({rowRc.left + 10.0f, y + itemH * 0.5f}, 3.2f, 3.2f), m_brush.Get());
                DrawTextStrict(it.label, it.selected ? m_fmtBold105.Get() : m_fmtSemi10.Get(),
                               {rowRc.left + 20.0f, y, rowRc.right - 76.0f, y + itemH},
                               it.selected ? pal.accent : pal.text);
                if (!it.badge.empty()) {
                    const D2D1_RECT_F bRc{rowRc.right - 72.0f, y + 5.0f, rowRc.right - 4.0f, y + 23.0f};
                    DrawRoundedBox(bRc, 4.0f, it.selected ? pal.accent : pal.surfaceAlt, D2dColor(0, 0, 0, 0), 0.0f);
                    DrawTextStrict(it.badge, m_fmtBold9.Get(), bRc, it.selected ? D2dColor(255, 255, 255) : pal.textMuted,
                                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
            }
            y += itemH + gap;
        }
    }
    m_rt->PopAxisAlignedClip();

    if (pAnimActive) {
        m_rt->PopLayer();
        m_rt->SetTransform(prevPopXform);
    }
}

void AppWindow::DrawSettingsView(const ThemePalette& pal) {
    const float pageLeft = 20.0f;
    const float pageRight = m_clientW - 20.0f;

    const D2D1_RECT_F backRc{pageLeft, 53.0f, pageLeft + 28.0f, 81.0f};
    RegisterHit(ControlId::SettingsBackBtn, backRc, true);
    const float bHov = EaseOutCubic(GetHoverAnim(ControlId::SettingsBackBtn));
    const bool bPrs = IsControlPressed(ControlId::SettingsBackBtn);
    if (bHov > 0.01f || bPrs) {
        DrawRoundedBox(backRc, 6.0f, bPrs ? pal.border : ColorLerp(D2dColor(0, 0, 0, 0), pal.surfaceAlt, bHov),
                       D2dColor(0, 0, 0, 0), 0.0f);
    }
    m_brush->SetColor(ColorLerp(pal.text, pal.accent, bHov));
    const float backOff = -2.0f * bHov;
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::ChevronLeft,
                    {backRc.left + 6.5f + backOff, backRc.top + 6.5f, backRc.right - 6.5f + backOff, backRc.bottom - 6.5f}, 1.9f);
    DrawTextStrict(L"Settings", m_fmtBold145.Get(), {pageLeft + 36.0f, 52.0f, 300.0f, 82.0f}, pal.text);

    m_brush->SetColor(pal.border);
    m_rt->DrawLine({pageLeft, 89.5f}, {pageRight, 89.5f}, m_brush.Get(), 1.0f);

    const float colW = (pageRight - pageLeft - 48.0f) * 0.5f;
    const float lX0 = pageLeft, lX1 = pageLeft + colW;
    const float rX0 = lX1 + 48.0f, rX1 = pageRight;
    const bool lockVideo = m_recorder.GetStats().isRecording;

    float ly = 98.0f;
    DrawTextStrict(L"APPEARANCE & WINDOW", m_fmtBold9.Get(), {lX0, ly, lX1, ly + 16.0f}, pal.accent);
    ly += 20.0f;

    DrawTextStrict(L"Theme", m_fmtReg115.Get(), {lX0, ly, lX0 + 180.0f, ly + 30.0f}, pal.text);
    const D2D1_RECT_F themeBox{lX1 - 166.0f, ly + 1.0f, lX1, ly + 29.0f};
    DrawSegmentedControl(pal, themeBox, 10002, {
        {ControlId::ThemeWhiteBtn, themeBox.left + 3.0f,  78.0f, L"White", static_cast<int>(icons::IconKind::Sun),  !pal.dark},
        {ControlId::ThemeBlackBtn, themeBox.left + 85.0f, 78.0f, L"Black", static_cast<int>(icons::IconKind::Moon), pal.dark}
    }, true, 7.0f, 5.0f, 10.0f);
    ly += 33.0f;

    auto drawSetToggle = [&](ControlId id, const wchar_t* label, bool on) {
        m_brush->SetColor(pal.divider);
        m_rt->DrawLine({lX0, ly}, {lX1, ly}, m_brush.Get(), 1.0f);
        ly += 1.0f;
        DrawTextStrict(label, m_fmtReg115.Get(), {lX0, ly, lX1 - 50.0f, ly + 28.0f}, pal.text);
        DrawToggleSwitch(pal, id, lX1, ly + 14.0f, on);
        ly += 28.0f;
    };
    drawSetToggle(ControlId::ToggleAlwaysOnTop,     L"Always on top",     m_config.alwaysOnTop);
    drawSetToggle(ControlId::ToggleMinToTray,       L"Minimize to tray",  m_config.minimizeToTray);
    drawSetToggle(ControlId::ToggleCloseToTray,     L"Close to tray",     m_config.closeToTray);
    drawSetToggle(ControlId::ToggleNotifications,   L"Notifications",     m_config.showNotifications);

    ly += 20.0f;
    DrawTextStrict(L"AUDIO DEVICES & QUALITY", m_fmtBold9.Get(), {lX0, ly, lX1, ly + 16.0f}, pal.accent);
    ly += 20.0f;

    auto resolveAudioLabel = [](const std::vector<AudioDeviceInfo>& list, const std::wstring& id, const wchar_t* defLbl) -> std::wstring {
        if (id.empty()) return defLbl;
        if (_wcsicmp(id.c_str(), L"none") == 0) return L"Disabled (No Microphone)";
        for (const auto& d : list) if (d.id == id) return d.name;
        return defLbl;
    };
    auto drawAudioPicker = [&](ControlId id, const wchar_t* lbl, const std::wstring& curName, PopupKind kind) {
        DrawTextStrict(lbl, m_fmtReg115.Get(), {lX0, ly, lX0 + 115.0f, ly + 28.0f}, pal.text);
        const D2D1_RECT_F bRc{lX0 + 120.0f, ly, lX1, ly + 28.0f};
        DrawAnimatedPickerBox(pal, id, bRc, curName, kind, true, false, 6.0f);
        ly += 35.0f;
    };

    drawAudioPicker(ControlId::DeskDeviceBtn, L"Desktop Audio",
                    resolveAudioLabel(m_outAudioDevs, m_config.desktopDeviceId, L"Default Windows Audio Output"),
                    PopupKind::DesktopAudioPicker);
    drawAudioPicker(ControlId::MicDeviceBtn, L"Microphone",
                    resolveAudioLabel(m_inAudioDevs, m_config.micDeviceId, L"Default Microphone"),
                    PopupKind::MicAudioPicker);

    DrawTextStrict(L"Audio Bitrate", m_fmtReg115.Get(), {lX0, ly, lX0 + 160.0f, ly + 28.0f}, pal.text);
    const D2D1_RECT_F abBox{lX1 - 156.0f, ly, lX1, ly + 28.0f};
    DrawSegmentedControl(pal, abBox, 10004, {
        {ControlId::Audio192Btn, abBox.left + 3.0f,  74.0f, L"192 kbps", -1, m_config.audioBitrateKbps < 256},
        {ControlId::Audio320Btn, abBox.left + 79.0f, 74.0f, L"320 kbps", -1, m_config.audioBitrateKbps >= 256}
    }, !lockVideo, 7.0f, 5.0f, 10.0f);

    float ry = 98.0f;
    DrawTextStrict(L"VIDEO, ENCODER & OUTPUT", m_fmtBold9.Get(), {rX0, ry, rX1, ry + 16.0f}, pal.accent);
    if (lockVideo) {
        DrawTextStrict(L"Locked while recording", m_fmtSemi10.Get(),
                       {rX1 - 160.0f, ry, rX1, ry + 16.0f}, pal.accent,
                       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    ry += 20.0f;

    const DetectedEncoder activeEnc = EncoderManager::ResolveSelection(m_encoders, m_config.encoderId);
    const EncoderPreset activePre = EncoderManager::ResolvePreset(activeEnc, m_config.encoderPreset);
    DrawTextStrict(L"Video Encoder", m_fmtReg115.Get(), {rX0, ry, rX0 + 120.0f, ry + 28.0f}, pal.text);
    const D2D1_RECT_F sEncRc{rX0 + 124.0f, ry, rX1, ry + 28.0f};
    std::wstring sEncTxt = (m_config.encoderId == L"auto") ? (L"Auto \u00B7 " + activeEnc.friendlyName) : activeEnc.friendlyName;
    DrawAnimatedPickerBox(pal, ControlId::SettingsEncPickerBtn, sEncRc, sEncTxt, PopupKind::EncoderPicker, !lockVideo, false, 6.0f);
    ry += 35.0f;

    DrawTextStrict(L"Encoder Preset", m_fmtReg115.Get(), {rX0, ry, rX0 + 120.0f, ry + 28.0f}, pal.text);
    const D2D1_RECT_F sPreRc{rX0 + 124.0f, ry, rX1, ry + 28.0f};
    DrawAnimatedPickerBox(pal, ControlId::PresetPickerBtn, sPreRc, activePre.name, PopupKind::EncoderPresetPicker, !lockVideo, false, 6.0f);
    ry += 35.0f;

    struct OptPill { ControlId id; const wchar_t* label; float w; bool sel; };
    auto drawPillRow = [&](const wchar_t* rowLabel, int groupSlot, const std::vector<OptPill>& pills, bool en) {
        DrawTextStrict(rowLabel, m_fmtReg115.Get(), {rX0, ry, rX0 + 130.0f, ry + 28.0f}, pal.text);
        float totalW = 6.0f;
        for (const auto& p : pills) totalW += p.w + 2.0f;
        totalW -= 2.0f;
        const D2D1_RECT_F box{rX1 - totalW, ry, rX1, ry + 28.0f};
        std::vector<SegmentedOption> segs;
        segs.reserve(pills.size());
        float px = box.left + 3.0f;
        for (const auto& p : pills) {
            segs.push_back({p.id, px, p.w, p.label, -1, p.sel});
            px += p.w + 2.0f;
        }
        DrawSegmentedControl(pal, box, groupSlot, segs, en, 7.0f, 5.0f, 10.0f);
        ry += 35.0f;
    };

    drawPillRow(L"Resolution", 10005, {
        {ControlId::ResNativeBtn, L"Native", 52.0f, _wcsicmp(m_config.resolution.c_str(), L"native") == 0},
        {ControlId::Res1440Btn,   L"1440p",  50.0f, _wcsicmp(m_config.resolution.c_str(), L"1440p") == 0},
        {ControlId::Res1080Btn,   L"1080p",  50.0f, _wcsicmp(m_config.resolution.c_str(), L"1080p") == 0},
        {ControlId::Res720Btn,    L"720p",   46.0f, _wcsicmp(m_config.resolution.c_str(), L"720p") == 0},
        {ControlId::ResVertBtn,   L"9:16",   42.0f, _wcsicmp(m_config.resolution.c_str(), L"vertical") == 0},
        {ControlId::ResCustomBtn, L"Custom", 54.0f, _wcsicmp(m_config.resolution.c_str(), L"custom") == 0}
    }, !lockVideo);

    drawPillRow(L"Frame Rate", 10006, {
        {ControlId::Fps30Btn,  L"30 FPS",  62.0f, m_config.fps == 30},
        {ControlId::Fps60Btn,  L"60 FPS",  62.0f, m_config.fps == 60},
        {ControlId::Fps120Btn, L"120 FPS", 68.0f, m_config.fps == 120}
    }, !lockVideo);

    drawPillRow(L"Video Bitrate", 10007, {
        {ControlId::Bitrate8Btn,  L"8M",  42.0f, m_config.bitrateMbps <= 12},
        {ControlId::Bitrate18Btn, L"18M", 44.0f, m_config.bitrateMbps > 12 && m_config.bitrateMbps <= 22},
        {ControlId::Bitrate28Btn, L"28M", 44.0f, m_config.bitrateMbps > 22 && m_config.bitrateMbps <= 36},
        {ControlId::Bitrate45Btn, L"45M", 44.0f, m_config.bitrateMbps > 36}
    }, !lockVideo);

    ry += 10.0f;
    DrawTextStrict(L"HOTKEYS & RECORDINGS FOLDER", m_fmtBold9.Get(), {rX0, ry, rX1, ry + 16.0f}, pal.accent);
    ry += 20.0f;

    drawPillRow(L"Record Hotkey", 10008, {
        {ControlId::HotkeyF8Btn,   L"F8",   44.0f, _wcsicmp(m_config.recordHotkey.c_str(), L"F8") == 0},
        {ControlId::HotkeyF9Btn,   L"F9",   44.0f, _wcsicmp(m_config.recordHotkey.c_str(), L"F9") == 0},
        {ControlId::HotkeyF10Btn,  L"F10",  48.0f, _wcsicmp(m_config.recordHotkey.c_str(), L"F10") == 0},
        {ControlId::HotkeyNoneBtn, L"None", 52.0f, _wcsicmp(m_config.recordHotkey.c_str(), L"None") == 0}
    }, true);

    DrawTextStrict(L"Output Folder", m_fmtReg115.Get(), {rX0, ry, rX1, ry + 18.0f}, pal.text);
    ry += 22.0f;
    const D2D1_RECT_F pathBox{rX0, ry, rX1 - 134.0f, ry + 28.0f};
    DrawRoundedBox(pathBox, 6.0f, pal.surface, pal.border, 1.0f);
    DrawTextStrict(m_config.outputDir, m_fmtSemi10.Get(),
                   {pathBox.left + 10.0f, pathBox.top, pathBox.right - 10.0f, pathBox.bottom}, pal.textSub);

    DrawButton(pal, ControlId::BrowseOutputBtn, {rX1 - 128.0f, ry, rX1 - 66.0f, ry + 28.0f}, L"Browse", false, false);
    DrawButton(pal, ControlId::OpenOutputSettingsBtn, {rX1 - 60.0f, ry, rX1, ry + 28.0f}, L"Open", false, false);
    ry += 40.0f;

    const D2D1_RECT_F infoRc{rX0, ry, rX1, std::max(ry + 80.0f, m_clientH - 96.0f)};
    DrawRoundedBox(infoRc, 8.0f, pal.surface, pal.border, 1.0f);
    DrawTextStrict(L"Detected Hardware & Software Encoders (" +
                       std::to_wstring(m_encoders.size() > 1 ? m_encoders.size() - 1 : 0) + L")",
                   m_fmtBold105.Get(), {infoRc.left + 12.0f, ry + 8.0f, infoRc.right - 12.0f, ry + 24.0f}, pal.text);
    std::wstring encSummary;
    for (size_t i = 1; i < m_encoders.size() && i < 6; ++i) {
        if (!encSummary.empty()) encSummary += L"\n";
        encSummary += L"• " + m_encoders[i].friendlyName + L" (" + m_encoders[i].badge + L")";
    }
    if (encSummary.empty()) encSummary = L"Standard Software H.264 Ready";
    DrawTextStrict(encSummary, m_fmtReg105.Get(),
                   {infoRc.left + 12.0f, ry + 26.0f, infoRc.right - 12.0f, infoRc.bottom - 8.0f},
                   pal.textSub, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);

    const float footerTop = m_clientH - 74.0f;
    m_brush->SetColor(pal.border);
    m_rt->DrawLine({pageLeft, footerTop}, {pageRight, footerTop}, m_brush.Get(), 1.0f);

    float titleLeft = pageLeft + 30.0f;
    if (ID2D1Bitmap* logo = GetOrCreateLogoBitmap()) {
        const D2D1_RECT_F srcRc = D2D1::RectF(24.0f, 98.0f, 360.0f, 314.0f);
        m_rt->DrawBitmap(logo, {pageLeft, footerTop + 16.0f, pageLeft + 40.0f, footerTop + 42.0f}, 1.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcRc);
        titleLeft = pageLeft + 48.0f;
    }
    const float nameW = std::max(72.0f, MeasureTextWidth(L"VRecorder", m_fmtBold135.Get()));
    DrawTextStrict(L"VRecorder", m_fmtBold135.Get(), {titleLeft, footerTop + 13.0f, titleLeft + nameW + 4.0f, footerTop + 33.0f}, pal.text);
    DrawTextStrict(L"Created by : @vinnotdkk", m_fmtReg115.Get(), {titleLeft, footerTop + 34.0f, 360.0f, footerTop + 52.0f}, pal.textSub);

    const D2D1_RECT_F supportRc{pageRight - 152.0f, footerTop + 17.0f, pageRight, footerTop + 51.0f};
    RegisterHit(ControlId::SupportDevBtn, supportRc, true);
    const float sHov = EaseOutCubic(GetHoverAnim(ControlId::SupportDevBtn));
    const bool sPrs = IsControlPressed(ControlId::SupportDevBtn);
    DrawRoundedBox(supportRc, 7.0f, sPrs ? pal.accentPress : ColorLerp(pal.accent, pal.accentHover, sHov), D2dColor(0, 0, 0, 0), 0.0f);
    m_brush->SetColor(D2dColor(255, 255, 255));
    icons::DrawIcon(m_rt.Get(), m_d2dFactory.Get(), m_brush.Get(), m_roundStroke.Get(), icons::IconKind::Heart,
                    {supportRc.left + 14.0f, footerTop + 27.5f, supportRc.left + 27.0f, footerTop + 40.5f});
    DrawTextStrict(L"Support Developer", m_fmtBold115.Get(),
                   {supportRc.left + 31.0f, supportRc.top, supportRc.right - 10.0f, supportRc.bottom}, D2dColor(255, 255, 255));
}

void AppWindow::InitTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;

    const RecorderStats st = m_recorder.GetStats();
    m_currentTrayIcon = m_trayRenderer.CreateStudioTrayIcon(m_hInst, st.isRecording, st.isPaused, m_dpi);
    nid.hIcon = m_currentTrayIcon ? m_currentTrayIcon : m_appIconSmall;
    wcscpy_s(nid.szTip, L"VRecorder");

    m_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    m_lastTrayRec = st.isRecording;
    m_lastTrayPaused = st.isPaused;
}

void AppWindow::UpdateTrayIcon(bool forceRecreate) {
    if (!m_trayAdded) return;
    const RecorderStats st = m_recorder.GetStats();

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;

    uint32_t ow = 1920, oh = 1080;
    m_recorder.ResolveOutputDimensions(m_config, ow, oh);
    if (st.isRecording) {
        const std::wstring hms = FormatDurationHms(st.elapsedSec);
        _snwprintf_s(nid.szTip, ARRAYSIZE(nid.szTip), _TRUNCATE,
                     L"VRecorder - %s %s (%ux%u)",
                     st.isPaused ? L"PAUSED" : L"REC", hms.c_str(), ow, oh);
    } else {
        _snwprintf_s(nid.szTip, ARRAYSIZE(nid.szTip), _TRUNCATE,
                     L"VRecorder - Ready (%ux%u @ %d FPS)", ow, oh, m_config.fps);
    }

    if (forceRecreate || m_lastTrayRec != st.isRecording || m_lastTrayPaused != st.isPaused) {
        HICON nextIcon = m_trayRenderer.CreateStudioTrayIcon(m_hInst, st.isRecording, st.isPaused, m_dpi);
        if (nextIcon) {
            if (m_currentTrayIcon) DestroyIcon(m_currentTrayIcon);
            m_currentTrayIcon = nextIcon;
            nid.uFlags |= NIF_ICON;
            nid.hIcon = m_currentTrayIcon;
        }
        m_lastTrayRec = st.isRecording;
        m_lastTrayPaused = st.isPaused;
    } else if (m_lastTrayTip == nid.szTip) {
        return;
    }
    m_lastTrayTip = nid.szTip;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AppWindow::RemoveTrayIcon() {
    if (!m_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_trayAdded = false;
}

void AppWindow::ShowTrayBalloon(const wchar_t* title, const wchar_t* body, bool errorIcon) {
    if (!m_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = (errorIcon ? NIIF_ERROR : NIIF_INFO) | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, body, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AppWindow::ShowTrayContextMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    const bool visible = IsWindowVisible(m_hwnd) != FALSE;
    const RecorderStats st = m_recorder.GetStats();

    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, visible ? L"Hide to Tray" : L"Show VRecorder");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_REC,  st.isRecording ? L"Stop Recording" : L"Start Recording");
    if (st.isRecording) {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_PAUSE, st.isPaused ? L"Resume Recording" : L"Pause Recording");
    }
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_FOLDER, L"Open Recordings Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SETTINGS, L"Settings");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

}
