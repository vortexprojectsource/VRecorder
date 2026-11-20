#include "AudioEngine.hpp"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace vrec {

namespace {

std::wstring GetFriendlyName(IMMDevice* dev) {
    ComPtr<IPropertyStore> store;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, store.GetAddressOf()))) return L"Audio Device";
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring name = L"Audio Device";
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        name = pv.pwszVal;
    }
    PropVariantClear(&pv);
    return name;
}

std::vector<AudioDeviceInfo> EnumerateEndpoints(EDataFlow flow) {
    std::vector<AudioDeviceInfo> list;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return list;
    }

    std::wstring defId;
    ComPtr<IMMDevice> defDev;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, defDev.GetAddressOf())) && defDev) {
        LPWSTR rawDef = nullptr;
        if (SUCCEEDED(defDev->GetId(&rawDef)) && rawDef) {
            defId = rawDef;
            CoTaskMemFree(rawDef);
        }
    }

    ComPtr<IMMDeviceCollection> col;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, col.GetAddressOf())) || !col) {
        return list;
    }

    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, dev.GetAddressOf())) || !dev) continue;
        LPWSTR rawId = nullptr;
        if (FAILED(dev->GetId(&rawId)) || !rawId) continue;
        AudioDeviceInfo info{};
        info.id = rawId;
        CoTaskMemFree(rawId);
        info.name = GetFriendlyName(dev.Get());
        info.isDefault = (!defId.empty() && info.id == defId);
        list.push_back(std::move(info));
    }
    return list;
}

bool ParseWaveFormat(const WAVEFORMATEX* pwfx, uint16_t& outBits, bool& outFloat) {
    if (!pwfx) return false;
    outBits = pwfx->wBitsPerSample;
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        outFloat = true;
        return outBits == 32;
    }
    if (pwfx->wFormatTag == WAVE_FORMAT_PCM) {
        outFloat = false;
        return outBits == 16 || outBits == 24 || outBits == 32;
    }
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            outFloat = true;
            return outBits == 32;
        }
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            outFloat = false;
            return outBits == 16 || outBits == 24 || outBits == 32;
        }
    }
    return false;
}

float ReadSampleAsFloat(const uint8_t* ptr, uint16_t bits, bool isFloat) noexcept {
    if (isFloat && bits == 32) {
        float v = 0.0f;
        std::memcpy(&v, ptr, sizeof(float));
        if (!std::isfinite(v)) return 0.0f;
        return std::clamp(v, -1.0f, 1.0f);
    }
    if (!isFloat && bits == 16) {
        int16_t v = 0;
        std::memcpy(&v, ptr, sizeof(int16_t));
        return static_cast<float>(v) / 32768.0f;
    }
    if (!isFloat && bits == 24) {
        int32_t v = (static_cast<int32_t>(static_cast<int8_t>(ptr[2])) << 16) |
                    (static_cast<int32_t>(ptr[1]) << 8) |
                    static_cast<int32_t>(ptr[0]);
        return static_cast<float>(v) / 8388608.0f;
    }
    if (!isFloat && bits == 32) {
        int32_t v = 0;
        std::memcpy(&v, ptr, sizeof(int32_t));
        return static_cast<float>(static_cast<double>(v) / 2147483648.0);
    }
    return 0.0f;
}

}

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { Stop(); }

void AudioEngine::Start() {
    if (m_running.exchange(true)) return;
    m_desktopThread = std::thread(&AudioEngine::StreamLoop, this, true);
    m_micThread     = std::thread(&AudioEngine::StreamLoop, this, false);
}

void AudioEngine::Stop() {
    if (!m_running.exchange(false)) return;
    if (m_desktopThread.joinable()) m_desktopThread.join();
    if (m_micThread.joinable()) m_micThread.join();
}

std::vector<AudioDeviceInfo> AudioEngine::EnumerateOutputDevices() {
    return EnumerateEndpoints(eRender);
}

std::vector<AudioDeviceInfo> AudioEngine::EnumerateInputDevices() {
    return EnumerateEndpoints(eCapture);
}

