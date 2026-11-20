#pragma once

#include "CaptureEngine.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrec {

struct CameraDeviceInfo {
    std::wstring id;
    std::wstring name;
};

class CameraEngine {
public:
    CameraEngine();
    ~CameraEngine();

    CameraEngine(const CameraEngine&) = delete;
    CameraEngine& operator=(const CameraEngine&) = delete;

    void Start();
    void Stop();

    static std::vector<CameraDeviceInfo> EnumerateCameras();

    void Configure(bool enabled, const std::wstring& deviceId, bool mirrorHorizontal);

    bool CopyLatestFrame(FrameCopy& outFrame, uint64_t lastKnownSeq) const;
    uint64_t GetLatestSequence() const noexcept { return m_latestSeqAtom.load(std::memory_order_acquire); }
    bool IsActive() const noexcept { return m_streamActive.load(); }
    std::wstring GetStatusText() const;

private:
    void CameraThreadProc();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_streamActive{false};
    std::thread m_thread;

    mutable std::mutex m_cfgMutex;
    bool m_enabled = false;
    std::wstring m_deviceId;
    bool m_mirror = true;
    uint64_t m_cfgEpoch = 1;

    mutable std::mutex m_frameMutex;
    std::vector<uint8_t> m_latestBgra;
    uint32_t m_latestW = 0;
    uint32_t m_latestH = 0;
    uint64_t m_frameSeq = 0;
    std::atomic<uint64_t> m_latestSeqAtom{0};
    std::wstring m_statusText = L"Camera Off";
};

}
