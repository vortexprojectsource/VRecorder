#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vrec {

class TrayIconRenderer {
public:
    TrayIconRenderer();
    ~TrayIconRenderer();

    HICON CreateStudioTrayIcon(HINSTANCE hInst, bool isRecording, bool isPaused, UINT dpi);
};

}
