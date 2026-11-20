# VRecorder Beta — Windows Native Screen & Audio Studio Recorder

**VRecorder Beta** is an open-source, ultra-lightweight standalone native screen, webcam, and audio studio recorder for **Windows 10 / 11** (all builds, including LTSC & stripped installations) and legacy Windows environments, written in **C++20**, **Win32**, and pure **Direct2D / DirectWrite / DXGI / D3D11 / WASAPI / Windows Media Foundation**. It statically links the MSVC runtime (`/MT`) with zero Qt, zero Chromium/CEF, zero Electron, zero Python, and zero external FFmpeg runtime dependencies so it launches reliably everywhere out of the box in a single 879 KB binary.

> [!NOTE]
> **Project Identity**
> - **Product Name:** `VRecorder Beta`
> - **File Description:** `VRecorder Beta - Windows Native Screen & Audio Studio Recorder`
> - **Copyright:** `Vortex Open Source Project`
> - **Created by:** `@vinnotdkk`
> - **Repository:** https://github.com/vortexprojectsource/VRecorder
> - **Support Development:** https://sociabuzz.com/idnvinnmalay

---

## Table of Contents

1. [What's New in Beta](#whats-new-in-beta)
2. [Architecture Overview](#architecture-overview)
3. [Technical Stack & Linked Libraries](#technical-stack--linked-libraries)
4. [Real-World Benchmark (VRecorder vs. OBS Studio)](#real-world-benchmark-vrecorder-vs-obs-studio)
5. [Video & Hardware Encoding Pipeline](#video--hardware-encoding-pipeline)
6. [Color Correction & Live Filter Engine](#color-correction--live-filter-engine)
7. [WASAPI Dual-Channel Audio Studio](#wasapi-dual-channel-audio-studio)
8. [Scene Components & Interactive Transform](#scene-components--interactive-transform)
9. [Keyboard Shortcuts](#keyboard-shortcuts)
10. [Building from Source](#building-from-source)
11. [Repository Structure](#repository-structure)
12. [License & Support](#license--support)

---

## What's New in Beta

- **Pure Native Direct2D + DirectWrite Studio UI:** Complete hardware-accelerated studio interface with crisp per-monitor DPI scaling, smooth 60 FPS preview rendering, and modular 3-dock studio layout.
- **Hardware-Accelerated Encoding (NVENC, QSV, AMF):** Direct GPU silicon encoding via Windows Media Foundation (`ICodecAPI`) supporting H.264 / HEVC with adjustable presets (`P1` to `P7`) and selectable bitrates (`8M`, `18M`, `28M`, `45M`).
- **Real-Time Color Correction Engine:** In-place GPU NV12 hardware buffer adjustments for Saturation, Gamma, Contrast, and Brightness without expensive color space conversions.
- **DXGI Desktop Duplication & Window Capture:** Low-latency multi-monitor screen capture, secondary display support, and standalone window recording.
- **Low-Latency WASAPI Audio Capture:** Dual-channel loopback for system audio and microphone with peak-hold VU meters and high-bitrate AAC audio encoding.
- **Zero-Redist Portable Binary (`/MT` Static CRT):** Compiles into a single standalone 879 KB executable with zero runtime dependencies.

---

## Architecture Overview

VRecorder separates native Windows desktop capture and hardware video encoding from its UI layer so rendering and capture never block user interaction:

```text
+-----------------------------------------------------------------------+
|                         wWinMain (src/main.cpp)                       |
|  - Enforces single instance via Local\VRecorder_SingleInstance_Mutex  |
|  - High-precision multimedia timer (timeBeginPeriod(1))               |
|  - Initializes COM STA cleanly & catches unhandled startup errors     |
+-----------------------------------+-----------------------------------+
                                    |
          +-------------------------+-------------------------+
          v                                                   v
+-----------------------------+               +-----------------------------+
|  AppWindow (Win32 + D2D)    |               |  Capture & Encoding Core    |
| - 3-Dock Studio Workspace   |               | - DXGI Desktop Duplication  |
| - Pure Direct2D / DWrite    |  Frame Pipe   | - IMFSourceReader (Webcam)  |
| - 60 FPS Hardware Preview   | <-----------> | - Media Foundation NVENC/QSV|
| - Interactive Transform Box |               | - WASAPI Audio Engine       |
| - Live Color Correction UI  |               | - AAC 192k Audio Encoder    |
+--------------+--------------+               +-----------------------------+
               |
    +----------+--------------------+--------------------+
    v                               v                    v
+--------------------+  +------------------------+  +-------------------+
|  TrayIconRenderer  |  |  ColorCorrection       |  |  ConfigManager    |
| - Direct2D/DWrite  |  | - Direct NV12 Shading  |  | - Native JSON/XML |
| - REC state badge  |  | - Sat/Gamma/Contr/Brt  |  | - %APPDATA%\VRec  |
| - Tray menu & hide |  | - In-place GPU pass    |  | - Global Hotkeys  |
+--------------------+  +------------------------+  +-------------------+
```

---

## Technical Stack & Linked Libraries

Every component in VRecorder relies directly on native Windows APIs with zero external runtime DLL requirements:

| Component | Implementation | Purpose |
| :--- | :--- | :--- |
| **Language & Runtime** | **C++20** (`/W4 /EHsc /permissive- /utf-8`, `/MT` static CRT) | Self-contained x64 binary (`CMakeLists.txt`) |
| **Window Host & DPI** | **Win32 (`HWND`)** + `PerMonitorV2` manifest (`resources/app.manifest`) | Frameless windowing, custom titlebar, responsive docking, sharp DPI scaling |
| **UI Rendering** | **Direct2D (`d2d1`)**, **DirectWrite (`dwrite`)**, **WIC** | Hardware-accelerated 60 FPS studio UI, vector paths, font glyphs |
| **Desktop Capture** | **DXGI Desktop Duplication (`dxgi1_2`)**, **D3D11 (`d3d11`)** | Direct zero-copy VRAM surface capture across multi-monitor setups |
| **Hardware Video Encoding** | **Windows Media Foundation (`mfplat`, `mfreadwrite`, `mfuuid`)** | Direct GPU silicon encoding (NVIDIA NVENC, Intel QSV, AMD AMF) |
| **Color Correction Engine** | In-place GPU NV12 hardware buffer transform | Saturation, Gamma, Contrast, and Brightness adjustments without black-screen artifacts |
| **Audio Capture & Mixing** | **WASAPI (`mmdevapi`)**, **Avrt** | Dual-channel loopback (Speakers + Microphone) with pro studio peak-hold VU meters |
| **Audio Encoding** | **Media Foundation AAC Encoder** | 192 kbps and 320 kbps broadcast audio encoding |
| **System Integration** | `advapi32`, `shell32`, `dwmapi`, `winmm` | System tray integration, global hotkeys (`F9`/`F10`), 1 ms precision timer |

---

## Real-World Benchmark (VRecorder vs. OBS Studio)

Tested side-by-side on an identical system (*Intel Core CPU + NVIDIA GeForce RTX 3050 Laptop GPU, Windows 11*) recording a 1920x1080 display @ 60 FPS with NVENC H.264 at 18 Mbps:

| Performance Metric | VRecorder Beta | OBS Studio 64-bit | Advantage / Difference |
| :--- | :---: | :---: | :---: |
| **Disk Installation Footprint** | **879 KB** (Single `.exe`) | **548.7 MB** (1,120+ files) | **VRecorder 623x Smaller** |
| **Cold Startup Latency** | **1.27 seconds** | **2.51 seconds** | **VRecorder 2x Faster** |
| **Idle RAM (Working Set)** | **66.4 MB** | **175.1 MB** | **62% Lighter Memory** (-108 MB) |
| **Idle Committed Memory** | **67.7 MB** | **203.4 MB** | **66% Lighter Virtual RAM** |
| **Idle CPU Overhead** | **0.18%** | **1.35%** | **Virtually 0% CPU Idle** |
| **Active Recording RAM (Working Set)** | **138.6 MB** | **330.9 MB** | **58% Lighter During Record** (-192 MB) |
| **Active Recording Committed Memory** | **240.6 MB** | **507.9 MB** | **52% Lighter Pagefile Memory** |
| **Active Recording CPU Usage** | **0.50%** | **1.38%** | **2.7x Lower CPU Usage** |
| **Kernel Handle Count** | **1,097 handles** | **2,089 handles** | **Clean OS footprint** |
| **Runtime Architecture** | **Native C++20 / Direct2D** | **Qt6 + Chromium CEF + Python** | Zero bloat, zero background daemons |

---

## Video & Hardware Encoding Pipeline

VRecorder queries hardware media encoders at startup through Windows Media Foundation `MFTEnumEx`:

- **NVIDIA NVENC:** H.264 / HEVC hardware encoder with quality presets (`P1 Fastest` through `P7 Quality`, 2-Pass VBR/CBR).
- **Intel Quick Sync Video (QSV):** Hardware-accelerated encoding for Intel Iris Xe / Arc graphics.
- **AMD AMF:** Hardware VCE/VCN encoder for Radeon graphics.
- **Microsoft Software MFT:** Fallback CPU H.264 encoder for systems without dedicated hardware encoders.
- **Bitrate Options:** Segmented selector for `8M`, `18M`, `28M`, and `45M` Mbps.
- **Framerate & Resolution:** Support for 30 FPS, 60 FPS, and up to 120 FPS high-refresh recording.

---

## Color Correction & Live Filter Engine

The color correction engine processes video buffers in real time directly on GPU NV12 hardware buffers:

- **Saturation:** -100% (Monochrome/B&W) to +200% (Vibrant Color).
- **Gamma:** -1.00 to +1.00 midtone curve adjustment.
- **Contrast:** -100% to +100% contrast expansion.
- **Brightness:** -100% to +100% luminance offset.
- **One-Click Reset:** Quickly revert all filters back to default.
- **Zero-Lag Slider Tracking:** 1:1 mouse knob tracking with responsive tactile feedback.

---

## WASAPI Dual-Channel Audio Studio

- **Dual-Channel Loopback:** Simultaneous capture of Desktop Output (Speakers) and Microphone Input.
- **Pro Studio VU Meters:** Recessed meter bars with segmented tick marks and floating peak-hold indicators.
- **Independent Controls:** Volume sliders (0% - 100%), instant mute toggles, and device dropdown pickers.
- **High-Bitrate AAC:** 192 kbps and 320 kbps broadcast audio encoding.

---

## Scene Components & Interactive Transform

- **Display & Window Capture:** DXGI Desktop Duplication with instant multi-monitor switching, application window filtering, and custom crop box (`X, Y, W, H`).
- **Interactive Canvas Transform:** Transform Display / Window capture live directly on preview with `Fit`, `Fill`, `Center`, `75%`, `Reset`, and 4-corner draggable bounding box with live coordinate feedback.
- **Webcam (On-Cam) Overlay:** Native `IMFSourceReader` integration with live canvas dragging, shape masking (`Rectangle`, `Rounded Box`, `Circle`), selfie mirror toggle, red studio border, and 4-corner quick snap (`TL`, `TR`, `BL`, `BR`).
- **Text Overlay:** Dynamic text rendering with customizable font size, colors, and opacity.
- **Live System Clock:** Real-time digital timestamp overlay.
- **Image / Watermark:** Overlay PNG/ICO watermarks with alpha transparency.
- **Studio Frame Box:** Accent framing for game streams or facecam borders.

---

## Keyboard Shortcuts

| Shortcut | Action |
| :--- | :--- |
| **`F9`** | Toggle Start / Stop Recording |
| **`F10`** | Toggle Pause / Resume Recording |
| **`Space`** (when focused on Preview) | Quick Pause / Resume |
| **`Esc`** | Close open dropdown menus or modal popups |

---

## Building from Source

### Prerequisites
- Windows 10 (Version 1809 or higher) or Windows 11 (x64)
- CMake 3.20 or newer
- Visual Studio 2019 / 2022 / 2026 with C++ Desktop Development (MSVC v142 / v143 toolset)
- Windows 10 / 11 SDK (10.0.19041.0 or newer)

### Automated Build

```bat
build.bat
```

### Manual Build via CMake

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Output executable: `build\Release\VRecorder.exe` (statically linked `/MT`, ~879 KB).

---

## Repository Structure

```text
VRecorder/
├── assets/
│   ├── vrecorder.ico                 # Multi-resolution Windows application icon (16..256 px)
│   └── vrecorder.png                 # Studio brand emblem
├── resources/
│   ├── app.manifest                  # PerMonitorV2 DPI & Windows 10/11 compatibility manifest
│   ├── resource.h                    # Win32 resource IDs
│   └── VRecorder.rc                  # Embedded resources & PE metadata
├── src/
│   ├── main.cpp                      # wWinMain, single-instance mutex & precision timer initialization
│   ├── AppWindow.hpp / .cpp          # Win32 host window, Direct2D/DWrite studio UI & preview loop
│   ├── CaptureEngine.hpp / .cpp      # DXGI Desktop Duplication & multi-monitor capture engine
│   ├── CameraEngine.hpp / .cpp       # IMFSourceReader webcam capture & transform overlay
│   ├── AudioEngine.hpp / .cpp        # WASAPI loopback capture & microphone mixer with peak VU meters
│   ├── EncoderManager.hpp / .cpp     # Media Foundation NVENC / QSV / AMF hardware video encoder
│   ├── RecorderEngine.hpp / .cpp     # Master recording pipeline coordinator & MP4 sink writer
│   ├── ColorCorrection.hpp           # Hardware NV12 real-time color correction filter engine
│   ├── BicubicShaders.hpp            # GPU preview scaling & bicubic sampling
│   ├── OverlayCompositor.hpp / .cpp  # Scene component compositor & draggable bounding box
│   ├── ConfigManager.hpp / .cpp      # Configuration I/O & HKCU startup entry
│   ├── TrayIconRenderer.hpp / .cpp   # Direct2D/DWrite tray notification renderer
│   ├── VectorIcons.hpp / .cpp        # Vector UI geometry renderer
│   └── Win32Compat.hpp               # Dynamic DPI helpers & shared WIC PNG decoder
├── .gitignore
├── build.bat                         # Automated CMake + MSVC Release x64 builder
├── CMakeLists.txt                    # C++20 + /MT static runtime CMake build configuration
├── VRecorder.exe                     # Compiled standalone portable binary (879 KB)
└── README.md
```

---

## License & Support

- **Project:** **Vortex Open Source Project** — [`github.com/vortexprojectsource`](https://github.com/vortexprojectsource)
- **Author:** **`@vinnotdkk`**
- **Support Development:** https://sociabuzz.com/idnvinnmalay
