#include "CameraEngine.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace vrec {

namespace {

inline uint8_t ClampByte(int v) noexcept {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void YuvToBgra(int y, int u, int v, uint8_t* dst) noexcept {
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    const int r = (298 * c + 409 * e + 128) >> 8;
    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    const int b = (298 * c + 516 * d + 128) >> 8;
    dst[0] = ClampByte(b);
    dst[1] = ClampByte(g);
    dst[2] = ClampByte(r);
    dst[3] = 255;
}

void ConvertYuy2ToBgra(const uint8_t* src, int srcStride, uint32_t w, uint32_t h,
                       bool mirror, std::vector<uint8_t>& outBgra) {
    outBgra.resize(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* sRow = (srcStride >= 0)
            ? (src + static_cast<size_t>(y) * static_cast<size_t>(srcStride))
            : (src + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(-srcStride));
        uint8_t* dRow = outBgra.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x + 1 < w; x += 2) {
            const int y0 = sRow[x * 2 + 0];
            const int u  = sRow[x * 2 + 1];
            const int y1 = sRow[x * 2 + 2];
            const int v  = sRow[x * 2 + 3];
            const uint32_t dx0 = mirror ? (w - 1 - x) : x;
            const uint32_t dx1 = mirror ? (w - 2 - x) : (x + 1);
            YuvToBgra(y0, u, v, dRow + dx0 * 4);
            YuvToBgra(y1, u, v, dRow + dx1 * 4);
        }
    }
}

void ConvertNv12ToBgra(const uint8_t* src, int srcStride, uint32_t w, uint32_t h,
                       bool mirror, std::vector<uint8_t>& outBgra) {
    const uint32_t absStride = static_cast<uint32_t>(std::max<int>(static_cast<int>(w), std::abs(srcStride)));
    const uint8_t* uvBase = src + static_cast<size_t>(absStride) * h;
    outBgra.resize(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* yRow = src + static_cast<size_t>(y) * absStride;
        const uint8_t* uvRow = uvBase + static_cast<size_t>(y / 2) * absStride;
        uint8_t* dRow = outBgra.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t uvIdx = (x & ~1u);
            const uint32_t dx = mirror ? (w - 1 - x) : x;
            YuvToBgra(yRow[x], uvRow[uvIdx], uvRow[uvIdx + 1], dRow + dx * 4);
        }
    }
}

void CopyRgb32ToBgra(const uint8_t* src, int srcStride, uint32_t w, uint32_t h,
                     bool mirror, std::vector<uint8_t>& outBgra) {
    outBgra.resize(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* sRow = (srcStride >= 0)
            ? (src + static_cast<size_t>(y) * static_cast<size_t>(srcStride))
            : (src + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(-srcStride));
        uint8_t* dRow = outBgra.data() + static_cast<size_t>(y) * w * 4;
        if (!mirror) {
            for (uint32_t x = 0; x < w; ++x) {
                dRow[x * 4 + 0] = sRow[x * 4 + 0];
                dRow[x * 4 + 1] = sRow[x * 4 + 1];
                dRow[x * 4 + 2] = sRow[x * 4 + 2];
                dRow[x * 4 + 3] = 255;
            }
        } else {
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t sx = w - 1 - x;
                dRow[x * 4 + 0] = sRow[sx * 4 + 0];
                dRow[x * 4 + 1] = sRow[sx * 4 + 1];
                dRow[x * 4 + 2] = sRow[sx * 4 + 2];
                dRow[x * 4 + 3] = 255;
            }
        }
    }
}

void ChooseBestNativeResolution(IMFSourceReader* reader) {
    ComPtr<IMFMediaType> bestType;
    double bestScore = -1.0;

    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, mt.GetAddressOf()))) {
            break;
        }
        UINT32 w = 0, h = 0, fpsNum = 30, fpsDen = 1;
        MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        if (w < 320 || h < 240 || fpsDen == 0) continue;

        const double fps = static_cast<double>(fpsNum) / static_cast<double>(fpsDen);

        double score = fps * 10.0;
        if (w >= 640 && w <= 1280 && h >= 360 && h <= 720) score += 500.0;
        if (w == 1280 && h == 720) score += 120.0;
        if (w == 640 && h == 480) score += 90.0;
        if (w > 1280) score -= 200.0;

        GUID sub = GUID_NULL;
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        if (IsEqualGUID(sub, MFVideoFormat_NV12) || IsEqualGUID(sub, MFVideoFormat_YUY2) || IsEqualGUID(sub, MFVideoFormat_RGB32)) {
            score += 40.0;
        }

        if (score > bestScore) {
            bestScore = score;
            bestType = mt;
        }
    }

    if (bestType) {
        reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, bestType.Get());
    }
}

}

