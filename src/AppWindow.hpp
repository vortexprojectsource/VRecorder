#pragma once

#include "AudioEngine.hpp"
#include "CameraEngine.hpp"
#include "CaptureEngine.hpp"
#include "ConfigManager.hpp"
#include "EncoderManager.hpp"
#include "OverlayCompositor.hpp"
#include "RecorderEngine.hpp"
#include "TrayIconRenderer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vrec {

constexpr UINT WM_APP_SHOW_WINDOW = WM_APP + 1;
constexpr UINT WM_APP_TRAY_ICON   = WM_APP + 2;

class AppWindow {
public:
    AppWindow(HINSTANCE hInst, bool startMinimized);
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    bool Create();
    bool PreTranslateMessage(MSG* msg);
    HWND GetHwnd() const { return m_hwnd; }

private:
    enum class FadeMode { None, In, OutHide, OutMinimize, OutClose };
    enum class PopupKind {
        None,
        AddSourceMenu,
        AddComponentMenu,
        SourcePicker,
        CameraPicker,
        EncoderPicker,
        EncoderPresetPicker,
        ResPicker,
        DesktopAudioPicker,
        MicAudioPicker
    };

    enum class EditFieldId {
        None = 0,
        OverlayText,
        OutWidth,
        OutHeight,
        CropX,
        CropY,
        CropW,
        CropH,
        SrcX,
        SrcY,
        SrcW,
        SrcH
    };

    enum class CanvasLayer {
        None = -1,
        MainSource = 0,
        Camera = 1,
        Overlay = 2
    };

    enum class CanvasDragMode {
        None = 0,
        MoveOverlay,
        ResizeOverlay,
        MoveCrop,
        ResizeCrop,
        MoveSource,
        ResizeSource,
        MoveCamera,
        ResizeCamera
    };

    enum class ControlId {
        None = 0,
        ModalBackdrop,
        WinMinimize,
        WinMaximize,
        WinClose,

        AddSourceBtn,
        AddSourceItem,
        AddComponentBtn,
        AddComponentItem,
        ToggleSourceVisBtn,
        RemoveSourceBtn,
        RemoveCamBtn,
        PresetPickerBtn,
        PresetPickerItem,

        SrcTypeDisplay,
        SrcTypeWindow,
        SrcTypeRegion,
        SourcePickerBtn,
        SourcePickerItem,
        RefreshWindowsBtn,
        CanvasPickWindowBtn,
        ToggleShowCursor,
        InputCropX,
        InputCropY,
        InputCropW,
        InputCropH,
        CropPreset1080,
        CropPreset720,
        CropPresetVert,
        CropPresetSq,

        CanvasSourceDrag,
        CanvasSourceResize,
        SrcFitBtn,
        SrcFillBtn,
        SrcCenterBtn,
        SrcPipBtn,
        SrcResetBtn,
        SrcKeepAspectToggle,
        InputSrcX,
        InputSrcY,
        InputSrcW,
        InputSrcH,

        SrcTabGeneral,
        SrcTabFilter,
        ToggleColorFilter,
        FilterGammaSlider,
        FilterContrastSlider,
        FilterBrightnessSlider,
        FilterSaturationSlider,
        FilterResetBtn,

        ToggleCamBtn,
        CamPickerBtn,
        CamPickerItem,
        RefreshCamerasBtn,
        CanvasCamDrag,
        CanvasCamResize,
        CamPosTL,
        CamPosTR,
        CamPosBL,
        CamPosBR,
        CamShapeRect,
        CamShapeRound,
        CamShapeCircle,
        CamSizeMinusBtn,
        CamSizePlusBtn,
        CamMirrorToggle,
        CamBorderToggle,

        LayerSelectSource,
        LayerSelectCamera,
        AddOverlayTextBtn,
        AddOverlayClockBtn,
        AddOverlayBoxBtn,
        AddOverlayImageBtn,
        OverlayListSelect,
        OverlayListToggleVis,
        OverlayListDelete,
        InputOverlayText,
        OverlayBrowseImgBtn,
        OverlaySizeMinusBtn,
        OverlaySizePlusBtn,
        OverlayColorBtn,
        OverlayBgToggleBtn,
        CanvasOverlayDrag,
        CanvasOverlayResize,
        CanvasCropDrag,
        CanvasCropResize,

