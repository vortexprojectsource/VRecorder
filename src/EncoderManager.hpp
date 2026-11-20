#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vrec {

struct DetectedEncoder {
    std::wstring id;
    CLSID clsid = GUID_NULL;
    GUID subtype = MFVideoFormat_H264;
    std::wstring friendlyName;
    std::wstring rawName;
    std::wstring badge;
    bool isHardware = false;
    bool isHevc = false;
    bool hasLuid = false;
    uint64_t adapterLuid = 0;
};

struct EncoderPreset {
    std::wstring id;
    std::wstring name;
    std::wstring shortLabel;
    uint32_t qualityVsSpeed = 50;
    uint32_t qualityLevel = 78;
    int bFrames = 0;
    bool lowLatency = false;
    uint32_t rateControlMode = 1;
    double bitrateMultiplier = 1.0;
};

class EncoderManager {
public:

    static std::vector<DetectedEncoder> EnumerateAvailableEncoders();

    static DetectedEncoder ResolveSelection(const std::vector<DetectedEncoder>& list, const std::wstring& encoderId);

    static std::vector<EncoderPreset> GetPresetsForEncoder(const DetectedEncoder& enc);

    static EncoderPreset ResolvePreset(const DetectedEncoder& enc, const std::wstring& presetId);
};

}
