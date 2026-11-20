#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrec {

enum class OverlayKind {
    Text = 0,
    Clock = 1,
    Box = 2,
    Image = 3
};

struct OverlayItem {
    uint32_t id = 1;
    OverlayKind kind = OverlayKind::Text;
    bool visible = true;
    std::wstring text = L"VRecorder Studio";
    std::wstring imagePath;
    float xNorm = 0.04f;
    float yNorm = 0.05f;
    float boxWidthNorm = 0.22f;
    float boxHeightNorm = 0.12f;
    int fontSize = 28;
    uint32_t colorArgb = 0xFFFFFFFF;
    bool bgEnabled = true;
    uint32_t bgArgb = 0xB809090B;
};

struct AppConfig {

    bool minimizeToTray = true;
    bool closeToTray = true;
    bool alwaysOnTop = false;
    bool showNotifications = true;
    std::wstring theme = L"white";

    bool showPreview = true;
    bool hasCaptureSource = false;
    bool sourceVisible = true;
    int sourceType = 0;
    int monitorIndex = 0;
    std::wstring windowTitleFilter;
    bool showCursor = true;
    int cropX = 0;
    int cropY = 0;
    int cropW = 1280;
    int cropH = 720;

    float srcPosX = 0.0f;
    float srcPosY = 0.0f;
    float srcScaleW = 1.0f;
    float srcScaleH = 1.0f;
    bool srcKeepAspect = true;

    bool colorFilterEnabled = false;
    float filterGamma = 0.0f;
    float filterContrast = 0.0f;
    float filterBrightness = 0.0f;
    float filterSaturation = 0.0f;

    bool camAdded = false;
    bool camEnabled = false;
    std::wstring camDeviceId;
    bool camMirror = true;
    int camShape = 1;
    bool camBorder = true;
    float camXNorm = 0.74f;
    float camYNorm = 0.68f;
    float camWidthNorm = 0.22f;

    std::wstring resolution = L"1080p";
    int customWidth = 1920;
    int customHeight = 1080;
    int fps = 60;
    int bitrateMbps = 18;
    std::wstring encoderId = L"auto";
    std::wstring encoderPreset = L"balanced";
    std::wstring outputDir;
    std::wstring recordHotkey = L"F9";

    bool desktopAudioMuted = false;
    int desktopVolume = 100;
    std::wstring desktopDeviceId;
    bool micMuted = false;
    int micVolume = 100;
    std::wstring micDeviceId;
    int audioBitrateKbps = 192;

    std::vector<OverlayItem> overlays;
    uint32_t nextOverlayId = 1;
};

class ConfigManager {
public:
    static std::wstring GetConfigFilePath();
    static std::wstring GetDefaultOutputDirectory();
    static AppConfig Load();
    static bool Save(const AppConfig& cfg);
};

}
