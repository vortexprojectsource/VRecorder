#include "RecorderEngine.hpp"
#include "ColorCorrection.hpp"

#include <codecapi.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <emmintrin.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <shlobj.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace vrec {

namespace {

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

std::wstring BuildTimestampMp4Path(const std::wstring& folder) {
    std::wstring dir = folder;
    if (dir.empty()) dir = ConfigManager::GetDefaultOutputDirectory();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
        dir.pop_back();
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[96]{};
    std::swprintf(name, 96, L"\\VRecorder_%04u-%02u-%02u_%02u-%02u-%02u.mp4",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return dir + name;
}

struct BicTap4 {
    uint32_t idx[4]{};
    int16_t  w[4]{};
};

inline double KeysCubicWeight(double x, double a = -0.75) noexcept {
    x = std::abs(x);
    if (x <= 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0)  return ((a * x - 5.0 * a) * x + 8.0 * a) * x - 4.0 * a;
    return 0.0;
}

void BuildBicubicAxisLut(uint32_t cropStart, uint32_t cropSize, int visStart,
                         int dstStart, int dstSize, int visSize, std::vector<BicTap4>& outLut) {
    outLut.resize(static_cast<size_t>(std::max(0, visSize)));
    if (visSize <= 0 || cropSize == 0 || dstSize <= 0) return;
    const uint32_t maxSrc = cropStart + cropSize - 1;
    for (int i = 0; i < visSize; ++i) {
        const double tc = ((static_cast<double>((visStart + i) - dstStart) + 0.5) *
                           static_cast<double>(cropSize) / static_cast<double>(dstSize)) - 0.5;
        const int b = static_cast<int>(std::floor(tc));
        const double f = tc - static_cast<double>(b);
        double fw[4]{};
        double sum = 0.0;
        for (int k = 0; k < 4; ++k) {
            const int sc = std::clamp(static_cast<int>(cropStart) + b - 1 + k,
                                      static_cast<int>(cropStart), static_cast<int>(maxSrc));
            outLut[static_cast<size_t>(i)].idx[k] = static_cast<uint32_t>(sc);
            fw[k] = KeysCubicWeight(static_cast<double>(k - 1) - f, -0.75);
            sum += fw[k];
        }
        int tot = 0;
        for (int k = 0; k < 4; ++k) {
            const int16_t q = static_cast<int16_t>(std::lround((fw[k] / sum) * 256.0));
            outLut[static_cast<size_t>(i)].w[k] = q;
            tot += q;
        }
        outLut[static_cast<size_t>(i)].w[1] = static_cast<int16_t>(outLut[static_cast<size_t>(i)].w[1] + (256 - tot));
    }
}

struct ScalePlan {
    uint32_t srcW = 0, srcH = 0;
    uint32_t cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    uint32_t outW = 0, outH = 0;
    float posX = 0.0f, posY = 0.0f, scaleW = 1.0f, scaleH = 1.0f;
    bool keepAspect = true;
    bool isUnscaled1to1 = false;
    int visX0 = 0, visY0 = 0, visW = 0, visH = 0;
    std::vector<BicTap4> bicX;
    std::vector<BicTap4> bicY;

    void Rebuild(uint32_t sw, uint32_t sh, const AppConfig& cfg, uint32_t ow, uint32_t oh) {
        sw = std::max<uint32_t>(1, sw);
        sh = std::max<uint32_t>(1, sh);
        uint32_t nx = 0, ny = 0, nw = sw, nh = sh;
        if (cfg.sourceType == 2) {
            nx = static_cast<uint32_t>(std::clamp(cfg.cropX, 0, static_cast<int>(sw - 1)));
            ny = static_cast<uint32_t>(std::clamp(cfg.cropY, 0, static_cast<int>(sh - 1)));
            nw = static_cast<uint32_t>(std::clamp(cfg.cropW, 2, static_cast<int>(sw - nx)));
            nh = static_cast<uint32_t>(std::clamp(cfg.cropH, 2, static_cast<int>(sh - ny)));
        }

        if (sw == srcW && sh == srcH && nx == cropX && ny == cropY &&
            nw == cropW && nh == cropH && ow == outW && oh == outH &&
            posX == cfg.srcPosX && posY == cfg.srcPosY &&
            scaleW == cfg.srcScaleW && scaleH == cfg.srcScaleH &&
            keepAspect == cfg.srcKeepAspect && (!bicX.empty() || visW == 0)) {
            return;
        }

        srcW = sw; srcH = sh;
        cropX = nx; cropY = ny; cropW = nw; cropH = nh;
        outW = std::max<uint32_t>(64, ow);
        outH = std::max<uint32_t>(64, oh);
        posX = cfg.srcPosX; posY = cfg.srcPosY;
        scaleW = cfg.srcScaleW; scaleH = cfg.srcScaleH;
        keepAspect = cfg.srcKeepAspect;

        float fx = 0.0f, fy = 0.0f, fw = 0.0f, fh = 0.0f;
        RecorderEngine::ComputeSourcePlacement(outW, outH, cropW, cropH, cfg, fx, fy, fw, fh);

        const int dstX = static_cast<int>(std::lround(fx));
        const int dstY = static_cast<int>(std::lround(fy));
        const int dstW = std::max(2, static_cast<int>(std::lround(fw)));
        const int dstH = std::max(2, static_cast<int>(std::lround(fh)));

        visX0 = std::clamp(dstX, 0, static_cast<int>(outW));
        visY0 = std::clamp(dstY, 0, static_cast<int>(outH));
        const int visX1 = std::clamp(dstX + dstW, 0, static_cast<int>(outW));
        const int visY1 = std::clamp(dstY + dstH, 0, static_cast<int>(outH));
        visW = std::max(0, visX1 - visX0);
        visH = std::max(0, visY1 - visY0);
        isUnscaled1to1 = (dstW == static_cast<int>(cropW)) && (dstH == static_cast<int>(cropH));

        BuildBicubicAxisLut(cropX, cropW, visX0, dstX, dstW, visW, bicX);
        BuildBicubicAxisLut(cropY, cropH, visY0, dstY, dstH, visH, bicY);
    }
};

class BandWorkerPool {
public:
    explicit BandWorkerPool(size_t totalWorkers = 4) {
        const size_t helpers = totalWorkers > 1 ? (totalWorkers - 1) : 0;
        m_threads.reserve(helpers);
        for (size_t i = 0; i < helpers; ++i) {
            m_threads.emplace_back([this, idx = i]() { WorkerLoop(idx); });
        }
    }

    ~BandWorkerPool() {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_stop = true;
            ++m_epoch;
        }
        m_cvWork.notify_all();
        for (auto& t : m_threads) {
            if (t.joinable()) t.join();
        }
    }

    template <typename Fn>
    void Dispatch(Fn&& fn) {
        if (m_threads.empty()) {
            fn(0, 1);
            return;
        }
        using Decayed = std::decay_t<Fn>;
        auto thunk = [](void* p, size_t wi, size_t wn) {
            (*static_cast<Decayed*>(p))(wi, wn);
        };
        const size_t total = m_threads.size() + 1;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_taskFn = thunk;
            m_taskCtx = const_cast<Decayed*>(&fn);
            m_remaining = m_threads.size();
            ++m_epoch;
        }
        m_cvWork.notify_all();
        fn(m_threads.size(), total);
        std::unique_lock<std::mutex> lk(m_mu);
        m_cvDone.wait(lk, [this]() { return m_remaining == 0; });
        m_taskFn = nullptr;
        m_taskCtx = nullptr;
    }

private:
    using TaskFn = void (*)(void*, size_t, size_t);

