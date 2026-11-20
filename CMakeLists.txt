cmake_minimum_required(VERSION 3.20)
project(VRecorder VERSION 2.2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC C/C++ runtime (/MT) so VRecorder.exe runs portable without VC++ Redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(VRecorder WIN32
    src/main.cpp
    src/AppWindow.cpp
    src/AppWindow.hpp
    src/CameraEngine.cpp
    src/CameraEngine.hpp
    src/CaptureEngine.cpp
    src/CaptureEngine.hpp
    src/AudioEngine.cpp
    src/AudioEngine.hpp
    src/BicubicShaders.hpp
    src/EncoderManager.cpp
    src/EncoderManager.hpp
    src/OverlayCompositor.cpp
    src/OverlayCompositor.hpp
    src/RecorderEngine.cpp
    src/RecorderEngine.hpp
    src/ConfigManager.cpp
    src/ConfigManager.hpp
    src/ColorCorrection.hpp
    src/TrayIconRenderer.cpp
    src/TrayIconRenderer.hpp
    src/VectorIcons.cpp
    src/VectorIcons.hpp
    src/Win32Compat.hpp
    resources/VRecorder.rc
    resources/app.manifest
)

set_source_files_properties(resources/VRecorder.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/vrecorder.ico;${CMAKE_CURRENT_SOURCE_DIR}/assets/vrecorder.png"
)

target_include_directories(VRecorder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

target_compile_definitions(VRecorder PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(VRecorder PRIVATE
        /W4
        /EHsc
        /permissive-
        /utf-8
        /Zc:__cplusplus
    )
    target_link_options(VRecorder PRIVATE
        /SUBSYSTEM:WINDOWS
    )
endif()

target_link_libraries(VRecorder PRIVATE
    user32
    gdi32
    shell32
    advapi32
    psapi
    dwmapi
    comctl32
    comdlg32
    shlwapi
    xmllite
    d2d1
    dwrite
    d3d11
    dxgi
    mf
    mfplat
    mfreadwrite
    mfuuid
    windowscodecs
    ole32
    oleaut32
    propsys
    winmm
)
