#pragma once

#include "CaptureEngine.hpp"
#include "ConfigManager.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vrec {

class OverlayCompositor {
public:
    OverlayCompositor();
    ~OverlayCompositor();

    OverlayCompositor(const OverlayCompositor&) = delete;
    OverlayCompositor& operator=(const OverlayCompositor&) = delete;

    void SetOverlays(const std::vector<OverlayItem>& items);
    std::vector<OverlayItem> GetOverlays() const;
    bool HasVisibleOverlays() const;

    void CompositeOntoFrame(std::vector<uint8_t>& frameBgra, uint32_t outW, uint32_t outH);

    void CompositeOntoNv12(std::vector<uint8_t>& nv12, uint32_t outW, uint32_t outH);

    static void CompositeCameraOntoFrame(std::vector<uint8_t>& frameBgra, uint32_t outW, uint32_t outH,
                                         const FrameCopy& camFrame, const AppConfig& cfg);

    static void CompositeCameraOntoNv12(std::vector<uint8_t>& nv12, uint32_t outW, uint32_t outH,
                                        const FrameCopy& camFrame, const AppConfig& cfg);

    static std::wstring FormatCurrentClockText();

private:
    struct CachedSprite {
        uint32_t w = 0;
        uint32_t h = 0;
        std::wstring signature;
        std::vector<uint8_t> pbgra;
    };

    struct DecodedImage {
        uint32_t w = 0;
        uint32_t h = 0;
        std::vector<uint8_t> pbgra;
    };

    bool RasterizeSprite(const OverlayItem& item, const std::wstring& resolvedText,
                         uint32_t outW, uint32_t outH, CachedSprite& sprite);

    mutable std::mutex m_mutex;
    std::vector<OverlayItem> m_items;
    std::unordered_map<uint32_t, CachedSprite> m_spriteCache;
    std::unordered_map<std::wstring, DecodedImage> m_imageCache;
};

}
