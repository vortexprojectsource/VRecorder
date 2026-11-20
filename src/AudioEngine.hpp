#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrec {

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

class AudioEngine {
public:
    static constexpr uint32_t kTargetSampleRate = 48000;
    static constexpr uint32_t kTargetChannels   = 2;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void Start();
    void Stop();

    static std::vector<AudioDeviceInfo> EnumerateOutputDevices();
    static std::vector<AudioDeviceInfo> EnumerateInputDevices();

    void Configure(const std::wstring& desktopDeviceId, float desktopGain, bool desktopMuted,
                   const std::wstring& micDeviceId, float micGain, bool micMuted);

    void SetRecordingState(bool recording, bool paused);

    void PullMixedPcm16(size_t numFrames, std::vector<int16_t>& outInterleaved);

    float GetDesktopMeterLevel() const noexcept { return m_desktopMeter.load(); }
    float GetMicMeterLevel() const noexcept { return m_micMeter.load(); }
    bool IsMicActive() const noexcept { return m_micActive.load(); }

private:
    struct ResampleBuffer {
        std::deque<float> samplesL;
        std::deque<float> samplesR;
        std::mutex mutex;
    };

    void StreamLoop(bool isLoopback);
    void PushConvertedFrames(ResampleBuffer& buf, const uint8_t* raw, uint32_t frames,
                             uint32_t srcRate, uint16_t srcChannels, uint16_t bitsPerSample,
                             bool isFloat, std::atomic<float>& meter);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_recording{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_micActive{false};

    std::atomic<float> m_desktopGain{1.0f};
    std::atomic<bool>  m_desktopMuted{false};
    std::atomic<float> m_micGain{1.0f};
    std::atomic<bool>  m_micMuted{false};
    std::atomic<float> m_desktopMeter{0.0f};
    std::atomic<float> m_micMeter{0.0f};

    std::mutex m_devMutex;
    std::wstring m_desktopDevId;
    std::wstring m_micDevId;
    uint64_t m_desktopEpoch = 1;
    uint64_t m_micEpoch = 1;

    ResampleBuffer m_desktopBuf;
    ResampleBuffer m_micBuf;

    std::vector<float> m_mixDL;
    std::vector<float> m_mixDR;
    std::vector<float> m_mixML;
    std::vector<float> m_mixMR;

    std::thread m_desktopThread;
    std::thread m_micThread;
};

}