CameraEngine::CameraEngine() = default;

CameraEngine::~CameraEngine() {
    Stop();
}

void CameraEngine::Start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread(&CameraEngine::CameraThreadProc, this);
}

void CameraEngine::Stop() {
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void CameraEngine::Configure(bool enabled, const std::wstring& deviceId, bool mirrorHorizontal) {
    std::lock_guard<std::mutex> lk(m_cfgMutex);
    if (m_enabled != enabled || m_deviceId != deviceId || m_mirror != mirrorHorizontal) {
        m_enabled = enabled;
        m_deviceId = deviceId;
        m_mirror = mirrorHorizontal;
        ++m_cfgEpoch;
    }
}

std::vector<CameraDeviceInfo> CameraEngine::EnumerateCameras() {
    std::vector<CameraDeviceInfo> out;
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(attr.GetAddressOf(), 1))) return out;
    if (FAILED(attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return out;
    }

    IMFActivate** ppDev = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attr.Get(), &ppDev, &count)) || !ppDev) return out;

    for (UINT32 i = 0; i < count; ++i) {
        if (!ppDev[i]) continue;
        wchar_t* fname = nullptr;
        UINT32 flen = 0;
        wchar_t* slink = nullptr;
        UINT32 slen = 0;
        ppDev[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fname, &flen);
        ppDev[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &slink, &slen);

        CameraDeviceInfo ci{};
        ci.name = fname ? fname : L"USB Camera";
        ci.id = slink ? slink : ci.name;
        if (fname) CoTaskMemFree(fname);
        if (slink) CoTaskMemFree(slink);

        out.push_back(std::move(ci));
        ppDev[i]->Release();
    }
    CoTaskMemFree(ppDev);
    return out;
}

bool CameraEngine::CopyLatestFrame(FrameCopy& outFrame, uint64_t lastKnownSeq) const {
    std::lock_guard<std::mutex> lk(m_frameMutex);
    if (m_frameSeq == 0 || m_frameSeq == lastKnownSeq || m_latestBgra.empty()) return false;
    outFrame.width = m_latestW;
    outFrame.height = m_latestH;
    outFrame.sequence = m_frameSeq;
    outFrame.bgra = m_latestBgra;
    return true;
}

std::wstring CameraEngine::GetStatusText() const {
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_statusText;
}