    void WorkerLoop(size_t idx) {
        uint64_t seenEpoch = 0;
        const size_t total = m_threads.size() + 1;
        for (;;) {
            TaskFn fn = nullptr;
            void* ctx = nullptr;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cvWork.wait(lk, [this, &seenEpoch]() { return m_stop || m_epoch != seenEpoch; });
                if (m_stop) return;
                seenEpoch = m_epoch;
                fn = m_taskFn;
                ctx = m_taskCtx;
            }
            if (fn && ctx) fn(ctx, idx, total);
            {
                std::lock_guard<std::mutex> lk(m_mu);
                if (--m_remaining == 0) m_cvDone.notify_one();
            }
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mu;
    std::condition_variable m_cvWork;
    std::condition_variable m_cvDone;
    TaskFn m_taskFn = nullptr;
    void* m_taskCtx = nullptr;
    uint64_t m_epoch = 0;
    size_t m_remaining = 0;
    bool m_stop = false;
};

void RenderScaledOutputBgra(const uint8_t* srcBgra, const ScalePlan& plan,
                            std::vector<uint8_t>& outBgra, BandWorkerPool& pool) {
    const size_t totalBytes = static_cast<size_t>(plan.outW) * plan.outH * 4;
    if (outBgra.size() != totalBytes) outBgra.resize(totalBytes);

    if (srcBgra && plan.visX0 == 0 && plan.visY0 == 0 &&
        plan.visW == static_cast<int>(plan.outW) && plan.visH == static_cast<int>(plan.outH) &&
        plan.cropX == 0 && plan.cropY == 0 &&
        plan.cropW == plan.outW && plan.cropH == plan.outH &&
        plan.srcW == plan.outW && plan.srcH == plan.outH) {
        std::memcpy(outBgra.data(), srcBgra, totalBytes);
        return;
    }

    if (!srcBgra || plan.visW <= 0 || plan.visH <= 0) {
        auto* dst32 = reinterpret_cast<uint32_t*>(outBgra.data());
        std::fill_n(dst32, static_cast<size_t>(plan.outW) * plan.outH, 0xFF090909u);
        return;
    }

    const size_t srcStride = static_cast<size_t>(plan.srcW) * 4;
    const size_t dstStride = static_cast<size_t>(plan.outW) * 4;
    const int visYEnd = plan.visY0 + plan.visH;
    const int visXEnd = plan.visX0 + plan.visW;
    const int rightPad = static_cast<int>(plan.outW) - visXEnd;
    const uint32_t directSrcX0 = (plan.isUnscaled1to1 && !plan.bicX.empty()) ? plan.bicX[0].idx[1] : plan.cropX;

    pool.Dispatch([&](size_t wi, size_t wn) {
        thread_local uint32_t t_srcY[4]{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
        thread_local std::vector<int16_t> t_hRows[4];
        for (int k = 0; k < 4; ++k) {
            t_srcY[k] = UINT32_MAX;
            if (!plan.isUnscaled1to1 && t_hRows[k].size() < static_cast<size_t>(plan.visW) * 3) {
                t_hRows[k].resize(static_cast<size_t>(plan.visW) * 3);
            }
        }
        uint32_t nextSlot = 0;

        auto getHRow = [&](uint32_t sy) -> const int16_t* {
            for (int k = 0; k < 4; ++k) {
                if (t_srcY[k] == sy) return t_hRows[k].data();
            }
            const uint32_t slot = (nextSlot++) & 3u;
            t_srcY[slot] = sy;
            int16_t* dstH = t_hRows[slot].data();
            const uint8_t* sr = srcBgra + static_cast<size_t>(sy) * srcStride;
            const BicTap4* xTaps = plan.bicX.data();
            for (int x = 0; x < plan.visW; ++x) {
                const BicTap4& tx = xTaps[x];
                const uint8_t* p0 = sr + static_cast<size_t>(tx.idx[0]) * 4;
                const uint8_t* p1 = sr + static_cast<size_t>(tx.idx[1]) * 4;
                const uint8_t* p2 = sr + static_cast<size_t>(tx.idx[2]) * 4;
                const uint8_t* p3 = sr + static_cast<size_t>(tx.idx[3]) * 4;
                dstH[x * 3 + 0] = static_cast<int16_t>((p0[0] * tx.w[0] + p1[0] * tx.w[1] + p2[0] * tx.w[2] + p3[0] * tx.w[3]) >> 4);
                dstH[x * 3 + 1] = static_cast<int16_t>((p0[1] * tx.w[0] + p1[1] * tx.w[1] + p2[1] * tx.w[2] + p3[1] * tx.w[3]) >> 4);
                dstH[x * 3 + 2] = static_cast<int16_t>((p0[2] * tx.w[0] + p1[2] * tx.w[1] + p2[2] * tx.w[2] + p3[2] * tx.w[3]) >> 4);
            }
            return dstH;
        };

        const uint32_t yBeg = static_cast<uint32_t>((static_cast<uint64_t>(plan.outH) * wi) / wn);
        const uint32_t yEnd = static_cast<uint32_t>((static_cast<uint64_t>(plan.outH) * (wi + 1)) / wn);
        for (uint32_t oy = yBeg; oy < yEnd; ++oy) {
            uint8_t* dRow = outBgra.data() + static_cast<size_t>(oy) * dstStride;
            auto* d32 = reinterpret_cast<uint32_t*>(dRow);
            const int iy = static_cast<int>(oy);
            if (iy < plan.visY0 || iy >= visYEnd) {
                std::fill_n(d32, plan.outW, 0xFF090909u);
                continue;
            }
            if (plan.visX0 > 0) std::fill_n(d32, plan.visX0, 0xFF090909u);
            if (rightPad > 0)   std::fill_n(d32 + visXEnd, rightPad, 0xFF090909u);

            const size_t yi = static_cast<size_t>(iy - plan.visY0);
            const BicTap4& ty = plan.bicY[yi];
            if (plan.isUnscaled1to1) {
                const uint8_t* sRow = srcBgra + static_cast<size_t>(ty.idx[1]) * srcStride + static_cast<size_t>(directSrcX0) * 4;
                std::memcpy(dRow + static_cast<size_t>(plan.visX0) * 4, sRow, static_cast<size_t>(plan.visW) * 4);
            } else {
                const int16_t* r0 = getHRow(ty.idx[0]);
                const int16_t* r1 = getHRow(ty.idx[1]);
                const int16_t* r2 = getHRow(ty.idx[2]);
                const int16_t* r3 = getHRow(ty.idx[3]);
                const int32_t wy0 = ty.w[0], wy1 = ty.w[1], wy2 = ty.w[2], wy3 = ty.w[3];
                uint8_t* dstVis8 = dRow + static_cast<size_t>(plan.visX0) * 4;
                for (int x = 0; x < plan.visW; ++x) {
                    const int i3 = x * 3;
                    const int32_t vb = (r0[i3 + 0] * wy0 + r1[i3 + 0] * wy1 + r2[i3 + 0] * wy2 + r3[i3 + 0] * wy3 + 2048) >> 12;
                    const int32_t vg = (r0[i3 + 1] * wy0 + r1[i3 + 1] * wy1 + r2[i3 + 1] * wy2 + r3[i3 + 1] * wy3 + 2048) >> 12;
                    const int32_t vr = (r0[i3 + 2] * wy0 + r1[i3 + 2] * wy1 + r2[i3 + 2] * wy2 + r3[i3 + 2] * wy3 + 2048) >> 12;
                    dstVis8[x * 4 + 0] = static_cast<uint8_t>(std::clamp(vb, 0, 255));
                    dstVis8[x * 4 + 1] = static_cast<uint8_t>(std::clamp(vg, 0, 255));
                    dstVis8[x * 4 + 2] = static_cast<uint8_t>(std::clamp(vr, 0, 255));
                    dstVis8[x * 4 + 3] = 255;
                }
            }
        }
    });
}

void ConvertOutputBgraToNv12(const uint8_t* outBgra, uint32_t outW, uint32_t outH,
                             std::vector<uint8_t>& nv12, BandWorkerPool& pool) {
    const size_t yBytes = static_cast<size_t>(outW) * outH;
    const size_t total = yBytes + (yBytes / 2);
    if (nv12.size() != total) nv12.resize(total);

    uint8_t* yPlane  = nv12.data();
    uint8_t* uvPlane = nv12.data() + yBytes;
    const size_t stride = static_cast<size_t>(outW) * 4;
    const uint32_t pairRows = outH >> 1;

    pool.Dispatch([&](size_t wi, size_t wn) {
        const uint32_t pBeg = static_cast<uint32_t>((static_cast<uint64_t>(pairRows) * wi) / wn);
        const uint32_t pEnd = static_cast<uint32_t>((static_cast<uint64_t>(pairRows) * (wi + 1)) / wn);
        const __m128i m16 = _mm_set1_epi8(16);
        const __m128i m128_16 = _mm_set1_epi16(128);
        const __m128i cY_R = _mm_set1_epi16(47);
        const __m128i cY_G = _mm_set1_epi16(157);
        const __m128i cY_B = _mm_set1_epi16(16);
        const __m128i maskB = _mm_set1_epi32(0x000000FF);

        auto calcY8 = [&](__m128i a, __m128i b) -> __m128i {
            const __m128i B = _mm_packs_epi32(_mm_and_si128(a, maskB), _mm_and_si128(b, maskB));
            const __m128i G = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(a, 8), maskB),
                                              _mm_and_si128(_mm_srli_epi32(b, 8), maskB));
            const __m128i R = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(a, 16), maskB),
                                              _mm_and_si128(_mm_srli_epi32(b, 16), maskB));
            const __m128i lum = _mm_srli_epi16(
                _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(R, cY_R), _mm_mullo_epi16(G, cY_G)),
                              _mm_add_epi16(_mm_mullo_epi16(B, cY_B), m128_16)),
                8);
            return _mm_add_epi8(_mm_packus_epi16(lum, lum), m16);
        };

        for (uint32_t p = pBeg; p < pEnd; ++p) {
            const uint32_t y = p << 1;
            const uint8_t* s0 = outBgra + static_cast<size_t>(y) * stride;
            const uint8_t* s1 = s0 + stride;
            uint8_t* y0 = yPlane + static_cast<size_t>(y) * outW;
            uint8_t* y1 = y0 + outW;
            uint8_t* uv = uvPlane + static_cast<size_t>(p) * outW;

            uint32_t x = 0;
            for (; x + 7 < outW; x += 8) {
                const __m128i p0a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s0 + static_cast<size_t>(x) * 4));
                const __m128i p0b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s0 + static_cast<size_t>(x) * 4 + 16));
                const __m128i p1a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s1 + static_cast<size_t>(x) * 4));
                const __m128i p1b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s1 + static_cast<size_t>(x) * 4 + 16));

                _mm_storel_epi64(reinterpret_cast<__m128i*>(y0 + x), calcY8(p0a, p0b));
                _mm_storel_epi64(reinterpret_cast<__m128i*>(y1 + x), calcY8(p1a, p1b));

                const __m128i avg0a = _mm_avg_epu8(p0a, p1a);
                const __m128i avg0b = _mm_avg_epu8(p0b, p1b);
                const __m128i pair0 = _mm_avg_epu8(avg0a, _mm_shuffle_epi32(avg0a, _MM_SHUFFLE(3, 3, 1, 1)));
                const __m128i pair1 = _mm_avg_epu8(avg0b, _mm_shuffle_epi32(avg0b, _MM_SHUFFLE(3, 3, 1, 1)));
                const __m128i p4 = _mm_castps_si128(
                    _mm_shuffle_ps(_mm_castsi128_ps(pair0), _mm_castsi128_ps(pair1), _MM_SHUFFLE(2, 0, 2, 0)));

                const __m128i B4 = _mm_packs_epi32(_mm_and_si128(p4, maskB), _mm_setzero_si128());
                const __m128i G4 = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(p4, 8), maskB), _mm_setzero_si128());
                const __m128i R4 = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(p4, 16), maskB), _mm_setzero_si128());

                __m128i U = _mm_add_epi16(
                    _mm_sub_epi16(_mm_mullo_epi16(B4, _mm_set1_epi16(112)),
                                  _mm_add_epi16(_mm_mullo_epi16(R4, _mm_set1_epi16(26)),
                                                _mm_mullo_epi16(G4, _mm_set1_epi16(86)))),
                    m128_16);
                __m128i V = _mm_add_epi16(
                    _mm_sub_epi16(_mm_mullo_epi16(R4, _mm_set1_epi16(112)),
                                  _mm_add_epi16(_mm_mullo_epi16(G4, _mm_set1_epi16(102)),
                                                _mm_mullo_epi16(B4, _mm_set1_epi16(10)))),
                    m128_16);
                U = _mm_add_epi16(_mm_srai_epi16(U, 8), m128_16);
                V = _mm_add_epi16(_mm_srai_epi16(V, 8), m128_16);
                __m128i UV = _mm_packus_epi16(_mm_unpacklo_epi16(U, V), _mm_setzero_si128());
                _mm_storel_epi64(reinterpret_cast<__m128i*>(uv + x), UV);
            }

            for (; x + 1 < outW; x += 2) {
                const size_t o = static_cast<size_t>(x) * 4;
                const int b00 = s0[o + 0], g00 = s0[o + 1], r00 = s0[o + 2];
                const int b01 = s0[o + 4], g01 = s0[o + 5], r01 = s0[o + 6];
                const int b10 = s1[o + 0], g10 = s1[o + 1], r10 = s1[o + 2];
                const int b11 = s1[o + 4], g11 = s1[o + 5], r11 = s1[o + 6];

                y0[x]     = static_cast<uint8_t>(((47 * r00 + 157 * g00 + 16 * b00 + 128) >> 8) + 16);
                y0[x + 1] = static_cast<uint8_t>(((47 * r01 + 157 * g01 + 16 * b01 + 128) >> 8) + 16);
                y1[x]     = static_cast<uint8_t>(((47 * r10 + 157 * g10 + 16 * b10 + 128) >> 8) + 16);
                y1[x + 1] = static_cast<uint8_t>(((47 * r11 + 157 * g11 + 16 * b11 + 128) >> 8) + 16);

                const int ra = (r00 + r01 + r10 + r11 + 2) >> 2;
                const int ga = (g00 + g01 + g10 + g11 + 2) >> 2;
                const int ba = (b00 + b01 + b10 + b11 + 2) >> 2;
                uv[x]     = static_cast<uint8_t>(((-26 * ra - 86 * ga + 112 * ba + 128) >> 8) + 128);
                uv[x + 1] = static_cast<uint8_t>(((112 * ra - 102 * ga - 10 * ba + 128) >> 8) + 128);
            }
        }
    });
}

