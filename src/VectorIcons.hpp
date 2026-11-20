#pragma once

#include <d2d1.h>

namespace vrec::icons {

enum class IconKind {
    VRecorderLogo,
    Minimize,
    Maximize,
    RestoreWindow,
    Close,
    ChevronDown,
    ChevronLeft,
    SettingsGear,
    TrayMinimize,
    Heart,
    Monitor,
    AppWindow,
    CropRegion,
    Speaker,
    SpeakerMuted,
    Microphone,
    MicMuted,
    RecordDot,
    StopSquare,
    Pause,
    Play,
    FolderOpen,
    Refresh,
    Eye,
    EyeOff,
    Sun,
    Moon,
    TextType,
    Clock,
    BoxShape,
    ImagePic,
    Plus,
    Trash,
    Camera,
    CameraOff,
    FitScreen,
    Layers
};

void DrawIcon(ID2D1RenderTarget* rt,
              ID2D1Factory* factory,
              ID2D1SolidColorBrush* brush,
              ID2D1StrokeStyle* roundStroke,
              IconKind kind,
              const D2D1_RECT_F& bounds,
              float strokeWidth = 1.8f,
              float rotationDeg = 0.0f);

}