        DeskMuteBtn,
        DeskVolSlider,
        MicMuteBtn,
        MicVolSlider,

        EncoderPickerBtn,
        EncoderPickerItem,
        RefreshEncodersBtn,
        ResPickerBtn,
        ResPickerItem,
        InputOutWidth,
        InputOutHeight,
        CycleFpsBtn,
        RecordToggleBtn,
        PauseToggleBtn,
        OpenFolderBtn,

        SettingsBtn,
        QuickThemeWhiteBtn,
        QuickThemeBlackBtn,
        MinimizeTrayBtn,

        SettingsBackBtn,
        ThemeWhiteBtn,
        ThemeBlackBtn,
        ToggleAlwaysOnTop,
        ToggleMinToTray,
        ToggleCloseToTray,
        ToggleNotifications,
        ResNativeBtn,
        Res1440Btn,
        Res1080Btn,
        Res720Btn,
        ResVertBtn,
        ResCustomBtn,
        Fps30Btn,
        Fps60Btn,
        Fps120Btn,
        Bitrate8Btn,
        Bitrate18Btn,
        Bitrate28Btn,
        Bitrate45Btn,
        SettingsEncPickerBtn,
        HotkeyF8Btn,
        HotkeyF9Btn,
        HotkeyF10Btn,
        HotkeyNoneBtn,
        BrowseOutputBtn,
        OpenOutputSettingsBtn,
        DeskDeviceBtn,
        MicDeviceBtn,
        AudioDevItem,
        Audio192Btn,
        Audio320Btn,
        SupportDevBtn
    };

    struct HitControl {
        ControlId id = ControlId::None;
        D2D1_RECT_F rect{};
        bool enabled = true;
        bool isSlider = false;
        double sliderMin = 0.0;
        double sliderMax = 100.0;
        double sliderStep = 1.0;
        int argInt = 0;
        std::wstring argStr;
    };

    struct ThemePalette {
        bool dark = false;
        D2D1_COLOR_F bg{};
        D2D1_COLOR_F surface{};
        D2D1_COLOR_F surfaceAlt{};
        D2D1_COLOR_F card{};
        D2D1_COLOR_F border{};
        D2D1_COLOR_F borderStrong{};
        D2D1_COLOR_F divider{};
        D2D1_COLOR_F text{};
        D2D1_COLOR_F textSub{};
        D2D1_COLOR_F textMuted{};
        D2D1_COLOR_F accent{};
        D2D1_COLOR_F accentHover{};
        D2D1_COLOR_F accentPress{};
        D2D1_COLOR_F accentTint{};
        D2D1_COLOR_F accentBorder{};
        D2D1_COLOR_F canvasBg{};
        D2D1_COLOR_F amber{};
        D2D1_COLOR_F green{};
    };

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnDestroy();
    void OnSize(UINT width, UINT height);
    void OnDpiChanged(UINT newDpi, const RECT* suggestedRect);
    void OnGetMinMaxInfo(MINMAXINFO* mmi) const;
    LRESULT OnNcHitTest(int screenX, int screenY) const;
    void OnTimer(UINT_PTR timerId);
    void OnTrayMessage(LPARAM lParam);
    void OnPaint();
    void OnPrintClient(HDC hdc);
    void OnMouseMove(int px, int py);
    void OnMouseLeave();
    void OnLButtonDown(int px, int py);
    void OnLButtonUp(int px, int py);
    void OnMouseWheel(int px, int py, short delta);
    void OnKeyDown(WPARAM vk);
    void OnChar(wchar_t ch);

    ThemePalette GetPalette() const noexcept;
    void ApplyDwmTheme();
    void ApplyAlwaysOnTop();
    void RegisterGlobalHotkeys();
    void InvalidateView();

    bool EnsureD2DFactoryResources();
    bool EnsureHwndRenderTarget();
    void DiscardD2DDeviceResources();
    void RenderScene(const ThemePalette& pal);