void CameraEngine::CameraThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (m_running.load()) {
        bool enabled = false;
        std::wstring devId;
        bool mirror = true;
        uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lk(m_cfgMutex);
            enabled = m_enabled;
            devId = m_deviceId;
            mirror = m_mirror;
            epoch = m_cfgEpoch;
        }

        if (!enabled) {
            m_streamActive.store(false);
            {
                std::lock_guard<std::mutex> lk(m_frameMutex);
                if (!m_latestBgra.empty() || m_statusText != L"Camera Off") {
                    m_latestBgra.clear();
                    m_latestW = 0;
                    m_latestH = 0;
                    ++m_frameSeq;
                    m_statusText = L"Camera Off";
                }
            }
            Sleep(60);
            continue;
        }

        ComPtr<IMFAttributes> enumAttr;
        IMFActivate** ppDev = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFCreateAttributes(enumAttr.GetAddressOf(), 1)) &&
            SUCCEEDED(enumAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
            MFEnumDeviceSources(enumAttr.Get(), &ppDev, &count);
        }

        ComPtr<IMFActivate> pickedAct;
        std::wstring pickedName = L"Webcam";
        if (ppDev && count > 0) {
            for (UINT32 i = 0; i < count; ++i) {
                if (!ppDev[i]) continue;
                wchar_t* slink = nullptr;
                UINT32 slen = 0;
                ppDev[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &slink, &slen);
                bool match = devId.empty() || (slink && _wcsicmp(slink, devId.c_str()) == 0);
                if (slink) CoTaskMemFree(slink);
                if (match && !pickedAct) {
                    pickedAct = ppDev[i];
                    wchar_t* fname = nullptr;
                    UINT32 flen = 0;
                    if (SUCCEEDED(ppDev[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fname, &flen)) && fname) {
                        pickedName = fname;
                        CoTaskMemFree(fname);
                    }
                }
                ppDev[i]->Release();
            }
            if (!pickedAct) {

            }
            CoTaskMemFree(ppDev);
        } else if (ppDev) {
            CoTaskMemFree(ppDev);
        }

        if (!pickedAct) {
            m_streamActive.store(false);
            {
                std::lock_guard<std::mutex> lk(m_frameMutex);
                m_statusText = L"No Camera Detected";
                m_latestBgra.clear();
                m_latestW = 0;
                m_latestH = 0;
            }
            Sleep(500);
            continue;
        }

        ComPtr<IMFMediaSource> mediaSource;
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFAttributes> rAttr;
        MFCreateAttributes(rAttr.GetAddressOf(), 1);
        if (rAttr) rAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        bool opened = SUCCEEDED(pickedAct->ActivateObject(IID_PPV_ARGS(&mediaSource))) &&
                      mediaSource &&
                      SUCCEEDED(MFCreateSourceReaderFromMediaSource(mediaSource.Get(), rAttr.Get(), reader.GetAddressOf())) &&
                      reader;

        if (!opened) {
            if (mediaSource) mediaSource->Shutdown();
            m_streamActive.store(false);
            {
                std::lock_guard<std::mutex> lk(m_frameMutex);
                m_statusText = L"Camera Busy / Blocked";
                m_latestBgra.clear();
            }
            Sleep(600);
            continue;
        }

        ChooseBestNativeResolution(reader.Get());

        const GUID trySubs[3] = {MFVideoFormat_RGB32, MFVideoFormat_YUY2, MFVideoFormat_NV12};
        GUID activeSub = GUID_NULL;
        for (const auto& sub : trySubs) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(MFCreateMediaType(t.GetAddressOf()))) {
                t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                t->SetGUID(MF_MT_SUBTYPE, sub);
                if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, t.Get()))) {
                    activeSub = sub;
                    break;
                }
            }
        }

        UINT32 camW = 0, camH = 0;
        LONG defStride = 0;
        ComPtr<IMFMediaType> curType;
        if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), curType.GetAddressOf())) && curType) {
            MFGetAttributeSize(curType.Get(), MF_MT_FRAME_SIZE, &camW, &camH);
            curType->GetGUID(MF_MT_SUBTYPE, &activeSub);
            UINT32 stVal = 0;
            if (SUCCEEDED(curType->GetUINT32(MF_MT_DEFAULT_STRIDE, &stVal))) {
                defStride = static_cast<LONG>(stVal);
            }
        }
        if (defStride == 0) {
            defStride = IsEqualGUID(activeSub, MFVideoFormat_RGB32) ? static_cast<LONG>(camW * 4)
                      : (IsEqualGUID(activeSub, MFVideoFormat_YUY2) ? static_cast<LONG>(camW * 2) : static_cast<LONG>(camW));
        }

        m_streamActive.store(true);
        {
            std::lock_guard<std::mutex> lk(m_frameMutex);
            m_statusText = pickedName + L" (" + std::to_wstring(camW) + L"x" + std::to_wstring(camH) + L")";
        }

        std::vector<uint8_t> workBuf;
        while (m_running.load()) {
            bool curMirror = true;
            {
                std::lock_guard<std::mutex> lk(m_cfgMutex);
                if (!m_enabled || m_cfgEpoch != epoch) break;
                curMirror = m_mirror;
            }

            DWORD streamIdx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            const HRESULT rhr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                                   0, &streamIdx, &flags, &ts, smp.GetAddressOf());
            if (FAILED(rhr) || (flags & MF_SOURCE_READERF_ERROR) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                break;
            }
            if (smp && camW > 0 && camH > 0) {
                ComPtr<IMFMediaBuffer> buf;
                if (SUCCEEDED(smp->ConvertToContiguousBuffer(buf.GetAddressOf())) && buf) {
                    BYTE* ptr = nullptr;
                    DWORD cb = 0;
                    if (SUCCEEDED(buf->Lock(&ptr, nullptr, &cb)) && ptr && cb > 0) {
                        if (IsEqualGUID(activeSub, MFVideoFormat_RGB32)) {
                            CopyRgb32ToBgra(ptr, defStride, camW, camH, curMirror, workBuf);
                        } else if (IsEqualGUID(activeSub, MFVideoFormat_YUY2)) {
                            ConvertYuy2ToBgra(ptr, defStride, camW, camH, curMirror, workBuf);
                        } else if (IsEqualGUID(activeSub, MFVideoFormat_NV12)) {
                            ConvertNv12ToBgra(ptr, defStride, camW, camH, curMirror, workBuf);
                        }
                        buf->Unlock();
                        if (!workBuf.empty()) {
                            std::lock_guard<std::mutex> lk(m_frameMutex);
                            m_latestW = camW;
                            m_latestH = camH;
                            m_latestBgra.swap(workBuf);
                            ++m_frameSeq;
                            m_latestSeqAtom.store(m_frameSeq, std::memory_order_release);
                        }
                    }
                }
            }
        }

        m_streamActive.store(false);
        reader.Reset();
        if (mediaSource) {
            mediaSource->Shutdown();
            mediaSource.Reset();
        }
    }

    CoUninitialize();
}

}