ComPtr<IMFSample> CloneCompressedSample(IMFSample* src) {
    if (!src) return nullptr;
    ComPtr<IMFMediaBuffer> srcBuf;
    if (FAILED(src->ConvertToContiguousBuffer(srcBuf.GetAddressOf())) || !srcBuf) {
        return nullptr;
    }

    BYTE* pData = nullptr;
    DWORD curLen = 0;
    if (FAILED(srcBuf->Lock(&pData, nullptr, &curLen))) {
        return nullptr;
    }

    ComPtr<IMFMediaBuffer> dstBuf;
    ComPtr<IMFSample> dstSample;
    if (SUCCEEDED(MFCreateMemoryBuffer(curLen, dstBuf.GetAddressOf())) && dstBuf &&
        SUCCEEDED(MFCreateSample(dstSample.GetAddressOf())) && dstSample) {
        BYTE* dstPtr = nullptr;
        if (SUCCEEDED(dstBuf->Lock(&dstPtr, nullptr, nullptr))) {
            if (curLen > 0 && pData) std::memcpy(dstPtr, pData, curLen);
            dstBuf->Unlock();
            dstBuf->SetCurrentLength(curLen);
            dstSample->AddBuffer(dstBuf.Get());

            LONGLONG pts = 0, dur = 0;
            DWORD flags = 0;
            if (SUCCEEDED(src->GetSampleTime(&pts))) dstSample->SetSampleTime(pts);
            if (SUCCEEDED(src->GetSampleDuration(&dur))) dstSample->SetSampleDuration(dur);
            if (SUCCEEDED(src->GetSampleFlags(&flags))) dstSample->SetSampleFlags(flags);
            src->CopyAllItems(dstSample.Get());
        } else {
            dstSample.Reset();
        }
    }

    srcBuf->Unlock();
    return dstSample;
}

struct Nv12BufferPool : public std::enable_shared_from_this<Nv12BufferPool> {
    std::mutex mu;
    std::vector<std::vector<uint8_t>> freeBufs;

    std::vector<uint8_t> Acquire(size_t bytes) {
        std::vector<uint8_t> b;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!freeBufs.empty()) {
                b = std::move(freeBufs.back());
                freeBufs.pop_back();
            }
        }
        if (b.size() != bytes) b.resize(bytes);
        return b;
    }

    void Recycle(std::vector<uint8_t>&& b) {
        std::lock_guard<std::mutex> lk(mu);
        if (freeBufs.size() < 6) freeBufs.push_back(std::move(b));
    }
};

class PooledNv12MediaBuffer final : public IMFMediaBuffer {
public:
    PooledNv12MediaBuffer(std::shared_ptr<Nv12BufferPool> pool, size_t capBytes)
        : m_pool(pool), m_wp(pool), m_data(pool ? pool->Acquire(capBytes) : std::vector<uint8_t>(capBytes)) {}

    ~PooledNv12MediaBuffer() {
        if (auto p = m_wp.lock()) p->Recycle(std::move(m_data));
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaBuffer) {
            *ppv = static_cast<IMFMediaBuffer*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    STDMETHODIMP_(ULONG) Release() override {
        const LONG c = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    STDMETHODIMP Lock(BYTE** ppbBuffer, DWORD* pcbMaxLength, DWORD* pcbCurrentLength) override {
        if (!ppbBuffer) return E_POINTER;
        *ppbBuffer = m_data.empty() ? nullptr : m_data.data();
        if (pcbMaxLength) *pcbMaxLength = static_cast<DWORD>(m_data.size());
        if (pcbCurrentLength) *pcbCurrentLength = m_curLen;
        return S_OK;
    }

    STDMETHODIMP Unlock() override { return S_OK; }
    STDMETHODIMP GetCurrentLength(DWORD* pcbCurrentLength) override {
        if (!pcbCurrentLength) return E_POINTER;
        *pcbCurrentLength = m_curLen;
        return S_OK;
    }
    STDMETHODIMP SetCurrentLength(DWORD cbCurrentLength) override {
        if (cbCurrentLength > m_data.size()) return E_INVALIDARG;
        m_curLen = cbCurrentLength;
        return S_OK;
    }
    STDMETHODIMP GetMaxLength(DWORD* pcbMaxLength) override {
        if (!pcbMaxLength) return E_POINTER;
        *pcbMaxLength = static_cast<DWORD>(m_data.size());
        return S_OK;
    }

    uint8_t* DataPtr() noexcept { return m_data.data(); }

private:
    std::atomic<LONG> m_ref{1};
    std::shared_ptr<Nv12BufferPool> m_pool;
    std::weak_ptr<Nv12BufferPool> m_wp;
    std::vector<uint8_t> m_data;
    DWORD m_curLen = 0;
};

bool CreateNv12Sample(const std::vector<uint8_t>& nv12, LONGLONG time100ns, LONGLONG dur100ns,
                      const std::shared_ptr<Nv12BufferPool>& pool, ComPtr<IMFSample>& outSmp) {
    ComPtr<IMFMediaBuffer> buf;
    if (pool) {
        auto* rawBuf = new PooledNv12MediaBuffer(pool, nv12.size());
        std::memcpy(rawBuf->DataPtr(), nv12.data(), nv12.size());
        rawBuf->SetCurrentLength(static_cast<DWORD>(nv12.size()));
        buf.Attach(rawBuf);
    } else {
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), buf.GetAddressOf()))) return false;
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
        std::memcpy(dst, nv12.data(), nv12.size());
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(nv12.size()));
    }
    if (FAILED(MFCreateSample(outSmp.GetAddressOf()))) return false;
    outSmp->AddBuffer(buf.Get());
    outSmp->SetSampleTime(time100ns);
    outSmp->SetSampleDuration(dur100ns);
    return true;
}

