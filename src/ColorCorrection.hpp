#pragma once

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>

namespace vrec {

struct ColorFilterParams {
    bool enabled = false;
    float gamma = 0.0f;
    float contrast = 0.0f;
    float brightness = 0.0f;
    float saturation = 0.0f;

    bool HasEffect() const {
        return enabled && (std::abs(gamma) > 0.001f || std::abs(contrast) > 0.001f ||
                           std::abs(brightness) > 0.001f || std::abs(saturation) > 0.001f);
    }
};

class ColorCorrection {
public:
    static void BuildLut(const ColorFilterParams& params, uint8_t lut[256]) {

        const float gammaExp = (params.gamma >= 0.0f)
            ? (1.0f / (1.0f + params.gamma * 2.0f))
            : (1.0f - params.gamma * 2.0f);

        const float contrastFactor = (params.contrast >= 0.0f)
            ? (1.0f + params.contrast * 2.0f)
            : std::max(0.0f, 1.0f + params.contrast);

        const float brightnessOffset = params.brightness;

        for (int i = 0; i < 256; ++i) {
            float v = static_cast<float>(i) / 255.0f;

            if (gammaExp != 1.0f && v > 0.0f) {
                v = std::pow(v, gammaExp);
            }

            v = (v - 0.5f) * contrastFactor + 0.5f;

            v += brightnessOffset;

            int iv = static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            lut[i] = static_cast<uint8_t>(std::clamp(iv, 0, 255));
        }
    }

    static void BuildUvLut(const ColorFilterParams& params, uint8_t lut[256]) {

        const float satFactor = std::max(0.0f, 1.0f + params.saturation);
        for (int i = 0; i < 256; ++i) {
            const float diff = static_cast<float>(i) - 128.0f;
            const float v = 128.0f + diff * satFactor;
            lut[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
        }
    }

    static void ApplyNv12(uint8_t* nv12Data, uint32_t width, uint32_t height, const ColorFilterParams& params) {
        if (!params.HasEffect() || !nv12Data || width == 0 || height == 0) return;

        const bool hasLut = (std::abs(params.gamma) > 0.001f ||
                             std::abs(params.contrast) > 0.001f ||
                             std::abs(params.brightness) > 0.001f);
        const bool hasSat = (std::abs(params.saturation) > 0.001f);

        const size_t yBytes = static_cast<size_t>(width) * height;
        if (hasLut) {
            uint8_t lutY[256];
            BuildLut(params, lutY);
            uint8_t* yp = nv12Data;
            for (size_t i = 0; i < yBytes; ++i) {
                yp[i] = lutY[yp[i]];
            }
        }

        if (hasSat) {
            uint8_t lutUV[256];
            BuildUvLut(params, lutUV);
            uint8_t* uvp = nv12Data + yBytes;
            const size_t uvBytes = yBytes / 2;
            for (size_t i = 0; i < uvBytes; ++i) {
                uvp[i] = lutUV[uvp[i]];
            }
        }
    }

    static void ApplyBgra(uint8_t* bgra, size_t pixelCount, const ColorFilterParams& params) {
        if (!params.HasEffect() || !bgra || pixelCount == 0) return;

        const bool hasLut = (std::abs(params.gamma) > 0.001f ||
                             std::abs(params.contrast) > 0.001f ||
                             std::abs(params.brightness) > 0.001f);
        const bool hasSat = (std::abs(params.saturation) > 0.001f);

        uint8_t lut[256];
        if (hasLut) {
            BuildLut(params, lut);
        }

        const float satFactor = std::max(0.0f, 1.0f + params.saturation);
        const int32_t satFixed = static_cast<int32_t>(std::lround(satFactor * 256.0f));

        uint8_t* p = bgra;
        for (size_t i = 0; i < pixelCount; ++i, p += 4) {
            uint8_t b = p[0];
            uint8_t g = p[1];
            uint8_t r = p[2];

            if (hasLut) {
                b = lut[b];
                g = lut[g];
                r = lut[r];
            }

            if (hasSat) {

                const int32_t y = (54 * r + 183 * g + 19 * b) >> 8;
                const int32_t newB = y + (((static_cast<int32_t>(b) - y) * satFixed) >> 8);
                const int32_t newG = y + (((static_cast<int32_t>(g) - y) * satFixed) >> 8);
                const int32_t newR = y + (((static_cast<int32_t>(r) - y) * satFixed) >> 8);
                p[0] = static_cast<uint8_t>(std::clamp(newB, 0, 255));
                p[1] = static_cast<uint8_t>(std::clamp(newG, 0, 255));
                p[2] = static_cast<uint8_t>(std::clamp(newR, 0, 255));
            } else {
                p[0] = b;
                p[1] = g;
                p[2] = r;
            }
        }
    }
};

}