void AudioEngine::Configure(const std::wstring& desktopDeviceId, float desktopGain, bool desktopMuted,
                            const std::wstring& micDeviceId, float micGain, bool micMuted) {
    m_desktopGain.store(std::clamp(desktopGain, 0.0f, 1.0f));
    m_desktopMuted.store(desktopMuted);
    m_micGain.store(std::clamp(micGain, 0.0f, 1.0f));
    m_micMuted.store(micMuted);

    std::lock_guard<std::mutex> lk(m_devMutex);
    if (m_desktopDevId != desktopDeviceId) {
        m_desktopDevId = desktopDeviceId;
        ++m_desktopEpoch;
    }
    if (m_micDevId != micDeviceId) {
        m_micDevId = micDeviceId;
        ++m_micEpoch;
    }
}

void AudioEngine::SetRecordingState(bool recording, bool paused) {
    if (recording && !m_recording.load()) {

        {
            std::lock_guard<std::mutex> lk(m_desktopBuf.mutex);
            m_desktopBuf.samplesL.clear();
            m_desktopBuf.samplesR.clear();
        }
        {
            std::lock_guard<std::mutex> lk(m_micBuf.mutex);
            m_micBuf.samplesL.clear();
            m_micBuf.samplesR.clear();
        }
    }
    m_paused.store(paused);
    m_recording.store(recording);
}

void AudioEngine::PushConvertedFrames(ResampleBuffer& buf, const uint8_t* raw, uint32_t frames,
                                      uint32_t srcRate, uint16_t srcChannels, uint16_t bitsPerSample,
                                      bool isFloat, std::atomic<float>& meter) {
    if (!raw || frames == 0 || srcRate == 0 || srcChannels == 0) return;
    const uint32_t bytesPerSample = bitsPerSample / 8;
    const uint32_t blockAlign = bytesPerSample * srcChannels;

    thread_local std::vector<float> tmpL;
    thread_local std::vector<float> tmpR;
    tmpL.resize(frames);
    tmpR.resize(frames);
    float peak = 0.0f;

    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t* fptr = raw + static_cast<size_t>(i) * blockAlign;
        const float l = ReadSampleAsFloat(fptr, bitsPerSample, isFloat);
        const float r = (srcChannels >= 2)
            ? ReadSampleAsFloat(fptr + bytesPerSample, bitsPerSample, isFloat)
            : l;
        tmpL[i] = l;
        tmpR[i] = r;
        peak = std::max({peak, std::fabs(l), std::fabs(r)});
    }

    const float prev = meter.load();
    const float nextMeter = (peak > prev) ? peak : (prev * 0.72f + peak * 0.28f);
    meter.store(std::clamp(nextMeter, 0.0f, 1.0f));

    if (!m_recording.load() || m_paused.load()) return;

    const double ratio = static_cast<double>(srcRate) / static_cast<double>(kTargetSampleRate);
    const size_t outCount = (srcRate == kTargetSampleRate)
        ? frames
        : static_cast<size_t>(std::max<double>(1.0, std::floor(static_cast<double>(frames) / ratio)));

    std::lock_guard<std::mutex> lk(buf.mutex);
    if (srcRate == kTargetSampleRate) {
        for (uint32_t i = 0; i < frames; ++i) {
            buf.samplesL.push_back(tmpL[i]);
            buf.samplesR.push_back(tmpR[i]);
        }
    } else {
        for (size_t k = 0; k < outCount; ++k) {
            const double pos = static_cast<double>(k) * ratio;
            const size_t i0 = std::min(static_cast<size_t>(pos), static_cast<size_t>(frames - 1));
            const size_t i1 = std::min(i0 + 1, static_cast<size_t>(frames - 1));
            const float frac = static_cast<float>(pos - static_cast<double>(i0));
            buf.samplesL.push_back(tmpL[i0] + (tmpL[i1] - tmpL[i0]) * frac);
            buf.samplesR.push_back(tmpR[i0] + (tmpR[i1] - tmpR[i0]) * frac);
        }
    }

    constexpr size_t kMaxQueueFrames = kTargetSampleRate * 2 / 5;
    while (buf.samplesL.size() > kMaxQueueFrames) {
        buf.samplesL.pop_front();
        buf.samplesR.pop_front();
    }
}