bool WriteAudioPcmSample(IMFSinkWriter* writer, DWORD streamIdx, const std::vector<int16_t>& pcm,
                         LONGLONG time100ns, LONGLONG dur100ns) {
    const DWORD bytes = static_cast<DWORD>(pcm.size() * sizeof(int16_t));
    if (bytes == 0) return true;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(bytes, buf.GetAddressOf()))) return false;
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
    std::memcpy(dst, pcm.data(), bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);

    ComPtr<IMFSample> smp;
    if (FAILED(MFCreateSample(smp.GetAddressOf()))) return false;
    smp->AddBuffer(buf.Get());
    smp->SetSampleTime(time100ns);
    smp->SetSampleDuration(dur100ns);
    return SUCCEEDED(writer->WriteSample(streamIdx, smp.Get()));
}

class DirectMftVideoEncoder {
public:
    bool Init(const DetectedEncoder& enc, const EncoderPreset& preset,
              uint32_t outW, uint32_t outH, uint32_t fps, uint32_t bitrateMbps) {
        Reset();
        if (enc.clsid == GUID_NULL) return false;

        IMFActivate** pp = nullptr;
        UINT32 count = 0;
        const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT |
                             MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                             MFT_ENUM_FLAG_SORTANDFILTER;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, nullptr, &pp, &count)) || !pp) {
            return false;
        }

        ComPtr<IMFActivate> chosenAct;
        for (UINT32 i = 0; i < count; ++i) {
            CLSID cid{};
            if (pp[i] && SUCCEEDED(pp[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &cid)) &&
                IsEqualCLSID(cid, enc.clsid) && !chosenAct) {
                chosenAct = pp[i];
            }
            if (pp[i]) pp[i]->Release();
        }
        CoTaskMemFree(pp);
        if (!chosenAct) return false;

        if (FAILED(chosenAct->ActivateObject(IID_PPV_ARGS(&m_mft))) || !m_mft) {
            return false;
        }

        ComPtr<IMFAttributes> ta;
        UINT32 d3dAware = 0, asyncFlag = 0;
        if (SUCCEEDED(m_mft->GetAttributes(ta.GetAddressOf())) && ta) {
            ta->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag);
            if (asyncFlag) ta->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            ta->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
        }
        m_isAsync = (asyncFlag != 0);

        if (d3dAware) {
            SetupVendorD3D11(enc);
        }

        const double scaledMbps = static_cast<double>(bitrateMbps) * std::max(0.5, preset.bitrateMultiplier);
        const uint32_t targetBps = static_cast<uint32_t>(std::clamp(scaledMbps * 1000000.0, 2000000.0, 120000000.0));

        if (!enc.isHardware) {
            ApplyPresetCodecApi(enc, preset, targetBps, fps);
        }

        m_outSubtype = enc.subtype;
        ComPtr<IMFMediaType> outT;
        if (FAILED(MFCreateMediaType(outT.GetAddressOf()))) return false;
        outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outT->SetGUID(MF_MT_SUBTYPE, m_outSubtype);
        outT->SetUINT32(MF_MT_AVG_BITRATE, targetBps);
        outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outT->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        outT->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        outT->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        outT->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        if (!enc.isHevc) {
            outT->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        }
        MFSetAttributeSize(outT.Get(), MF_MT_FRAME_SIZE, outW, outH);
        MFSetAttributeRatio(outT.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(outT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(m_mft->SetOutputType(0, outT.Get(), 0))) {
            outT->DeleteItem(MF_MT_MPEG2_PROFILE);
            if (FAILED(m_mft->SetOutputType(0, outT.Get(), 0))) {
                Reset();
                return false;
            }
        }

        ComPtr<IMFMediaType> inT;
        if (FAILED(MFCreateMediaType(inT.GetAddressOf()))) return false;
        inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        inT->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        inT->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        inT->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        inT->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        MFSetAttributeSize(inT.Get(), MF_MT_FRAME_SIZE, outW, outH);
        MFSetAttributeRatio(inT.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(inT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(m_mft->SetInputType(0, inT.Get(), 0))) {
            Reset();
            return false;
        }

        m_nv12Pool = std::make_shared<Nv12BufferPool>();
        ApplyPresetCodecApi(enc, preset, targetBps, fps);

        if (m_isAsync && FAILED(m_mft.As(&m_evGen))) {
            Reset();
            return false;
        }

        if (FAILED(m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
            Reset();
            return false;
        }
        m_streamingActive = true;

        m_mft->GetOutputCurrentType(0, m_finalOutType.GetAddressOf());
        if (!m_finalOutType) m_finalOutType = outT;
        return true;
    }

    bool PrewarmAndCaptureHeader(const std::vector<uint8_t>& nv12Frame, LONGLONG dur100ns,
                                 std::vector<ComPtr<IMFSample>>& outPrewarmedSamples,
                                 uint64_t& outFedFrames) {
        outPrewarmedSamples.clear();
        outFedFrames = 0;
        if (!m_mft) return false;

        if (!m_isAsync) {
            uint64_t fedCount = 0;
            for (uint64_t f = 0; f < 8; ++f) {
                ComPtr<IMFSample> s;
                if (!CreateNv12Sample(nv12Frame, static_cast<LONGLONG>(f) * dur100ns, dur100ns, m_nv12Pool, s)) break;
                if (FAILED(m_mft->ProcessInput(0, s.Get(), 0))) break;
                ++fedCount;
                DrainSync(outPrewarmedSamples);
                if (!outPrewarmedSamples.empty()) break;
            }
            outFedFrames = fedCount;
            if (fedCount == 0) return false;
            m_mft->GetOutputCurrentType(0, m_finalOutType.ReleaseAndGetAddressOf());
            return true;
        }

        uint64_t fedCount = 0;
        const ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 1200) {
            PumpAsyncEvents(outPrewarmedSamples);
            while (m_needInputCredits > 0 && fedCount < 10 && outPrewarmedSamples.empty()) {
                ComPtr<IMFSample> s;
                if (CreateNv12Sample(nv12Frame, static_cast<LONGLONG>(fedCount) * dur100ns, dur100ns, m_nv12Pool, s) &&
                    SUCCEEDED(m_mft->ProcessInput(0, s.Get(), 0))) {
                    --m_needInputCredits;
                    ++fedCount;
                } else {
                    break;
                }
            }
            if (!outPrewarmedSamples.empty()) {
                m_mft->GetOutputCurrentType(0, m_finalOutType.ReleaseAndGetAddressOf());
                outFedFrames = fedCount;
                return true;
            }
            Sleep(2);
        }
        outFedFrames = fedCount;
        if (fedCount > 0) {
            m_mft->GetOutputCurrentType(0, m_finalOutType.ReleaseAndGetAddressOf());
            return true;
        }
        return false;
    }

    bool PushFrame(const std::vector<uint8_t>& nv12, LONGLONG time100ns, LONGLONG dur100ns,
                   std::vector<ComPtr<IMFSample>>& outEncoded) {
        if (!m_mft) return false;
        if (!m_isAsync) {
            ComPtr<IMFSample> s;
            if (!CreateNv12Sample(nv12, time100ns, dur100ns, m_nv12Pool, s)) return false;
            HRESULT hr = m_mft->ProcessInput(0, s.Get(), 0);
            if (hr == MF_E_NOTACCEPTING) {
                DrainSync(outEncoded);
                hr = m_mft->ProcessInput(0, s.Get(), 0);
            }
            DrainSync(outEncoded);
            return SUCCEEDED(hr);
        }

        PumpAsyncEvents(outEncoded);
        if (m_needInputCredits == 0) {
            const ULONGLONG wait0 = GetTickCount64();
            while (m_needInputCredits == 0 && (GetTickCount64() - wait0 < 45)) {
                Sleep(1);
                PumpAsyncEvents(outEncoded);
            }
        }
        bool accepted = false;
        if (m_needInputCredits > 0) {
            ComPtr<IMFSample> s;
            if (CreateNv12Sample(nv12, time100ns, dur100ns, m_nv12Pool, s) &&
                SUCCEEDED(m_mft->ProcessInput(0, s.Get(), 0))) {
                --m_needInputCredits;
                accepted = true;
            }
        }
        PumpAsyncEvents(outEncoded);
        return accepted;
    }

    void DrainAll(std::vector<ComPtr<IMFSample>>& outEncoded) {
        if (!m_mft) return;
        if (m_streamingActive) {
            m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        }
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (!m_isAsync) {
            DrainSync(outEncoded);
            return;
        }
        const ULONGLONG t0 = GetTickCount64();
        while (!m_drainComplete && (GetTickCount64() - t0 < 1200)) {
            PumpAsyncEvents(outEncoded);
            if (!m_drainComplete) Sleep(2);
        }
    }

    IMFMediaType* FinalOutputType() const { return m_finalOutType.Get(); }

    void Reset() {
        if (m_mft) {
            if (m_streamingActive) {
                m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
                m_streamingActive = false;
            }
            ComPtr<IMFShutdown> shutdown;
            if (SUCCEEDED(m_mft.As(&shutdown)) && shutdown) {
                shutdown->Shutdown();
            }
        }
        m_finalOutType.Reset();
        m_evGen.Reset();
        m_mft.Reset();
        m_d3dMgr.Reset();
        m_d3dDev.Reset();
        m_nv12Pool.reset();
        m_isAsync = false;
        m_needInputCredits = 0;
        m_drainComplete = false;
        m_streamingActive = false;
    }

private:
    void ApplyPresetCodecApi(const DetectedEncoder& enc, const EncoderPreset& preset, uint32_t targetBps, uint32_t fps) {
        if (!m_mft) return;
        ComPtr<ICodecAPI> api;
        if (FAILED(m_mft.As(&api)) || !api) return;

        auto setUi4 = [&](const GUID& key, ULONG val) {
            VARIANT v{};
            v.vt = VT_UI4;
            v.ulVal = val;
            api->SetValue(&key, &v);
        };
        auto setBool = [&](const GUID& key, bool val) {
            VARIANT v{};
            v.vt = VT_BOOL;
            v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
            api->SetValue(&key, &v);
        };

        setUi4(CODECAPI_AVEncCommonRateControlMode, preset.rateControlMode);
        setBool(CODECAPI_AVEncCommonLowLatency, preset.lowLatency);
        setUi4(CODECAPI_AVEncCommonQualityVsSpeed, preset.qualityVsSpeed);
        setUi4(CODECAPI_AVEncCommonQuality, preset.qualityLevel);
        setUi4(CODECAPI_AVEncCommonMeanBitRate, targetBps);
        setUi4(CODECAPI_AVEncCommonMaxBitRate, static_cast<ULONG>(std::min<double>(targetBps * 2.0, 160000000.0)));
        setUi4(CODECAPI_AVEncCommonBufferSize, targetBps);
        setUi4(CODECAPI_AVEncMPVGOPSize, std::max<ULONG>(15, fps));
        if (!enc.isHevc) {
            setBool(CODECAPI_AVEncH264CABACEnable, true);
        }

        setUi4(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    }

    void SetupVendorD3D11(const DetectedEncoder& enc) {
        ComPtr<IDXGIFactory1> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || !fac) return;
        ComPtr<IDXGIAdapter1> pickAdp;
        ComPtr<IDXGIAdapter1> adp;

        const UINT wantVen =
            (enc.rawName.find(L"NVIDIA") != std::wstring::npos) ? 0x10DEu :
            ((enc.rawName.find(L"Intel") != std::wstring::npos) ? 0x8086u :
            ((enc.rawName.find(L"AMD") != std::wstring::npos) ? 0x1002u : 0u));

        for (UINT a = 0; fac->EnumAdapters1(a, adp.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
            DXGI_ADAPTER_DESC1 dd{};
            adp->GetDesc1(&dd);
            const UINT64 aLuid = (static_cast<UINT64>(dd.AdapterLuid.HighPart) << 32) |
                                 static_cast<UINT32>(dd.AdapterLuid.LowPart);
            if ((enc.hasLuid && aLuid == enc.adapterLuid) ||
                (!enc.hasLuid && wantVen != 0 && dd.VendorId == wantVen)) {
                pickAdp = adp;
                break;
            }
        }
        if (SUCCEEDED(D3D11CreateDevice(pickAdp.Get(),
                                        pickAdp ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                        nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                        D3D11_SDK_VERSION, m_d3dDev.GetAddressOf(), nullptr, nullptr)) && m_d3dDev) {
            ComPtr<ID3D10Multithread> mt;
            if (SUCCEEDED(m_d3dDev.As(&mt)) && mt) {
                mt->SetMultithreadProtected(TRUE);
            }
            UINT resetTok = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&resetTok, m_d3dMgr.GetAddressOf())) && m_d3dMgr) {
                m_d3dMgr->ResetDevice(m_d3dDev.Get(), resetTok);
                m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_d3dMgr.Get()));
            }
        }
    }

    void DrainSync(std::vector<ComPtr<IMFSample>>& outEncoded) {
        MFT_OUTPUT_STREAM_INFO osi{};
        m_mft->GetOutputStreamInfo(0, &osi);
        const bool prov = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        for (int k = 0; k < 24; ++k) {
            MFT_OUTPUT_DATA_BUFFER odb{};
            odb.dwStreamID = 0;
            ComPtr<IMFSample> localSmp;
            if (!prov) {
                MFCreateSample(localSmp.GetAddressOf());
                ComPtr<IMFMediaBuffer> b;
                MFCreateMemoryBuffer(osi.cbSize > 0 ? osi.cbSize : (1024u * 1024u), b.GetAddressOf());
                if (localSmp && b) localSmp->AddBuffer(b.Get());
                odb.pSample = localSmp.Get();
                if (odb.pSample) odb.pSample->AddRef();
            }

            DWORD status = 0;
            const HRESULT hr = m_mft->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) odb.pEvents->Release();

            if (SUCCEEDED(hr) && odb.pSample) {
                ComPtr<IMFSample> srcWrapped;
                srcWrapped.Attach(odb.pSample);
                ComPtr<IMFSample> standalone = CloneCompressedSample(srcWrapped.Get());
                if (standalone) {
                    outEncoded.push_back(std::move(standalone));
                }
            } else {
                if (odb.pSample) odb.pSample->Release();
                if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                    ComPtr<IMFMediaType> nt;
                    if (SUCCEEDED(m_mft->GetOutputAvailableType(0, 0, nt.GetAddressOf())) && nt) {
                        m_mft->SetOutputType(0, nt.Get(), 0);
                    }
                    continue;
                }
                break;
            }
        }
    }

    void PumpAsyncEvents(std::vector<ComPtr<IMFSample>>& outEncoded) {
        if (!m_evGen) return;
        for (int iter = 0; iter < 128; ++iter) {
            ComPtr<IMFMediaEvent> ev;
            if (m_evGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, ev.GetAddressOf()) != S_OK || !ev) break;
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            if (met == METransformNeedInput) {
                ++m_needInputCredits;
            } else if (met == METransformHaveOutput) {
                DrainSync(outEncoded);
            } else if (met == METransformDrainComplete) {
                m_drainComplete = true;
            }
        }
    }

    ComPtr<IMFTransform> m_mft;
    ComPtr<IMFMediaEventGenerator> m_evGen;
    ComPtr<ID3D11Device> m_d3dDev;
    ComPtr<IMFDXGIDeviceManager> m_d3dMgr;
    std::shared_ptr<Nv12BufferPool> m_nv12Pool;
    ComPtr<IMFMediaType> m_finalOutType;
    GUID m_outSubtype = MFVideoFormat_H264;
    bool m_isAsync = false;
    bool m_streamingActive = false;
    int m_needInputCredits = 0;
    bool m_drainComplete = false;
};