    void SyncEnginesFromConfig();
    void RefreshAvailableLists();
    void SaveAndApplyConfig();
    void BrowseOutputFolder();
    void BrowseOverlayImage();
    void OpenRecordingsFolder();
    void ToggleStartStopRecording();
    void TogglePauseRecording();
    void ToggleMaximizeRestore();
    void SetActionStatus(const std::wstring& text, bool isError);

    void BeginEditField(EditFieldId field, const std::wstring& initialText);
    void CommitEditField(bool applyChanges);

    void AddOverlay(OverlayKind kind);
    OverlayItem* FindSelectedOverlay();
    void DeleteOverlay(uint32_t id);
    void TogglePopup(PopupKind kind, D2D1_RECT_F anchorRc = {}, bool openUp = true);
    void SwitchToSettings(bool showSettings);
    void ShowMainWindow();
    void HideToTray();
    void MinimizeWindow();
    void StartWindowFade(FadeMode mode);
    void ApplyLayeredAlpha(BYTE alpha);
    void ClearLayeredStyle();

    void InitTrayIcon();
    void UpdateTrayIcon(bool forceRecreate);
    void RemoveTrayIcon();
    void ShowTrayBalloon(const wchar_t* title, const wchar_t* body, bool errorIcon);
    void ShowTrayContextMenu();

    D2D1_POINT_2F PtToDip(int px, int py) const noexcept;
    const HitControl* HitTestDip(D2D1_POINT_2F pt) const;
    bool IsControlHovered(ControlId id, int argInt = 0, const std::wstring& argStr = L"") const;
    bool IsControlPressed(ControlId id, int argInt = 0, const std::wstring& argStr = L"") const;
    void KickUiAnimation();
    void StepUiAnimations(float dtSec);
    float GetHoverAnim(ControlId id, int argInt = 0, const std::wstring& argStr = L"") const;
    float GetToggleAnim(ControlId id, bool isOn) const;
    float AnimSlot(int slotKey, float target, float speed = 18.0f) const;
    ID2D1Layer* EnsureAnimLayer();

    struct SegmentedOption {
        ControlId id = ControlId::None;
        float x = 0.0f;
        float w = 0.0f;
        const wchar_t* label = L"";
        int iconId = -1;
        bool selected = false;
    };
    void DrawAnimatedPickerBox(const ThemePalette& pal, ControlId id, const D2D1_RECT_F& rc,
                               const std::wstring& text, PopupKind popupKind, bool enabled = true,
                               bool warnAccent = false, float radius = 6.0f);
    void DrawSegmentedControl(const ThemePalette& pal, const D2D1_RECT_F& boxRc, int animGroupId,
                              const std::vector<SegmentedOption>& options, bool enabled = true,
                              float boxRadius = 7.0f, float pillRadius = 5.0f, float fontSize = 10.0f);
    void ApplySliderFromDipX(const HitControl& ctrl, float dipX);
    void ActivateControl(const HitControl& ctrl);
    void RegisterHit(ControlId id, const D2D1_RECT_F& rc, bool enabled = true,
                     int argInt = 0, std::wstring argStr = L"");

    void DrawTitleBar(const ThemePalette& pal);
    void DrawMainView(const ThemePalette& pal);
    void DrawPreviewStage(const ThemePalette& pal, const D2D1_RECT_F& stageRc);
    void DrawSceneLayersSidebar(const ThemePalette& pal, const D2D1_RECT_F& panelRc);
    void DrawDockAudioMixer(const ThemePalette& pal, const D2D1_RECT_F& dockRc);
    void DrawDockOutputAndEncoder(const ThemePalette& pal, const D2D1_RECT_F& dockRc);
    void DrawDockControls(const ThemePalette& pal, const D2D1_RECT_F& dockRc);
    void DrawBottomBar(const ThemePalette& pal);
    void DrawPopups(const ThemePalette& pal);
    void DrawSettingsView(const ThemePalette& pal);

    void DrawObsSelectionFrame(const ThemePalette& pal, const D2D1_RECT_F& rc, ControlId resizeId, int argInt, bool fourCorners);

