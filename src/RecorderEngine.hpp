#pragma once

#include "AudioEngine.hpp"
#include "CameraEngine.hpp"
#include "CaptureEngine.hpp"
#include "ConfigManager.hpp"
#include "EncoderManager.hpp"
#include "OverlayCompositor.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace vrec {

struct RecorderStats {
    bool isRecording = false;
    bool isPaused = false;
    double elapsedSec = 0.0;
    double actualFps = 0.0;
    uint64_t encodedFrames = 0;
    uint64_t droppedFrames = 0;
    uint64_t fileBytes = 0;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    std::wstring currentFilePath;
    std::wstring encoderLabel = L"Ready";
};

class RecorderEngine {
public:
    RecorderEngine(CaptureEngine& capture, CameraEngine& camera, AudioEngine& audio, OverlayCompositor& overlays);
    ~RecorderEngine();

    RecorderEngine(const RecorderEngine&) = delete;
    RecorderEngine& operator=(const RecorderEngine&) = delete;

    bool StartRecording(const AppConfig& cfg, const std::vector<DetectedEncoder>& encoders, std::wstring& outError);
    void StopRecording();
    void SetPaused(bool paused);

    void ResolveOutputDimensions(const AppConfig& cfg, uint32_t& outW, uint32_t& outH) const;

    static void ComputeSourcePlacement(uint32_t canvasW, uint32_t canvasH,
                                       uint32_t srcContentW, uint32_t srcContentH,
                                       const AppConfig& cfg,
                                       float& outDstX, float& outDstY, float& outDstW, float& outDstH);

    RecorderStats GetStats() const;
    std::wstring GetLastCompletedFile() const;

private:
    void WriterThreadProc(AppConfig cfg, DetectedEncoder chosenEnc, std::wstring targetPath, uint32_t outW, uint32_t outH);

    CaptureEngine&     m_capture;
    CameraEngine&      m_camera;
    AudioEngine&       m_audio;
    OverlayCompositor& m_overlays;

    std::atomic<bool> m_isRecording{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_isPaused{false};
    std::thread m_thread;

    mutable std::mutex m_statsMutex;
    RecorderStats m_stats{};
    std::wstring m_lastCompletedFile;
};

}