bool CreatePassthroughSinkWriter(const std::wstring& outPath,
                                 IMFMediaType* videoEncOutType,
                                 uint32_t audioBitrateKbps,
                                 ComPtr<IMFSinkWriter>& outWriter,
                                 DWORD& outVidIdx,
                                 DWORD& outAudIdx,
                                 bool& outHasAudio) {
    outWriter.Reset();
    outVidIdx = 0;
    outAudIdx = 0;
    outHasAudio = false;

    if (!videoEncOutType) return false;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(attrs.GetAddressOf(), 2);
    if (attrs) attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    if (FAILED(MFCreateSinkWriterFromURL(outPath.c_str(), nullptr, attrs.Get(), outWriter.GetAddressOf())) || !outWriter) {
        return false;
    }

    if (FAILED(outWriter->AddStream(videoEncOutType, &outVidIdx)) ||
        FAILED(outWriter->SetInputMediaType(outVidIdx, videoEncOutType, nullptr))) {
        outWriter.Reset();
        return false;
    }

    ComPtr<IMFMediaType> audOut, audIn;
    if (SUCCEEDED(MFCreateMediaType(audOut.GetAddressOf())) &&
        SUCCEEDED(MFCreateMediaType(audIn.GetAddressOf()))) {
        audOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioEngine::kTargetSampleRate);
        audOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioEngine::kTargetChannels);
        audOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (audioBitrateKbps * 1000u) / 8u);

        audIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        audIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioEngine::kTargetSampleRate);
        audIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioEngine::kTargetChannels);
        audIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        audIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioEngine::kTargetSampleRate * 4u);

        if (SUCCEEDED(outWriter->AddStream(audOut.Get(), &outAudIdx)) &&
            SUCCEEDED(outWriter->SetInputMediaType(outAudIdx, audIn.Get(), nullptr))) {
            outHasAudio = true;
        }
    }

    if (FAILED(outWriter->BeginWriting())) {
        outWriter.Reset();
        return false;
    }
    return true;
}

}