    void DrawTextStrict(const std::wstring& text,
                        IDWriteTextFormat* fmt,
                        const D2D1_RECT_F& rc,
                        D2D1_COLOR_F color,
                        DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING,
                        DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                        bool wrap = false);
    float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* fmt);
    void DrawRoundedBox(const D2D1_RECT_F& rc, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F border, float borderW = 1.0f);
    void DrawButton(const ThemePalette& pal,
                    ControlId id,
                    const D2D1_RECT_F& rc,
                    const std::wstring& text,
                    bool primaryRed,
                    bool selectedRedTint,
                    bool enabled = true,
                    float fontSize = 11.0f,
                    bool bold = true,
                    float radius = 6.0f,
                    int argInt = 0,
                    std::wstring argStr = L"");
    void DrawToggleSwitch(const ThemePalette& pal, ControlId id, float rightX, float centerY, bool isOn, bool enabled = true);
    void DrawRedSlider(const ThemePalette& pal, ControlId id, float leftX, float rightX, float centerY,
                       double val, double minVal, double maxVal, double stepVal, bool enabled = true);
    void DrawVuMeter(const ThemePalette& pal, const D2D1_RECT_F& rc, float level, float peakLevel = 0.0f, bool muted = false);
    void DrawInlineInputBox(const ThemePalette& pal, ControlId id, EditFieldId field,
                            const D2D1_RECT_F& rc, const std::wstring& displayVal, bool enabled = true);
    D2D1_RECT_F ComputeOverlayCanvasRect(const OverlayItem& ov, const D2D1_RECT_F& drawRc);
    ID2D1Bitmap* GetOrLoadOverlayD2DBitmap(const std::wstring& imagePath);
    ID2D1Bitmap* GetOrCreateLogoBitmap();

    HINSTANCE m_hInst = nullptr;
    HWND m_hwnd = nullptr;
    bool m_startMinimized = false;
    bool m_trayAdded = false;
    bool m_closingForReal = false;
    bool m_showingSettings = false;
    bool m_mouseTracking = false;
    int  m_srcTab = 0;
    float m_srcTabAnim = 1.0f;

    FadeMode m_fadeMode = FadeMode::None;
    double m_fadeProgress = 0.0;
    float m_gearAngle = 0.0f;
    float m_settingsAnim = 0.0f;
    float m_popupAnim = 0.0f;
    float m_statusAnim = 0.0f;
    float m_recPulsePhase = 0.0f;
    float m_uiDeskMeter = 0.0f;
    float m_uiMicMeter = 0.0f;
    float m_targetDeskMeter = 0.0f;
    float m_targetMicMeter = 0.0f;
    float m_uiDeskPeak = 0.0f;
    float m_uiMicPeak = 0.0f;
    float m_uiDeskPeakHoldSec = 0.0f;
    float m_uiMicPeakHoldSec = 0.0f;
    int64_t m_animLastQpc = 0;
    float m_introAnim = 0.0f;
    float m_inspAnim = 1.0f;
    float m_refreshSpinAngle = 0.0f;
    bool  m_statusFadingOut = false;
    mutable bool m_animNeedTimer = false;
    PopupKind m_lastPopup = PopupKind::None;
    ControlId m_spinningRefreshId = ControlId::None;
    struct FloatAnim {
        float cur = 0.0f;
        float target = 0.0f;
        float speed = 18.0f;
    };
    std::unordered_map<uint64_t, float> m_hoverAnims;
    std::unordered_map<int, float> m_toggleAnims;
    mutable std::unordered_map<int, FloatAnim> m_floatAnims;

    PopupKind m_openPopup = PopupKind::None;
    D2D1_RECT_F m_popupAnchorRc{};
    bool m_popupOpenUp = true;
    D2D1_RECT_F m_popupRc{};
    float m_popupScroll = 0.0f;
    float m_popupMaxScroll = 0.0f;

    float m_overlayListScroll = 0.0f;
    float m_overlayListMaxScroll = 0.0f;
    D2D1_RECT_F m_overlayListRc{};

    UINT m_dpi = 96;
    float m_clientW = 1280.0f;
    float m_clientH = 760.0f;

    HICON m_currentTrayIcon = nullptr;
    HICON m_appIconLarge = nullptr;
    HICON m_appIconSmall = nullptr;
    bool m_lastTrayRec = false;
    bool m_lastTrayPaused = false;

    AppConfig m_config{};
    CaptureEngine     m_capture;
    CameraEngine      m_camera;
    AudioEngine       m_audio;
    OverlayCompositor m_overlays;
    RecorderEngine    m_recorder;
    TrayIconRenderer  m_trayRenderer;

    std::vector<MonitorInfo>      m_monitors;
    std::vector<WindowInfo>       m_windows;
    std::vector<CameraDeviceInfo> m_cameras;
    std::vector<AudioDeviceInfo>  m_outAudioDevs;
    std::vector<AudioDeviceInfo>  m_inAudioDevs;
    std::vector<DetectedEncoder>  m_encoders;

    FramePtr m_previewFramePtr;
    uint64_t m_previewSeqRendered = 0;
    ULONGLONG m_lastRecPreviewTick = 0;
    std::vector<uint8_t> m_previewScaledBuf;
    uint32_t m_previewScaledW = 0;
    uint32_t m_previewScaledH = 0;
    uint64_t m_previewScaledSeq = 0;
    FrameCopy m_camPreviewFrame{};
    uint64_t m_camSeqRendered = 0;

    std::wstring m_actionStatusText;
    bool m_actionStatusError = false;

    std::vector<HitControl> m_hitControls;
    ControlId m_hoverId = ControlId::None;
    int m_hoverArgInt = 0;
    std::wstring m_hoverArgStr;
    ControlId m_pressId = ControlId::None;
    int m_pressArgInt = 0;
    std::wstring m_pressArgStr;
    ControlId m_draggingSliderId = ControlId::None;

    EditFieldId m_activeEditField = EditFieldId::None;
    std::wstring m_editBuffer;
    bool m_editSelectAll = false;
    bool m_caretVisible = true;

    CanvasLayer m_selectedLayer = CanvasLayer::None;
    uint32_t m_selectedOverlayId = 0;
    CanvasDragMode m_canvasDrag = CanvasDragMode::None;
    int m_dragResizeCorner = 3;
    D2D1_RECT_F m_activeDrawRc{};
    D2D1_RECT_F m_activeSourceRc{};
    D2D1_RECT_F m_activeCamRc{};
    D2D1_POINT_2F m_dragStartDip{};
    float m_dragOrigXNorm = 0.0f;
    float m_dragOrigYNorm = 0.0f;
    float m_dragOrigWNorm = 0.2f;
    float m_dragOrigHNorm = 0.1f;
    int m_dragOrigFont = 28;
    int m_dragOrigCropX = 0, m_dragOrigCropY = 0, m_dragOrigCropW = 1280, m_dragOrigCropH = 720;
    float m_dragOrigSrcX = 0.0f, m_dragOrigSrcY = 0.0f, m_dragOrigSrcW = 1.0f, m_dragOrigSrcH = 1.0f;
    float m_dragOrigCamX = 0.74f, m_dragOrigCamY = 0.68f, m_dragOrigCamW = 0.22f;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_hwndRt;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> m_rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;
    Microsoft::WRL::ComPtr<ID2D1Layer> m_animLayer;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> m_roundStroke;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_previewBitmap;
    uint32_t m_previewBmpW = 0;
    uint32_t m_previewBmpH = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_camBitmap;
    uint32_t m_camBmpW = 0;
    uint32_t m_camBmpH = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_logoBitmap;
    std::vector<uint8_t> m_logoPx;
    uint32_t m_logoW = 0;
    uint32_t m_logoH = 0;
    bool m_logoTriedLoad = false;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> m_overlayD2DBmps;

    double m_lastUiDeskMeter = -1.0;
    double m_lastUiMicMeter = -1.0;
    int64_t m_lastUiRecSec = -1;
    int m_lastClockUiSec = -1;
    bool m_lastSyncActive = true;
    std::wstring m_lastTrayTip;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtReg9;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtReg105;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtReg115;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtSemi10;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtSemi11;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBold9;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBold105;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBold115;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBold135;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBold145;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBold16;
};

}