void AudioEngine::PullMixedPcm16(size_t numFrames, std::vector<int16_t>& outInterleaved) {
    outInterleaved.assign(numFrames * 2, 0);
    if (numFrames == 0) return;

    const float dGain = m_desktopMuted.load() ? 0.0f : m_desktopGain.load();
    const float mGain = (m_micMuted.load() || !m_micActive.load()) ? 0.0f : m_micGain.load();

    m_mixDL.assign(numFrames, 0.0f);
    m_mixDR.assign(numFrames, 0.0f);
    m_mixML.assign(numFrames, 0.0f);
    m_mixMR.assign(numFrames, 0.0f);

    if (dGain > 0.0001f) {
        std::lock_guard<std::mutex> lk(m_desktopBuf.mutex);
        const size_t avail = std::min(numFrames, m_desktopBuf.samplesL.size());
        for (size_t i = 0; i < avail; ++i) {
            m_mixDL[i] = m_desktopBuf.samplesL[i];
            m_mixDR[i] = m_desktopBuf.samplesR[i];
        }
        if (avail > 0) {
            m_desktopBuf.samplesL.erase(m_desktopBuf.samplesL.begin(), m_desktopBuf.samplesL.begin() + static_cast<ptrdiff_t>(avail));
            m_desktopBuf.samplesR.erase(m_desktopBuf.samplesR.begin(), m_desktopBuf.samplesR.begin() + static_cast<ptrdiff_t>(avail));
        }
    } else {
        std::lock_guard<std::mutex> lk(m_desktopBuf.mutex);
        m_desktopBuf.samplesL.clear();
        m_desktopBuf.samplesR.clear();
    }

    if (mGain > 0.0001f) {
        std::lock_guard<std::mutex> lk(m_micBuf.mutex);
        const size_t avail = std::min(numFrames, m_micBuf.samplesL.size());
        for (size_t i = 0; i < avail; ++i) {
            m_mixML[i] = m_micBuf.samplesL[i];
            m_mixMR[i] = m_micBuf.samplesR[i];
        }
        if (avail > 0) {
            m_micBuf.samplesL.erase(m_micBuf.samplesL.begin(), m_micBuf.samplesL.begin() + static_cast<ptrdiff_t>(avail));
            m_micBuf.samplesR.erase(m_micBuf.samplesR.begin(), m_micBuf.samplesR.begin() + static_cast<ptrdiff_t>(avail));
        }
    } else {
        std::lock_guard<std::mutex> lk(m_micBuf.mutex);
        m_micBuf.samplesL.clear();
        m_micBuf.samplesR.clear();
    }

    for (size_t i = 0; i < numFrames; ++i) {
        const float mixL = std::clamp(m_mixDL[i] * dGain + m_mixML[i] * mGain, -1.0f, 1.0f);
        const float mixR = std::clamp(m_mixDR[i] * dGain + m_mixMR[i] * mGain, -1.0f, 1.0f);
        outInterleaved[i * 2]     = static_cast<int16_t>(std::lround(mixL * 32767.0f));
        outInterleaved[i * 2 + 1] = static_cast<int16_t>(std::lround(mixR * 32767.0f));
    }
}