RecorderEngine::RecorderEngine(CaptureEngine& capture, CameraEngine& camera, AudioEngine& audio, OverlayCompositor& overlays)
    : m_capture(capture)
    , m_camera(camera)
    , m_audio(audio)
    , m_overlays(overlays) {}

RecorderEngine::~RecorderEngine() {
    StopRecording();
}

void RecorderEngine::ComputeSourcePlacement(uint32_t canvasW, uint32_t canvasH,
                                            uint32_t srcContentW, uint32_t srcContentH,
                                            const AppConfig& cfg,
                                            float& outDstX, float& outDstY, float& outDstW, float& outDstH) {
    const float cw = static_cast<float>(std::max<uint32_t>(1, canvasW));
    const float ch = static_cast<float>(std::max<uint32_t>(1, canvasH));
    const float sw = static_cast<float>(std::max<uint32_t>(1, srcContentW));
    const float sh = static_cast<float>(std::max<uint32_t>(1, srcContentH));

    const float boxX = cfg.srcPosX * cw;
    const float boxY = cfg.srcPosY * ch;
    const float boxW = std::max(16.0f, cfg.srcScaleW * cw);
    const float boxH = std::max(16.0f, cfg.srcScaleH * ch);

    if (cfg.srcKeepAspect) {
        const float sc = std::min(boxW / sw, boxH / sh);
        outDstW = std::max(8.0f, sw * sc);
        outDstH = std::max(8.0f, sh * sc);
        outDstX = boxX + (boxW - outDstW) * 0.5f;
        outDstY = boxY + (boxH - outDstH) * 0.5f;
    } else {
        outDstX = boxX;
        outDstY = boxY;
        outDstW = boxW;
        outDstH = boxH;
    }
}

void RecorderEngine::ResolveOutputDimensions(const AppConfig& cfg, uint32_t& outW, uint32_t& outH) const {
    uint32_t srcW = 1920, srcH = 1080;
    if (cfg.hasCaptureSource && cfg.sourceVisible) {
        if (cfg.sourceType == 2) {
            srcW = static_cast<uint32_t>(std::clamp(cfg.cropW, 64, 3840));
            srcH = static_cast<uint32_t>(std::clamp(cfg.cropH, 64, 2160));
        } else if (!m_capture.GetCurrentDimensions(srcW, srcH)) {
            const auto mons = CaptureEngine::EnumerateMonitors();
            if (!mons.empty()) {
                const size_t idx = std::min<size_t>(static_cast<size_t>(std::max(0, cfg.monitorIndex)), mons.size() - 1);
                srcW = static_cast<uint32_t>(std::max(64, mons[idx].width));
                srcH = static_cast<uint32_t>(std::max(64, mons[idx].height));
            }
        }
    }

    if (_wcsicmp(cfg.resolution.c_str(), L"1440p") == 0) {
        outW = 2560; outH = 1440;
    } else if (_wcsicmp(cfg.resolution.c_str(), L"1080p") == 0) {
        outW = 1920; outH = 1080;
    } else if (_wcsicmp(cfg.resolution.c_str(), L"720p") == 0) {
        outW = 1280; outH = 720;
    } else if (_wcsicmp(cfg.resolution.c_str(), L"vertical") == 0) {
        outW = 1080; outH = 1920;
    } else if (_wcsicmp(cfg.resolution.c_str(), L"custom") == 0) {
        outW = static_cast<uint32_t>(std::clamp(cfg.customWidth, 64, 3840));
        outH = static_cast<uint32_t>(std::clamp(cfg.customHeight, 64, 2160));
    } else {
        outW = srcW;
        outH = srcH;
        if (outW > 3840 || outH > 2160) {
            const double sc = std::min(3840.0 / outW, 2160.0 / outH);
            outW = static_cast<uint32_t>(std::lround(outW * sc));
            outH = static_cast<uint32_t>(std::lround(outH * sc));
        }
    }
    outW = std::max<uint32_t>(64, outW & ~1u);
    outH = std::max<uint32_t>(64, outH & ~1u);
}

bool RecorderEngine::StartRecording(const AppConfig& cfg,
                                    const std::vector<DetectedEncoder>& encoders,
                                    std::wstring& outError) {
    if (m_isRecording.load()) {
        outError = L"Already recording";
        return false;
    }

    uint32_t outW = 1920, outH = 1080;
    ResolveOutputDimensions(cfg, outW, outH);
    const DetectedEncoder chosenEnc = EncoderManager::ResolveSelection(encoders, cfg.encoderId);
    const std::wstring targetPath = BuildTimestampMp4Path(cfg.outputDir);

    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_stats = RecorderStats{};
        m_stats.isRecording = true;
        m_stats.outWidth = outW;
        m_stats.outHeight = outH;
        m_stats.currentFilePath = targetPath;
        m_stats.encoderLabel = chosenEnc.friendlyName.empty() ? L"Initializing..." : chosenEnc.friendlyName;
    }

    m_stopRequested.store(false);
    m_isPaused.store(false);
    m_isRecording.store(true);
    m_audio.SetRecordingState(true, false);

    m_thread = std::thread(&RecorderEngine::WriterThreadProc, this, cfg, chosenEnc, targetPath, outW, outH);
    outError.clear();
    return true;
}

void RecorderEngine::StopRecording() {
    if (!m_isRecording.load() && !m_thread.joinable()) return;
    m_stopRequested.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_audio.SetRecordingState(false, false);
    m_isRecording.store(false);
    m_isPaused.store(false);
}

void RecorderEngine::SetPaused(bool paused) {
    if (!m_isRecording.load()) return;
    m_isPaused.store(paused);
    m_audio.SetRecordingState(true, paused);
    std::lock_guard<std::mutex> lk(m_statsMutex);
    m_stats.isPaused = paused;
}

RecorderStats RecorderEngine::GetStats() const {
    std::lock_guard<std::mutex> lk(m_statsMutex);
    return m_stats;
}

std::wstring RecorderEngine::GetLastCompletedFile() const {
    std::lock_guard<std::mutex> lk(m_statsMutex);
    return m_lastCompletedFile;
}

