#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrec {

struct MonitorInfo {
    int index = 0;
    UINT adapterIndex = 0;
    UINT outputIndex = 0;
    RECT desktopRect{};
    int width = 0;
    int height = 0;
    bool isPrimary = false;
    std::wstring label;
    std::wstring deviceName;
};

struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring processName;
    int width = 0;
    int height = 0;
};

struct FrameCopy {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sequence = 0;
    uint64_t targetEpoch = 0;
    std::vector<uint8_t> bgra;
    std::vector<uint8_t> nv12;
    uint32_t nv12Width = 0;
    uint32_t nv12Height = 0;
};

using FramePtr = std::shared_ptr<const FrameCopy>;

class CaptureEngine {
public:
    CaptureEngine();
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    void Start();
    void Stop();

    void ConfigureTarget(int sourceType, int monitorIndex, const std::wstring& windowTitle,
                         HWND excludeHwnd, bool showCursor, bool activeCaptureNeeded, int targetFps);

    void SetGpuNv12Target(bool enabled, uint32_t outW, uint32_t outH,
                          int cropX, int cropY, int cropW, int cropH,
                          int dstX, int dstY, int dstW, int dstH);
    uint64_t GetCurrentTargetEpoch() const noexcept;
    static bool GetMonitorBounds(int monitorIdx, int& outW, int& outH);

    static std::vector<MonitorInfo> EnumerateMonitors();
    static std::vector<WindowInfo> EnumerateWindows(HWND excludeHwnd);

    uint64_t GetLatestSequence() const noexcept { return m_latestSeqAtom.load(std::memory_order_acquire); }

    FramePtr GetLatestSharedFrame(uint64_t lastKnownSeq) const;

    bool CopyLatestFrame(FrameCopy& outFrame, uint64_t lastKnownSeq) const;
    bool GetCurrentDimensions(uint32_t& outW, uint32_t& outH) const;
    double GetMeasuredCaptureFps() const;
    std::wstring GetBackendName() const;
    bool HasActiveFrame() const;

private:
    enum class MapStatus { Ok, StillDrawing, Failed };
    struct FramePool;

    struct GpuTransformCfg {
        bool enabled = false;
        bool isPreviewDownscale = false;
        uint32_t outW = 0;
        uint32_t outH = 0;
        RECT srcRect{};
        RECT dstRect{};
    };

    void CaptureThreadProc();
    std::shared_ptr<FrameCopy> AcquirePooledFrame();
    bool InitDxgiForMonitor(int monitorIdx, const std::vector<MonitorInfo>& mons);
    void ReleaseDxgi();
    void ReleaseVideoProcessor();
    bool EnsureBicubicShaders();
    void RunBicubicGpuPass(const RECT& srcRc, const RECT& dstRc);
    bool EnsureVideoProcessor(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH,
                              bool outIsNv12, const RECT& srcRc, const RECT& dstRc);
    MapStatus MapStagingToRaw(int slotIdx, bool allowWait, bool copyNv12, uint32_t mapW, uint32_t mapH);
    bool CaptureMonitorDxgi(int monitorIdx, const std::vector<MonitorInfo>& mons, bool showCursor,
                            const GpuTransformCfg& gpuCfg, FrameCopy& outFrame, bool& outNewPublish);
    bool CaptureMonitorGdi(const MonitorInfo& mon, bool showCursor, std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH);
    bool CaptureWindowGdi(HWND targetHwnd, bool showCursor, std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH);
    void EnsureGdiScratch(int w, int h);
    void ReleaseGdiScratch();

    mutable std::mutex m_cfgMutex;
    int m_sourceType = 0;
    int m_monitorIndex = 0;
    std::wstring m_windowTitle;
    HWND m_excludeHwnd = nullptr;
    bool m_showCursor = true;
    bool m_activeCapture = true;
    int m_targetFps = 60;
    uint64_t m_cfgEpoch = 1;
    bool m_gpuNv12Enabled = false;
    uint32_t m_gpuOutW = 0;
    uint32_t m_gpuOutH = 0;
    int m_gpuCropX = 0, m_gpuCropY = 0, m_gpuCropW = 0, m_gpuCropH = 0;
    int m_gpuDstX = 0, m_gpuDstY = 0, m_gpuDstW = 0, m_gpuDstH = 0;
    std::atomic<uint64_t> m_cfgEpochAtom{1};

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    std::shared_ptr<FramePool> m_framePool;
    mutable std::mutex m_frameMutex;
    FramePtr m_latestFrame;
    uint64_t m_frameSeq = 0;
    std::atomic<uint64_t> m_latestSeqAtom{0};
    std::atomic<double> m_measuredFps{0.0};
    std::wstring m_backendName = L"DXGI";

    static constexpr int kStagingCount = 3;
    int m_dxgiFailStreak = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dCtx;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_dupl;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTex[kStagingCount];
    int m_stagingWriteIdx = 0;
    int m_stagingPendingIdx = -1;
    int m_duplMonitorIdx = -1;
    uint32_t m_duplW = 0;
    uint32_t m_duplH = 0;
    RECT m_duplDesktopRect{};
    int64_t m_nextDxgiRetryQpc = 0;
    std::vector<uint8_t> m_rawDxgiBgra;
    std::vector<uint8_t> m_rawDxgiNv12;
    uint32_t m_rawDxgiW = 0;
    uint32_t m_rawDxgiH = 0;
    uint32_t m_rawNv12W = 0;
    uint32_t m_rawNv12H = 0;
    RECT m_rawNv12SrcRect{};
    RECT m_rawNv12DstRect{};
    POINT m_lastCurPt{-99999, -99999};
    HCURSOR m_lastCurH = nullptr;
    bool m_lastCurShown = false;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_vDev;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_vCtx;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_vp;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_vpInTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_vpOutTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_vpStaging[kStagingCount];
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_vpInView;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_vpOutView;
    uint32_t m_vpInW = 0, m_vpInH = 0, m_vpOutW = 0, m_vpOutH = 0;
    bool m_vpOutIsNv12 = false;
    bool m_vpUseBicubic = false;
    bool m_vpDisabled = false;
    RECT m_vpSrcRect{};
    RECT m_vpDstRect{};
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_bicVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_bicPs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_bicCb;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_bicRs;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_bicSrcTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_bicSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_bicDstTex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_bicRtv;

    HDC m_gdiDc = nullptr;
    HBITMAP m_gdiBmp = nullptr;
    HBITMAP m_gdiOldBmp = nullptr;
    void* m_gdiBits = nullptr;
    int m_gdiW = 0;
    int m_gdiH = 0;
};

}
