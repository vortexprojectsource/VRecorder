#include "ConfigManager.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace vrec {

namespace {

constexpr wchar_t kRunSubKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"VRecorder";

bool ParseBool(const wchar_t* val, bool fallback) {
    if (!val) return fallback;
    if (_wcsicmp(val, L"true") == 0 || _wcsicmp(val, L"1") == 0) return true;
    if (_wcsicmp(val, L"false") == 0 || _wcsicmp(val, L"0") == 0) return false;
    return fallback;
}

int ParseInt(const wchar_t* val, int fallback) {
    if (!val || !*val) return fallback;
    wchar_t* end = nullptr;
    long v = wcstol(val, &end, 10);
    if (end == val) return fallback;
    return static_cast<int>(v);
}

uint32_t ParseHexU32(const wchar_t* val, uint32_t fallback) {
    if (!val || !*val) return fallback;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(val, &end, 16);
    if (end == val) return fallback;
    return static_cast<uint32_t>(v);
}

float ParseFloat(const wchar_t* val, float fallback) {
    if (!val || !*val) return fallback;
    wchar_t* end = nullptr;
    double v = wcstod(val, &end);
    if (end == val) return fallback;
    return static_cast<float>(v);
}

void ClearLegacyAutoStartup() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, kRunValueName);
        RegCloseKey(hKey);
    }
}

}

std::wstring ConfigManager::GetConfigFilePath() {
    std::wstring dir;
    PWSTR appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath) {
        dir = appDataPath;
        CoTaskMemFree(appDataPath);
    } else {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        dir = exePath;
    }

    dir += L"\\VRecorder";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\config.xml";
}

std::wstring ConfigManager::GetDefaultOutputDirectory() {
    std::wstring dir;
    PWSTR videosPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videosPath)) && videosPath) {
        dir = videosPath;
        CoTaskMemFree(videosPath);
    } else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &videosPath)) && videosPath) {
        dir = videosPath;
        CoTaskMemFree(videosPath);
    } else {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        dir = exePath;
    }

    dir += L"\\VRecorder";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