const std::vector<uint8_t>* PrepareNv12Frame(const FramePtr& capPkt, const FrameCopy& camFrame, const AppConfig& cfg,
                                           bool useCaptureSrc, bool camActive, bool hasVisibleOverlays,
                                           uint32_t outW, uint32_t outH,
                                           ScalePlan& scalePlan, OverlayCompositor& overlays,
                                           std::vector<uint8_t>& outBgra, std::vector<uint8_t>& nv12, BandWorkerPool& pool) {
    const ColorFilterParams colorParams{
        cfg.colorFilterEnabled,
        cfg.filterGamma,
        cfg.filterContrast,
        cfg.filterBrightness,
        cfg.filterSaturation
    };
    const bool hasFilter = colorParams.HasEffect();

    if (useCaptureSrc && capPkt && !capPkt->nv12.empty() &&
        capPkt->nv12Width == outW && capPkt->nv12Height == outH) {
        if (!hasFilter && !camActive && !hasVisibleOverlays) {
            return &capPkt->nv12;
        }
        nv12 = capPkt->nv12;
        if (hasFilter) {
            uint8_t lutY[256];
            uint8_t lutUV[256];
            const bool hasLut = (std::abs(colorParams.gamma) > 0.001f ||
                                 std::abs(colorParams.contrast) > 0.001f ||
                                 std::abs(colorParams.brightness) > 0.001f);
            const bool hasSat = (std::abs(colorParams.saturation) > 0.001f);
            if (hasLut) ColorCorrection::BuildLut(colorParams, lutY);
            if (hasSat) ColorCorrection::BuildUvLut(colorParams, lutUV);

            const size_t yBytes = static_cast<size_t>(outW) * outH;
            const size_t uvBytes = yBytes / 2;
            pool.Dispatch([&](size_t wi, size_t wn) {
                if (hasLut) {
                    const size_t yChunk = (yBytes + wn - 1) / wn;
                    const size_t yStart = wi * yChunk;
                    const size_t yEnd = std::min(yBytes, yStart + yChunk);
                    uint8_t* yp = nv12.data() + yStart;
                    for (size_t i = yStart; i < yEnd; ++i, ++yp) {
                        *yp = lutY[*yp];
                    }
                }
                if (hasSat) {
                    const size_t uvChunk = (uvBytes + wn - 1) / wn;
                    const size_t uvStart = wi * uvChunk;
                    const size_t uvEnd = std::min(uvBytes, uvStart + uvChunk);
                    uint8_t* uvp = nv12.data() + yBytes + uvStart;
                    for (size_t i = uvStart; i < uvEnd; ++i, ++uvp) {
                        *uvp = lutUV[*uvp];
                    }
                }
            });
        }
        if (camActive) {
            OverlayCompositor::CompositeCameraOntoNv12(nv12, outW, outH, camFrame, cfg);
        }
        if (hasVisibleOverlays) {
            overlays.CompositeOntoNv12(nv12, outW, outH);
        }
        return &nv12;
    }

    const uint32_t fw = (useCaptureSrc && capPkt && capPkt->width > 0) ? capPkt->width : outW;
    const uint32_t fh = (useCaptureSrc && capPkt && capPkt->height > 0) ? capPkt->height : outH;
    scalePlan.Rebuild(fw, fh, cfg, outW, outH);

    const bool canDirectNv12 =
        !hasFilter &&
        useCaptureSrc && capPkt && !capPkt->bgra.empty() && !camActive && !hasVisibleOverlays &&
        scalePlan.visX0 == 0 && scalePlan.visY0 == 0 &&
        scalePlan.visW == static_cast<int>(outW) && scalePlan.visH == static_cast<int>(outH) &&
        scalePlan.cropX == 0 && scalePlan.cropY == 0 &&
        scalePlan.cropW == outW && scalePlan.cropH == outH &&
        scalePlan.srcW == outW && scalePlan.srcH == outH;

    if (canDirectNv12) {
        ConvertOutputBgraToNv12(capPkt->bgra.data(), outW, outH, nv12, pool);
        return &nv12;
    }

    std::vector<uint8_t> tmpBgra;
    const uint8_t* srcBgraPtr = nullptr;
    if (useCaptureSrc && capPkt) {
        if (!capPkt->bgra.empty()) {
            srcBgraPtr = capPkt->bgra.data();
        } else if (!capPkt->nv12.empty() && capPkt->nv12Width > 0 && capPkt->nv12Height > 0) {
            const uint32_t w = capPkt->nv12Width;
            const uint32_t h = capPkt->nv12Height;
            tmpBgra.resize(static_cast<size_t>(w) * h * 4);
            const uint8_t* yp = capPkt->nv12.data();
            const uint8_t* uvp = yp + static_cast<size_t>(w) * h;
            auto* dst32 = reinterpret_cast<uint32_t*>(tmpBgra.data());
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
            srcBgraPtr = tmpBgra.data();
        }
    }

    RenderScaledOutputBgra(srcBgraPtr, scalePlan, outBgra, pool);
    if (hasFilter) {
        pool.Dispatch([&](size_t wi, size_t wn) {
            const size_t total = static_cast<size_t>(outW) * outH;
            const size_t chunk = (total + wn - 1) / wn;
            const size_t start = wi * chunk;
            const size_t end = std::min(total, start + chunk);
            if (start < end) {
                ColorCorrection::ApplyBgra(outBgra.data() + start * 4, end - start, colorParams);
            }
        });
    }
    if (camActive) {
        OverlayCompositor::CompositeCameraOntoFrame(outBgra, outW, outH, camFrame, cfg);
    }
    if (hasVisibleOverlays) {
        overlays.CompositeOntoFrame(outBgra, outW, outH);
    }
    ConvertOutputBgraToNv12(outBgra.data(), outW, outH, nv12, pool);
    return &nv12;
}