void AudioEngine::StreamLoop(bool isLoopback) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    uint64_t seenEpoch = 0;
    while (m_running.load()) {
        std::wstring targetId;
        uint64_t curEpoch = 0;
        {
            std::lock_guard<std::mutex> lk(m_devMutex);
            targetId = isLoopback ? m_desktopDevId : m_micDevId;
            curEpoch = isLoopback ? m_desktopEpoch : m_micEpoch;
        }
        seenEpoch = curEpoch;

        if (!isLoopback && _wcsicmp(targetId.c_str(), L"none") == 0) {
            m_micActive.store(false);
            m_micMeter.store(0.0f);
            Sleep(120);
            continue;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator))) || !enumerator) {
            if (!isLoopback) { m_micActive.store(false); m_micMeter.store(0.0f); }
            else m_desktopMeter.store(0.0f);
            Sleep(500);
            continue;
        }

        ComPtr<IMMDevice> device;
        HRESULT hr = E_FAIL;
        if (!targetId.empty()) {
            hr = enumerator->GetDevice(targetId.c_str(), device.GetAddressOf());
        }
        if (FAILED(hr) || !device) {
            hr = enumerator->GetDefaultAudioEndpoint(isLoopback ? eRender : eCapture, eConsole, device.GetAddressOf());
        }
        if (FAILED(hr) || !device) {
            if (!isLoopback) { m_micActive.store(false); m_micMeter.store(0.0f); }
            else m_desktopMeter.store(0.0f);
            Sleep(500);
            continue;
        }

        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client.GetAddressOf()))) || !client) {
            if (!isLoopback) m_micActive.store(false);
            Sleep(500);
            continue;
        }

        WAVEFORMATEX* pwfx = nullptr;
        if (FAILED(client->GetMixFormat(&pwfx)) || !pwfx) {
            if (!isLoopback) m_micActive.store(false);
            Sleep(500);
            continue;
        }

        uint16_t bitsPerSample = 32;
        bool isFloat = true;
        if (!ParseWaveFormat(pwfx, bitsPerSample, isFloat)) {
            CoTaskMemFree(pwfx);
            if (!isLoopback) m_micActive.store(false);
            Sleep(500);
            continue;
        }

        const uint32_t srcRate = pwfx->nSamplesPerSec;
        const uint16_t srcCh   = pwfx->nChannels;
        const DWORD flags = isLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        const REFERENCE_TIME bufDur = 400000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufDur, 0, pwfx, nullptr);
        CoTaskMemFree(pwfx);
        if (FAILED(hr)) {
            if (!isLoopback) m_micActive.store(false);
            Sleep(500);
            continue;
        }

        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(capture.GetAddressOf()))) || !capture) {
            if (!isLoopback) m_micActive.store(false);
            Sleep(500);
            continue;
        }

        if (FAILED(client->Start())) {
            if (!isLoopback) m_micActive.store(false);
            Sleep(500);
            continue;
        }

        if (!isLoopback) m_micActive.store(true);
        ResampleBuffer& targetBuf = isLoopback ? m_desktopBuf : m_micBuf;
        std::atomic<float>& meter = isLoopback ? m_desktopMeter : m_micMeter;

        while (m_running.load()) {
            {
                std::lock_guard<std::mutex> lk(m_devMutex);
                if (seenEpoch != (isLoopback ? m_desktopEpoch : m_micEpoch)) break;
            }

            UINT32 packetLen = 0;
            hr = capture->GetNextPacketSize(&packetLen);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(hr)) {
                break;
            }

            bool gotAudio = false;
            while (packetLen > 0) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD pktFlags = 0;
                hr = capture->GetBuffer(&pData, &numFrames, &pktFlags, nullptr, nullptr);
                if (FAILED(hr)) break;

                gotAudio = true;
                if (pktFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    const float cur = meter.load();
                    meter.store(cur * 0.72f);
                } else {
                    PushConvertedFrames(targetBuf, pData, numFrames, srcRate, srcCh,
                                        bitsPerSample, isFloat, meter);
                }
                capture->ReleaseBuffer(numFrames);

                hr = capture->GetNextPacketSize(&packetLen);
                if (FAILED(hr)) break;
            }

            if (!gotAudio) {

                const float cur = meter.load();
                if (cur > 0.002f) meter.store(cur * 0.80f);
                else if (cur != 0.0f) meter.store(0.0f);
            }
            Sleep(8);
        }

        client->Stop();
        if (!isLoopback) {
            m_micActive.store(false);
            m_micMeter.store(0.0f);
        } else {
            m_desktopMeter.store(0.0f);
        }
    }

    CoUninitialize();
}

}