AppConfig ConfigManager::Load() {
    ClearLegacyAutoStartup();
    AppConfig cfg{};
    cfg.showPreview = true;
    cfg.outputDir = GetDefaultOutputDirectory();

    const std::wstring path = GetConfigFilePath();
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                      FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream))) {
        return cfg;
    }

    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr))) {
        return cfg;
    }
    reader->SetInput(stream.Get());

    XmlNodeType nodeType = XmlNodeType_None;
    while (reader->Read(&nodeType) == S_OK) {
        if (nodeType != XmlNodeType_Element) continue;
        const wchar_t* elem = nullptr;
        if (FAILED(reader->GetLocalName(&elem, nullptr)) || !elem) continue;

        if (wcscmp(elem, L"Appearance") == 0) {
            if (SUCCEEDED(reader->MoveToFirstAttribute())) {
                do {
                    const wchar_t* name = nullptr;
                    const wchar_t* val = nullptr;
                    if (SUCCEEDED(reader->GetLocalName(&name, nullptr)) &&
                        SUCCEEDED(reader->GetValue(&val, nullptr)) && name && val) {
                        if (wcscmp(name, L"theme") == 0) {
                            cfg.theme = (_wcsicmp(val, L"black") == 0) ? L"black" : L"white";
                        } else if (wcscmp(name, L"minimizeToTray") == 0) {
                            cfg.minimizeToTray = ParseBool(val, cfg.minimizeToTray);
                        } else if (wcscmp(name, L"closeToTray") == 0) {
                            cfg.closeToTray = ParseBool(val, cfg.closeToTray);
                        } else if (wcscmp(name, L"alwaysOnTop") == 0) {
                            cfg.alwaysOnTop = ParseBool(val, cfg.alwaysOnTop);
                        } else if (wcscmp(name, L"showNotifications") == 0) {
                            cfg.showNotifications = ParseBool(val, cfg.showNotifications);
                        }
                    }
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
        } else if (wcscmp(elem, L"Capture") == 0) {
            if (SUCCEEDED(reader->MoveToFirstAttribute())) {
                do {
                    const wchar_t* name = nullptr;
                    const wchar_t* val = nullptr;
                    if (SUCCEEDED(reader->GetLocalName(&name, nullptr)) &&
                        SUCCEEDED(reader->GetValue(&val, nullptr)) && name && val) {
                        if (wcscmp(name, L"showPreview") == 0) cfg.showPreview = ParseBool(val, cfg.showPreview);
                        else if (wcscmp(name, L"hasCaptureSource") == 0) cfg.hasCaptureSource = ParseBool(val, cfg.hasCaptureSource);
                        else if (wcscmp(name, L"sourceVisible") == 0) cfg.sourceVisible = ParseBool(val, cfg.sourceVisible);
                        else if (wcscmp(name, L"sourceType") == 0) cfg.sourceType = std::clamp(ParseInt(val, cfg.sourceType), 0, 2);
                        else if (wcscmp(name, L"monitorIndex") == 0) cfg.monitorIndex = std::max(0, ParseInt(val, cfg.monitorIndex));
                        else if (wcscmp(name, L"windowTitleFilter") == 0) cfg.windowTitleFilter = val;
                        else if (wcscmp(name, L"showCursor") == 0) cfg.showCursor = ParseBool(val, cfg.showCursor);
                        else if (wcscmp(name, L"cropX") == 0) cfg.cropX = std::max(0, ParseInt(val, cfg.cropX));
                        else if (wcscmp(name, L"cropY") == 0) cfg.cropY = std::max(0, ParseInt(val, cfg.cropY));
                        else if (wcscmp(name, L"cropW") == 0) cfg.cropW = std::clamp(ParseInt(val, cfg.cropW), 64, 7680);
                        else if (wcscmp(name, L"cropH") == 0) cfg.cropH = std::clamp(ParseInt(val, cfg.cropH), 64, 4320);
                        else if (wcscmp(name, L"srcPosX") == 0) cfg.srcPosX = std::clamp(ParseFloat(val, cfg.srcPosX), -0.5f, 0.95f);
                        else if (wcscmp(name, L"srcPosY") == 0) cfg.srcPosY = std::clamp(ParseFloat(val, cfg.srcPosY), -0.5f, 0.95f);
                        else if (wcscmp(name, L"srcScaleW") == 0) cfg.srcScaleW = std::clamp(ParseFloat(val, cfg.srcScaleW), 0.10f, 1.5f);
                        else if (wcscmp(name, L"srcScaleH") == 0) cfg.srcScaleH = std::clamp(ParseFloat(val, cfg.srcScaleH), 0.10f, 1.5f);
                        else if (wcscmp(name, L"srcKeepAspect") == 0) cfg.srcKeepAspect = ParseBool(val, cfg.srcKeepAspect);
                    }
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
        } else if (wcscmp(elem, L"ColorFilter") == 0) {
            if (SUCCEEDED(reader->MoveToFirstAttribute())) {
                do {
                    const wchar_t* name = nullptr;
                    const wchar_t* val = nullptr;
                    if (SUCCEEDED(reader->GetLocalName(&name, nullptr)) &&
                        SUCCEEDED(reader->GetValue(&val, nullptr)) && name && val) {
                        if (wcscmp(name, L"enabled") == 0) cfg.colorFilterEnabled = ParseBool(val, cfg.colorFilterEnabled);
                        else if (wcscmp(name, L"gamma") == 0) cfg.filterGamma = std::clamp(ParseFloat(val, cfg.filterGamma), -1.0f, 1.0f);
                        else if (wcscmp(name, L"contrast") == 0) cfg.filterContrast = std::clamp(ParseFloat(val, cfg.filterContrast), -1.0f, 1.0f);
                        else if (wcscmp(name, L"brightness") == 0) cfg.filterBrightness = std::clamp(ParseFloat(val, cfg.filterBrightness), -1.0f, 1.0f);
                        else if (wcscmp(name, L"saturation") == 0) cfg.filterSaturation = std::clamp(ParseFloat(val, cfg.filterSaturation), -1.0f, 2.0f);
                    }
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
        } else if (wcscmp(elem, L"Camera") == 0) {
            if (SUCCEEDED(reader->MoveToFirstAttribute())) {
                do {
                    const wchar_t* name = nullptr;
                    const wchar_t* val = nullptr;
                    if (SUCCEEDED(reader->GetLocalName(&name, nullptr)) &&
                        SUCCEEDED(reader->GetValue(&val, nullptr)) && name && val) {
                        if (wcscmp(name, L"added") == 0) cfg.camAdded = ParseBool(val, cfg.camAdded);
                        else if (wcscmp(name, L"enabled") == 0) cfg.camEnabled = ParseBool(val, cfg.camEnabled);
                        else if (wcscmp(name, L"deviceId") == 0) cfg.camDeviceId = val;
                        else if (wcscmp(name, L"mirror") == 0) cfg.camMirror = ParseBool(val, cfg.camMirror);
                        else if (wcscmp(name, L"shape") == 0) cfg.camShape = std::clamp(ParseInt(val, cfg.camShape), 0, 2);
                        else if (wcscmp(name, L"border") == 0) cfg.camBorder = ParseBool(val, cfg.camBorder);
                        else if (wcscmp(name, L"x") == 0) cfg.camXNorm = std::clamp(ParseFloat(val, cfg.camXNorm), 0.0f, 0.94f);
                        else if (wcscmp(name, L"y") == 0) cfg.camYNorm = std::clamp(ParseFloat(val, cfg.camYNorm), 0.0f, 0.94f);
                        else if (wcscmp(name, L"w") == 0) cfg.camWidthNorm = std::clamp(ParseFloat(val, cfg.camWidthNorm), 0.08f, 0.70f);
                    }
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
        } else if (wcscmp(elem, L"Output") == 0) {
            if (SUCCEEDED(reader->MoveToFirstAttribute())) {
                do {
                    const wchar_t* name = nullptr;
                    const wchar_t* val = nullptr;
                    if (SUCCEEDED(reader->GetLocalName(&name, nullptr)) &&
                        SUCCEEDED(reader->GetValue(&val, nullptr)) && name && val) {
                        if (wcscmp(name, L"resolution") == 0) {
                            if (_wcsicmp(val, L"native") == 0 || _wcsicmp(val, L"1440p") == 0 ||
                                _wcsicmp(val, L"1080p") == 0 || _wcsicmp(val, L"720p") == 0 ||
                                _wcsicmp(val, L"vertical") == 0 || _wcsicmp(val, L"custom") == 0) {
                                cfg.resolution = val;
                            }
                        } else if (wcscmp(name, L"customWidth") == 0) {
                            cfg.customWidth = std::clamp(ParseInt(val, cfg.customWidth), 64, 3840) & ~1;
                        } else if (wcscmp(name, L"customHeight") == 0) {
                            cfg.customHeight = std::clamp(ParseInt(val, cfg.customHeight), 64, 2160) & ~1;
                        } else if (wcscmp(name, L"fps") == 0) {
                            const int f = ParseInt(val, cfg.fps);
                            cfg.fps = (f == 30 || f == 60 || f == 120) ? f : 60;
                        } else if (wcscmp(name, L"bitrateMbps") == 0) {
                            cfg.bitrateMbps = std::clamp(ParseInt(val, cfg.bitrateMbps), 4, 80);
                        } else if (wcscmp(name, L"encoderId") == 0 && *val) {
                            cfg.encoderId = val;
                        } else if (wcscmp(name, L"encoderPreset") == 0 && *val) {
                            cfg.encoderPreset = val;
                        } else if (wcscmp(name, L"outputDir") == 0 && *val) {
                            cfg.outputDir = val;
                        } else if (wcscmp(name, L"recordHotkey") == 0 && *val) {
                            cfg.recordHotkey = val;
                        }
                    }
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
        } else if (wcscmp(elem, L"Audio") == 0) {
            if (SUCCEEDED(reader->MoveToFirstAttribute())) {
                do {
                    const wchar_t* name = nullptr;
                    const wchar_t* val = nullptr;
                    if (SUCCEEDED(reader->GetLocalName(&name, nullptr)) &&
                        SUCCEEDED(reader->GetValue(&val, nullptr)) && name && val) {
                        if (wcscmp(name, L"desktopAudioMuted") == 0) cfg.desktopAudioMuted = ParseBool(val, cfg.desktopAudioMuted);
                        else if (wcscmp(name, L"desktopVolume") == 0) cfg.desktopVolume = std::clamp(ParseInt(val, cfg.desktopVolume), 0, 100);
                        else if (wcscmp(name, L"desktopDeviceId") == 0) cfg.desktopDeviceId = val;
                        else if (wcscmp(name, L"micMuted") == 0) cfg.micMuted = ParseBool(val, cfg.micMuted);
                        else if (wcscmp(name, L"micVolume") == 0) cfg.micVolume = std::clamp(ParseInt(val, cfg.micVolume), 0, 100);
                        else if (wcscmp(name, L"micDeviceId") == 0) cfg.micDeviceId = val;
                        else if (wcscmp(name, L"audioBitrateKbps") == 0) {
                            const int ab = ParseInt(val, cfg.audioBitrateKbps);
                            cfg.audioBitrateKbps = (ab >= 256) ? 320 : 192;
                        }
                    }
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
        } else if (wcscmp(elem, L"Overlay") == 0) {
            OverlayItem ov{};
            ov.id = cfg.nextOverlayId++;
            if (SUCCEEDED(reader->MoveToFirstAttribute())) {
                do {
                    const wchar_t* name = nullptr;
                    const wchar_t* val = nullptr;
                    if (SUCCEEDED(reader->GetLocalName(&name, nullptr)) &&
                        SUCCEEDED(reader->GetValue(&val, nullptr)) && name && val) {
                        if (wcscmp(name, L"kind") == 0) {
                            ov.kind = static_cast<OverlayKind>(std::clamp(ParseInt(val, 0), 0, 3));
                        } else if (wcscmp(name, L"visible") == 0) {
                            ov.visible = ParseBool(val, true);
                        } else if (wcscmp(name, L"text") == 0) {
                            ov.text = val;
                        } else if (wcscmp(name, L"imagePath") == 0) {
                            ov.imagePath = val;
                        } else if (wcscmp(name, L"x") == 0) {
                            ov.xNorm = std::clamp(ParseFloat(val, 0.04f), 0.0f, 0.95f);
                        } else if (wcscmp(name, L"y") == 0) {
                            ov.yNorm = std::clamp(ParseFloat(val, 0.05f), 0.0f, 0.95f);
                        } else if (wcscmp(name, L"bw") == 0) {
                            ov.boxWidthNorm = std::clamp(ParseFloat(val, 0.22f), 0.04f, 1.0f);
                        } else if (wcscmp(name, L"bh") == 0) {
                            ov.boxHeightNorm = std::clamp(ParseFloat(val, 0.12f), 0.04f, 1.0f);
                        } else if (wcscmp(name, L"fontSize") == 0) {
                            ov.fontSize = std::clamp(ParseInt(val, 28), 12, 96);
                        } else if (wcscmp(name, L"color") == 0) {
                            ov.colorArgb = ParseHexU32(val, 0xFFFFFFFF);
                        } else if (wcscmp(name, L"bg") == 0) {
                            ov.bgEnabled = ParseBool(val, true);
                        } else if (wcscmp(name, L"bgColor") == 0) {
                            ov.bgArgb = ParseHexU32(val, 0xB809090B);
                        }
                    }
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
            cfg.overlays.push_back(std::move(ov));
        }
    }

    if (cfg.camEnabled) cfg.camAdded = true;
    cfg.showPreview = true;
    if (cfg.outputDir.empty()) {
        cfg.outputDir = GetDefaultOutputDirectory();
    } else {
        SHCreateDirectoryExW(nullptr, cfg.outputDir.c_str(), nullptr);
    }
    return cfg;
}

bool ConfigManager::Save(const AppConfig& cfg) {
    const std::wstring path = GetConfigFilePath();
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(),
                                      STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                      FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) {
        return false;
    }

    ComPtr<IXmlWriter> writer;
    if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), reinterpret_cast<void**>(writer.GetAddressOf()), nullptr))) {
        return false;
    }
    writer->SetOutput(stream.Get());
    writer->SetProperty(XmlWriterProperty_Indent, TRUE);
    writer->WriteStartDocument(XmlStandalone_Omit);
    writer->WriteStartElement(nullptr, L"VRecorderConfig", nullptr);

    auto bstr = [](bool v) { return v ? L"true" : L"false"; };
    auto istr = [](int v) { return std::to_wstring(v); };

    writer->WriteStartElement(nullptr, L"Appearance", nullptr);
    writer->WriteAttributeString(nullptr, L"theme", nullptr, cfg.theme.c_str());
    writer->WriteAttributeString(nullptr, L"minimizeToTray", nullptr, bstr(cfg.minimizeToTray));
    writer->WriteAttributeString(nullptr, L"closeToTray", nullptr, bstr(cfg.closeToTray));
    writer->WriteAttributeString(nullptr, L"alwaysOnTop", nullptr, bstr(cfg.alwaysOnTop));
    writer->WriteAttributeString(nullptr, L"showNotifications", nullptr, bstr(cfg.showNotifications));
    writer->WriteEndElement();

    auto fstr = [](float v) {
        wchar_t b[24]{};
        std::swprintf(b, 24, L"%.4f", v);
        return std::wstring(b);
    };

    writer->WriteStartElement(nullptr, L"Capture", nullptr);
    writer->WriteAttributeString(nullptr, L"showPreview", nullptr, bstr(cfg.showPreview));
    writer->WriteAttributeString(nullptr, L"hasCaptureSource", nullptr, bstr(cfg.hasCaptureSource));
    writer->WriteAttributeString(nullptr, L"sourceVisible", nullptr, bstr(cfg.sourceVisible));
    writer->WriteAttributeString(nullptr, L"sourceType", nullptr, istr(cfg.sourceType).c_str());
    writer->WriteAttributeString(nullptr, L"monitorIndex", nullptr, istr(cfg.monitorIndex).c_str());
    writer->WriteAttributeString(nullptr, L"windowTitleFilter", nullptr, cfg.windowTitleFilter.c_str());
    writer->WriteAttributeString(nullptr, L"showCursor", nullptr, bstr(cfg.showCursor));
    writer->WriteAttributeString(nullptr, L"cropX", nullptr, istr(cfg.cropX).c_str());
    writer->WriteAttributeString(nullptr, L"cropY", nullptr, istr(cfg.cropY).c_str());
    writer->WriteAttributeString(nullptr, L"cropW", nullptr, istr(cfg.cropW).c_str());
    writer->WriteAttributeString(nullptr, L"cropH", nullptr, istr(cfg.cropH).c_str());
    writer->WriteAttributeString(nullptr, L"srcPosX", nullptr, fstr(cfg.srcPosX).c_str());
    writer->WriteAttributeString(nullptr, L"srcPosY", nullptr, fstr(cfg.srcPosY).c_str());
    writer->WriteAttributeString(nullptr, L"srcScaleW", nullptr, fstr(cfg.srcScaleW).c_str());
    writer->WriteAttributeString(nullptr, L"srcScaleH", nullptr, fstr(cfg.srcScaleH).c_str());
    writer->WriteAttributeString(nullptr, L"srcKeepAspect", nullptr, bstr(cfg.srcKeepAspect));
    writer->WriteEndElement();

    writer->WriteStartElement(nullptr, L"ColorFilter", nullptr);
    writer->WriteAttributeString(nullptr, L"enabled", nullptr, bstr(cfg.colorFilterEnabled));
    writer->WriteAttributeString(nullptr, L"gamma", nullptr, fstr(cfg.filterGamma).c_str());
    writer->WriteAttributeString(nullptr, L"contrast", nullptr, fstr(cfg.filterContrast).c_str());
    writer->WriteAttributeString(nullptr, L"brightness", nullptr, fstr(cfg.filterBrightness).c_str());
    writer->WriteAttributeString(nullptr, L"saturation", nullptr, fstr(cfg.filterSaturation).c_str());
    writer->WriteEndElement();

    writer->WriteStartElement(nullptr, L"Camera", nullptr);
    writer->WriteAttributeString(nullptr, L"added", nullptr, bstr(cfg.camAdded));
    writer->WriteAttributeString(nullptr, L"enabled", nullptr, bstr(cfg.camEnabled));
    writer->WriteAttributeString(nullptr, L"deviceId", nullptr, cfg.camDeviceId.c_str());
    writer->WriteAttributeString(nullptr, L"mirror", nullptr, bstr(cfg.camMirror));
    writer->WriteAttributeString(nullptr, L"shape", nullptr, istr(cfg.camShape).c_str());
    writer->WriteAttributeString(nullptr, L"border", nullptr, bstr(cfg.camBorder));
    writer->WriteAttributeString(nullptr, L"x", nullptr, fstr(cfg.camXNorm).c_str());
    writer->WriteAttributeString(nullptr, L"y", nullptr, fstr(cfg.camYNorm).c_str());
    writer->WriteAttributeString(nullptr, L"w", nullptr, fstr(cfg.camWidthNorm).c_str());
    writer->WriteEndElement();

    writer->WriteStartElement(nullptr, L"Output", nullptr);
    writer->WriteAttributeString(nullptr, L"resolution", nullptr, cfg.resolution.c_str());
    writer->WriteAttributeString(nullptr, L"customWidth", nullptr, istr(cfg.customWidth).c_str());
    writer->WriteAttributeString(nullptr, L"customHeight", nullptr, istr(cfg.customHeight).c_str());
    writer->WriteAttributeString(nullptr, L"fps", nullptr, istr(cfg.fps).c_str());
    writer->WriteAttributeString(nullptr, L"bitrateMbps", nullptr, istr(cfg.bitrateMbps).c_str());
    writer->WriteAttributeString(nullptr, L"encoderId", nullptr, cfg.encoderId.c_str());
    writer->WriteAttributeString(nullptr, L"encoderPreset", nullptr, cfg.encoderPreset.c_str());
    writer->WriteAttributeString(nullptr, L"outputDir", nullptr, cfg.outputDir.c_str());
    writer->WriteAttributeString(nullptr, L"recordHotkey", nullptr, cfg.recordHotkey.c_str());
    writer->WriteEndElement();

    writer->WriteStartElement(nullptr, L"Audio", nullptr);
    writer->WriteAttributeString(nullptr, L"desktopAudioMuted", nullptr, bstr(cfg.desktopAudioMuted));
    writer->WriteAttributeString(nullptr, L"desktopVolume", nullptr, istr(cfg.desktopVolume).c_str());
    writer->WriteAttributeString(nullptr, L"desktopDeviceId", nullptr, cfg.desktopDeviceId.c_str());
    writer->WriteAttributeString(nullptr, L"micMuted", nullptr, bstr(cfg.micMuted));
    writer->WriteAttributeString(nullptr, L"micVolume", nullptr, istr(cfg.micVolume).c_str());
    writer->WriteAttributeString(nullptr, L"micDeviceId", nullptr, cfg.micDeviceId.c_str());
    writer->WriteAttributeString(nullptr, L"audioBitrateKbps", nullptr, istr(cfg.audioBitrateKbps).c_str());
    writer->WriteEndElement();

    writer->WriteStartElement(nullptr, L"Overlays", nullptr);
    for (const auto& ov : cfg.overlays) {
        wchar_t xBuf[16]{}, yBuf[16]{}, bwBuf[16]{}, bhBuf[16]{}, clrBuf[16]{}, bgBuf[16]{};
        std::swprintf(xBuf, 16, L"%.4f", ov.xNorm);
        std::swprintf(yBuf, 16, L"%.4f", ov.yNorm);
        std::swprintf(bwBuf, 16, L"%.4f", ov.boxWidthNorm);
        std::swprintf(bhBuf, 16, L"%.4f", ov.boxHeightNorm);
        std::swprintf(clrBuf, 16, L"%08X", ov.colorArgb);
        std::swprintf(bgBuf, 16, L"%08X", ov.bgArgb);

        writer->WriteStartElement(nullptr, L"Overlay", nullptr);
        writer->WriteAttributeString(nullptr, L"kind", nullptr, istr(static_cast<int>(ov.kind)).c_str());
        writer->WriteAttributeString(nullptr, L"visible", nullptr, bstr(ov.visible));
        writer->WriteAttributeString(nullptr, L"text", nullptr, ov.text.c_str());
        writer->WriteAttributeString(nullptr, L"imagePath", nullptr, ov.imagePath.c_str());
        writer->WriteAttributeString(nullptr, L"x", nullptr, xBuf);
        writer->WriteAttributeString(nullptr, L"y", nullptr, yBuf);
        writer->WriteAttributeString(nullptr, L"bw", nullptr, bwBuf);
        writer->WriteAttributeString(nullptr, L"bh", nullptr, bhBuf);
        writer->WriteAttributeString(nullptr, L"fontSize", nullptr, istr(ov.fontSize).c_str());
        writer->WriteAttributeString(nullptr, L"color", nullptr, clrBuf);
        writer->WriteAttributeString(nullptr, L"bg", nullptr, bstr(ov.bgEnabled));
        writer->WriteAttributeString(nullptr, L"bgColor", nullptr, bgBuf);
        writer->WriteEndElement();
    }
    writer->WriteEndElement();

    writer->WriteEndDocument();
    writer->Flush();
    return true;
}

}