void RecorderEngine::WriterThreadProc(AppConfig cfg, DetectedEncoder chosenEnc,
                                      std::wstring targetPath, uint32_t outW, uint32_t outH) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ScopedMsTimer {
        ScopedMsTimer() { timeBeginPeriod(1); }
        ~ScopedMsTimer() { timeEndPeriod(1); }
    } msTimerGuard;

    BandWorkerPool bandPool(4);

    const uint32_t fps = static_cast<uint32_t>(std::clamp(cfg.fps, 15, 120));
    const uint32_t bitrate = static_cast<uint32_t>(std::clamp(cfg.bitrateMbps, 4, 80));
    const uint32_t audioKbps = static_cast<uint32_t>((cfg.audioBitrateKbps >= 256) ? 320 : 192);
    const LONGLONG frameDur100ns = 10000000LL / static_cast<LONGLONG>(fps);
    const EncoderPreset chosenPreset = EncoderManager::ResolvePreset(chosenEnc, cfg.encoderPreset);

    const bool useCaptureSrc = cfg.hasCaptureSource && cfg.sourceVisible;
    const bool camActive = cfg.camAdded && cfg.camEnabled;
    bool hasClockOverlay = false;
    for (const auto& ov : cfg.overlays) {
        if (ov.visible && ov.kind == OverlayKind::Clock) { hasClockOverlay = true; break; }
    }

    uint64_t gpuTargetEp = 0;
    const bool useGpuVpFastPath = useCaptureSrc && (cfg.sourceType == 0 || cfg.sourceType == 2);
    if (useGpuVpFastPath) {
        int monW = static_cast<int>(outW), monH = static_cast<int>(outH);
        CaptureEngine::GetMonitorBounds(cfg.monitorIndex, monW, monH);
        ScalePlan tmpPlan;
        tmpPlan.Rebuild(static_cast<uint32_t>(monW), static_cast<uint32_t>(monH), cfg, outW, outH);
        m_capture.SetGpuNv12Target(true, outW, outH,
                                    static_cast<int>(tmpPlan.cropX), static_cast<int>(tmpPlan.cropY),
                                    static_cast<int>(tmpPlan.cropW), static_cast<int>(tmpPlan.cropH),
                                    tmpPlan.visX0, tmpPlan.visY0, tmpPlan.visW, tmpPlan.visH);
        gpuTargetEp = m_capture.GetCurrentTargetEpoch();
    } else {
        m_capture.SetGpuNv12Target(false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    FramePtr capPkt;
    if (useCaptureSrc) {
        for (int i = 0; i < 25 && !m_stopRequested.load(); ++i) {
            FramePtr cand = m_capture.GetLatestSharedFrame(0);
            if (cand && (!useGpuVpFastPath || cand->targetEpoch >= gpuTargetEp)) {
                capPkt = std::move(cand);
                break;
            }
            if (cand && i >= 12) {
                capPkt = std::move(cand);
                break;
            }
            Sleep(10);
        }
    }
    ScalePlan scalePlan;
    FrameCopy camFrame{};
    if (camActive) m_camera.CopyLatestFrame(camFrame, 0);

    std::vector<uint8_t> outBgra;
    std::vector<uint8_t> nv12;
    const std::vector<uint8_t>* activeNv12 = PrepareNv12Frame(
        capPkt, camFrame, cfg, useCaptureSrc, camActive, m_overlays.HasVisibleOverlays(),
        outW, outH, scalePlan, m_overlays, outBgra, nv12, bandPool);

    uint64_t lastCompCapSeq = capPkt ? capPkt->sequence : 0;
    uint64_t lastCompCamSeq = camFrame.sequence;
    SYSTEMTIME stInit{};
    GetLocalTime(&stInit);
    int lastCompSec = stInit.wSecond;
    bool haveCachedNv12 = true;

    DirectMftVideoEncoder encSession;
    std::vector<ComPtr<IMFSample>> prewarmPackets;
    std::wstring usedLabel = chosenEnc.friendlyName + L" \u00B7 " + chosenPreset.shortLabel;
    uint64_t encodedFrames = 0;
    bool ready = encSession.Init(chosenEnc, chosenPreset, outW, outH, fps, bitrate) &&
                 encSession.PrewarmAndCaptureHeader(*activeNv12, frameDur100ns, prewarmPackets, encodedFrames);

    if (!ready) {

        const auto all = EncoderManager::EnumerateAvailableEncoders();
        for (const auto& candidate : all) {
            if (candidate.id != L"auto" && !candidate.isHardware && !candidate.isHevc) {
                encSession.Reset();
                prewarmPackets.clear();
                encodedFrames = 0;
                const EncoderPreset fbPreset = EncoderManager::ResolvePreset(candidate, cfg.encoderPreset);
                if (encSession.Init(candidate, fbPreset, outW, outH, fps, bitrate) &&
                    encSession.PrewarmAndCaptureHeader(*activeNv12, frameDur100ns, prewarmPackets, encodedFrames)) {
                    usedLabel = candidate.friendlyName + L" (Fallback)";
                    ready = true;
                    break;
                }
            }
        }
    }

    ComPtr<IMFSinkWriter> writer;
    DWORD vidStream = 0, audStream = 0;
    bool hasAudio = false;
    if (ready) {
        ready = CreatePassthroughSinkWriter(targetPath, encSession.FinalOutputType(), audioKbps,
                                            writer, vidStream, audStream, hasAudio);
    }

    if (!ready || !writer) {
        encSession.Reset();
        m_capture.SetGpuNv12Target(false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_stats.isRecording = false;
        m_stats.encoderLabel = L"Encoder Init Failed";
        m_audio.SetRecordingState(false, false);
        m_isRecording.store(false);
        CoUninitialize();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_stats.encoderLabel = usedLabel;
    }

    uint64_t writtenPackets = 0;
    for (auto& pkt : prewarmPackets) {
        if (pkt && SUCCEEDED(writer->WriteSample(vidStream, pkt.Get()))) ++writtenPackets;
    }
    prewarmPackets.clear();

    HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t qpcFreq = QueryQpcFreq();
    const LONGLONG activeOffset100ns = static_cast<LONGLONG>(encodedFrames) * frameDur100ns;
    LONGLONG lastVideoPts = (encodedFrames > 0) ? (static_cast<LONGLONG>(encodedFrames - 1) * frameDur100ns) : -frameDur100ns;
    LONGLONG nextSlot100ns = activeOffset100ns;
    int64_t activeQpc = 0;
    int64_t lastQpc = QueryQpcNow();
    int64_t fpsStatStartQpc = lastQpc;
    uint64_t fpsStatFrames = encodedFrames;
    uint64_t writtenAudioFrames = 0;
    uint64_t lastUiCheckFrames = 0;
    uint64_t lastFileBytes = 0;
    std::vector<int16_t> audioPcm;
    std::vector<ComPtr<IMFSample>> encPackets;

    while (!m_stopRequested.load()) {
        const int64_t tickQpc = QueryQpcNow();
        const int64_t deltaQpc = tickQpc - lastQpc;
        lastQpc = tickQpc;

        if (m_isPaused.load()) {
            Sleep(25);
            continue;
        }

        activeQpc += std::max<int64_t>(0, deltaQpc);
        const double activeSec = static_cast<double>(activeQpc) / static_cast<double>(qpcFreq);
        const LONGLONG elapsedActive100ns = static_cast<LONGLONG>(activeSec * 10000000.0);
        const LONGLONG targetTimeline100ns = activeOffset100ns + elapsedActive100ns;

        if (targetTimeline100ns >= nextSlot100ns) {
            bool capChanged = false;
            if (useCaptureSrc) {
                FramePtr nextPkt = m_capture.GetLatestSharedFrame(lastCompCapSeq);
                if (nextPkt) {
                    capPkt = std::move(nextPkt);
                    capChanged = true;
                }
            }
            bool camChanged = false;
            if (camActive) {
                camChanged = m_camera.CopyLatestFrame(camFrame, lastCompCamSeq);
            }
            bool clockChanged = false;
            if (hasClockOverlay) {
                SYSTEMTIME stNow{};
                GetLocalTime(&stNow);
                if (static_cast<int>(stNow.wSecond) != lastCompSec) {
                    lastCompSec = stNow.wSecond;
                    clockChanged = true;
                }
            }

            if (!haveCachedNv12 || capChanged || camChanged || clockChanged) {
                activeNv12 = PrepareNv12Frame(capPkt, camFrame, cfg, useCaptureSrc, camActive, m_overlays.HasVisibleOverlays(),
                                              outW, outH, scalePlan, m_overlays, outBgra, nv12, bandPool);
                lastCompCapSeq = capPkt ? capPkt->sequence : 0;
                lastCompCamSeq = camFrame.sequence;
                haveCachedNv12 = true;
            }

            const LONGLONG jitterDiff = targetTimeline100ns - nextSlot100ns;
            const LONGLONG chosenPts = (jitterDiff < (frameDur100ns / 2)) ? nextSlot100ns : targetTimeline100ns;
            const LONGLONG samplePts = std::max(chosenPts, lastVideoPts + std::max<LONGLONG>(10000LL, frameDur100ns / 2));
            const LONGLONG sampleDur = std::clamp(samplePts - lastVideoPts, frameDur100ns, 2000000LL);

            if (targetTimeline100ns >= nextSlot100ns + frameDur100ns) {
                const LONGLONG stepIdx = (elapsedActive100ns / frameDur100ns) + 1;
                nextSlot100ns = activeOffset100ns + stepIdx * frameDur100ns;
            } else {
                nextSlot100ns += frameDur100ns;
            }

            encPackets.clear();
            const bool framePushed = encSession.PushFrame(activeNv12 ? *activeNv12 : nv12, samplePts, sampleDur, encPackets);
            for (auto& pkt : encPackets) {
                if (pkt && SUCCEEDED(writer->WriteSample(vidStream, pkt.Get()))) ++writtenPackets;
            }
            if (framePushed) {
                lastVideoPts = samplePts;
                ++encodedFrames;
                ++fpsStatFrames;
            }
        }

        if (hasAudio) {
            constexpr size_t kChunkFrames = 960;
            const uint64_t expectedAud = static_cast<uint64_t>(activeSec * static_cast<double>(AudioEngine::kTargetSampleRate));
            while (writtenAudioFrames + kChunkFrames <= expectedAud + kChunkFrames / 2) {
                m_audio.PullMixedPcm16(kChunkFrames, audioPcm);
                const LONGLONG audTime = static_cast<LONGLONG>((writtenAudioFrames * 10000000ULL) / AudioEngine::kTargetSampleRate);
                const LONGLONG audDur  = static_cast<LONGLONG>((kChunkFrames * 10000000ULL) / AudioEngine::kTargetSampleRate);
                WriteAudioPcmSample(writer.Get(), audStream, audioPcm, audTime, audDur);
                writtenAudioFrames += kChunkFrames;
            }
        }

        if (encodedFrames >= lastUiCheckFrames + std::max<uint64_t>(1, fps / 4)) {
            lastUiCheckFrames = encodedFrames;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(targetPath.c_str(), GetFileExInfoStandard, &fad)) {
                ULARGE_INTEGER uli{};
                uli.LowPart = fad.nFileSizeLow;
                uli.HighPart = fad.nFileSizeHigh;
                lastFileBytes = uli.QuadPart;
            }
            const double statSec = static_cast<double>(tickQpc - fpsStatStartQpc) / static_cast<double>(qpcFreq);
            double curFps = static_cast<double>(fps);
            if (statSec >= 0.35) {
                curFps = static_cast<double>(fpsStatFrames) / statSec;
                fpsStatFrames = 0;
                fpsStatStartQpc = tickQpc;
            }
            std::lock_guard<std::mutex> lk(m_statsMutex);
            m_stats.elapsedSec = activeSec;
            m_stats.actualFps = curFps;
            m_stats.encodedFrames = encodedFrames;
            m_stats.fileBytes = lastFileBytes;
        }

        const int64_t afterQpc = QueryQpcNow();
        const LONGLONG curTimeline100ns = activeOffset100ns +
            static_cast<LONGLONG>((static_cast<double>(activeQpc + std::max<int64_t>(0, afterQpc - tickQpc)) * 10000000.0) /
                                  static_cast<double>(qpcFreq));
        const LONGLONG wait100ns = nextSlot100ns - curTimeline100ns;
        if (wait100ns > 1500LL) {
            const LONGLONG clampedWait = std::min<LONGLONG>(250000LL, wait100ns);
            if (hTimer) {
                LARGE_INTEGER li{};
                li.QuadPart = -clampedWait;
                SetWaitableTimerEx(hTimer, &li, 0, nullptr, nullptr, nullptr, 0);
                WaitForSingleObject(hTimer, 30);
            } else {
                Sleep(1);
            }
        } else if (wait100ns > 0) {
            SwitchToThread();
        }
    }

    if (hTimer) CloseHandle(hTimer);
    m_capture.SetGpuNv12Target(false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    encPackets.clear();
    encSession.DrainAll(encPackets);
    for (auto& pkt : encPackets) {
        if (pkt && SUCCEEDED(writer->WriteSample(vidStream, pkt.Get()))) ++writtenPackets;
    }
    encPackets.clear();

    writer->Finalize();
    writer.Reset();
    encSession.Reset();

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(targetPath.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER uli{};
        uli.LowPart = fad.nFileSizeLow;
        uli.HighPart = fad.nFileSizeHigh;
        lastFileBytes = uli.QuadPart;
    }

    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_stats.isRecording = false;
        m_stats.isPaused = false;
        m_stats.fileBytes = lastFileBytes;
        if (writtenPackets > 0) m_lastCompletedFile = targetPath;
    }

    m_audio.SetRecordingState(false, false);
    m_isPaused.store(false);
    m_isRecording.store(false);
    CoUninitialize();
}

}
