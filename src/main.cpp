#include "AppWindow.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstring>
#include <exception>

namespace {

bool HasCommandLineFlag(const wchar_t* cmdLine, const wchar_t* flag) {
    if (!cmdLine || !flag) return false;
    return wcsstr(cmdLine, flag) != nullptr;
}

}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    SetCurrentProcessExplicitAppUserModelID(L"Vortex.VRecorder.Studio.2");
    const bool elevatedRestart = HasCommandLineFlag(lpCmdLine, L"--elevated-restart");
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\VRecorder_V1_SingleInstance_Mutex");
    if (!elevatedRestart && hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"VRecorder_V1_HostWindow", nullptr)) {
            PostMessageW(existing, vrec::WM_APP_SHOW_WINDOW, 0, 0);
        }
        CloseHandle(hMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const HRESULT mfHr  = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    const bool startMinimized = HasCommandLineFlag(lpCmdLine, L"--minimized");

    int exitCode = 0;
    try {
        vrec::AppWindow app(hInstance, startMinimized);
        if (!app.Create()) {
            MessageBoxW(nullptr,
                        L"Failed to initialize VRecorder UI (Direct2D/DirectWrite required).",
                        L"VRecorder",
                        MB_OK | MB_ICONERROR);
            if (SUCCEEDED(mfHr)) MFShutdown();
            if (SUCCEEDED(comHr)) CoUninitialize();
            if (hMutex) {
                ReleaseMutex(hMutex);
                CloseHandle(hMutex);
            }
            return 1;
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (app.PreTranslateMessage(&msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = static_cast<int>(msg.wParam);
    } catch (const std::exception&) {
        MessageBoxW(nullptr,
                    L"Unexpected runtime error in VRecorder.",
                    L"VRecorder - Error",
                    MB_OK | MB_ICONERROR);
        exitCode = 1;
    } catch (...) {
        MessageBoxW(nullptr,
                    L"Unexpected error in VRecorder.",
                    L"VRecorder - Error",
                    MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(mfHr)) MFShutdown();
    if (SUCCEEDED(comHr)) CoUninitialize();
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return exitCode;
}
