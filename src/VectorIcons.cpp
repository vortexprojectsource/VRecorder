#include "VectorIcons.hpp"

#include <wrl/client.h>

#include <cmath>

using Microsoft::WRL::ComPtr;

namespace vrec::icons {

namespace {

constexpr float kPi = 3.14159265358979323846f;

void DrawPolylineIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1SolidColorBrush* brush,
                      ID2D1StrokeStyle* rs, float sw, const D2D1_POINT_2F* pts, size_t count, bool closed = false) {
    ComPtr<ID2D1PathGeometry> p;
    if (FAILED(factory->CreatePathGeometry(&p))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(p->Open(&sink))) return;
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts + 1, static_cast<UINT32>(count - 1));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(p.Get(), brush, sw, rs);
}

ComPtr<ID2D1PathGeometry> BuildGearOuterPath(ID2D1Factory* factory) {
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return nullptr;

    constexpr int kTeeth = 8;
    constexpr float cx = 12.0f, cy = 12.0f, rInner = 6.9f, rOuter = 9.6f;
    D2D1_POINT_2F pts[kTeeth * 4]{};
    const float sector = (2.0f * kPi) / static_cast<float>(kTeeth);
    for (int i = 0; i < kTeeth; ++i) {
        const float a0 = sector * static_cast<float>(i);
        const float angles[4] = {a0 - sector * 0.24f, a0 - sector * 0.14f, a0 + sector * 0.14f, a0 + sector * 0.24f};
        const float radii[4] = {rInner, rOuter, rOuter, rInner};
        for (int k = 0; k < 4; ++k) {
            pts[i * 4 + k] = D2D1::Point2F(cx + radii[k] * std::cos(angles[k]), cy + radii[k] * std::sin(angles[k]));
        }
    }
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts + 1, static_cast<UINT32>(kTeeth * 4 - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

ComPtr<ID2D1PathGeometry> BuildHeartPath(ID2D1Factory* factory) {
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return nullptr;

    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure({12.0f, 20.6f}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment({6.5f, 16.9f}, {2.5f, 13.0f}, {2.5f, 8.6f}));
    sink->AddBezier(D2D1::BezierSegment({2.5f, 5.4f}, {5.0f, 3.6f}, {7.6f, 3.6f}));
    sink->AddBezier(D2D1::BezierSegment({9.5f, 3.6f}, {11.1f, 4.7f}, {12.0f, 6.2f}));
    sink->AddBezier(D2D1::BezierSegment({12.9f, 4.7f}, {14.5f, 3.6f}, {16.4f, 3.6f}));
    sink->AddBezier(D2D1::BezierSegment({19.0f, 3.6f}, {21.5f, 5.4f}, {21.5f, 8.6f}));
    sink->AddBezier(D2D1::BezierSegment({21.5f, 13.0f}, {17.5f, 16.9f}, {12.0f, 20.6f}));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

}

void DrawIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1SolidColorBrush* brush,
              ID2D1StrokeStyle* roundStroke, IconKind kind, const D2D1_RECT_F& bounds,
              float strokeWidth, float rotationDeg) {
    if (!rt || !factory || !brush) return;
    const float w = bounds.right - bounds.left;
    const float h = bounds.bottom - bounds.top;
    if (w <= 0.0f || h <= 0.0f) return;

    const float side = (w < h) ? w : h;
    const float scale = side / 24.0f;
    const float tx = bounds.left + (w - side) * 0.5f;
    const float ty = bounds.top + (h - side) * 0.5f;

    D2D1_MATRIX_3X2_F oldXform{};
    rt->GetTransform(&oldXform);

    D2D1::Matrix3x2F mat = D2D1::Matrix3x2F::Identity();
    if (std::fabs(rotationDeg) > 0.01f) {
        mat = D2D1::Matrix3x2F::Rotation(rotationDeg, D2D1::Point2F(12.0f, 12.0f));
    }
    mat = mat * D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(tx, ty);
    rt->SetTransform(mat * (*D2D1::Matrix3x2F::ReinterpretBaseType(&oldXform)));

    const float sw = (scale > 0.01f) ? (strokeWidth / scale) : strokeWidth;

    switch (kind) {
    case IconKind::VRecorderLogo:

        break;

    case IconKind::Minimize:
        rt->DrawLine({5.0f, 12.0f}, {19.0f, 12.0f}, brush, sw, roundStroke);
        break;

    case IconKind::Maximize:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(5.5f, 5.5f, 18.5f, 18.5f), 1.5f, 1.5f),
                                 brush, sw, roundStroke);
        break;

    case IconKind::RestoreWindow: {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(5.0f, 8.0f, 16.0f, 19.0f), 1.2f, 1.2f),
                                 brush, sw, roundStroke);
        const D2D1_POINT_2F back[3] = {{8.5f, 8.0f}, {8.5f, 5.0f}, {19.0f, 5.0f}};
        const D2D1_POINT_2F side2[2] = {{19.0f, 5.0f}, {19.0f, 15.5f}};
        const D2D1_POINT_2F bot[2] = {{19.0f, 15.5f}, {16.0f, 15.5f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, back, 3);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, side2, 2);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, bot, 2);
        break;
    }

    case IconKind::Close:
        rt->DrawLine({6.0f, 6.0f}, {18.0f, 18.0f}, brush, sw, roundStroke);
        rt->DrawLine({18.0f, 6.0f}, {6.0f, 18.0f}, brush, sw, roundStroke);
        break;

    case IconKind::ChevronDown: {
        const D2D1_POINT_2F pts[3] = {{6.0f, 9.0f}, {12.0f, 15.0f}, {18.0f, 9.0f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, pts, 3);
        break;
    }

    case IconKind::ChevronLeft: {
        const D2D1_POINT_2F pts[3] = {{15.0f, 6.0f}, {9.0f, 12.0f}, {15.0f, 18.0f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, pts, 3);
        break;
    }

    case IconKind::SettingsGear:
        if (auto p = BuildGearOuterPath(factory)) rt->DrawGeometry(p.Get(), brush, sw, roundStroke);
        rt->DrawEllipse(D2D1::Ellipse({12.0f, 12.0f}, 3.1f, 3.1f), brush, sw, roundStroke);
        break;

    case IconKind::TrayMinimize: {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3.0f, 4.0f, 21.0f, 20.0f), 2.0f, 2.0f),
                                 brush, sw, roundStroke);
        rt->DrawLine({3.0f, 15.0f}, {21.0f, 15.0f}, brush, sw, roundStroke);
        const D2D1_POINT_2F pts[3] = {{9.5f, 8.8f}, {12.0f, 11.6f}, {14.5f, 8.8f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, pts, 3);
        break;
    }

    case IconKind::Heart:
        if (auto p = BuildHeartPath(factory)) rt->FillGeometry(p.Get(), brush);
        break;

    case IconKind::Monitor:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2.5f, 3.5f, 21.5f, 16.5f), 2.0f, 2.0f),
                                 brush, sw, roundStroke);
        rt->DrawLine({8.0f, 20.5f}, {16.0f, 20.5f}, brush, sw, roundStroke);
        rt->DrawLine({12.0f, 16.5f}, {12.0f, 20.5f}, brush, sw, roundStroke);
        break;

    case IconKind::AppWindow:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3.0f, 4.0f, 21.0f, 20.0f), 2.2f, 2.2f),
                                 brush, sw, roundStroke);
        rt->DrawLine({3.0f, 9.0f}, {21.0f, 9.0f}, brush, sw, roundStroke);
        break;

    case IconKind::CropRegion: {
        const D2D1_POINT_2F p1[3] = {{6.5f, 2.5f}, {6.5f, 17.5f}, {21.5f, 17.5f}};
        const D2D1_POINT_2F p2[3] = {{2.5f, 6.5f}, {17.5f, 6.5f}, {17.5f, 21.5f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, p1, 3);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, p2, 3);
        break;
    }

    case IconKind::Speaker:
    case IconKind::SpeakerMuted: {
        const D2D1_POINT_2F spk[6] = {{3.5f, 9.5f}, {7.5f, 9.5f}, {12.5f, 5.0f}, {12.5f, 19.0f}, {7.5f, 14.5f}, {3.5f, 14.5f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, spk, 6, true);
        if (kind == IconKind::Speaker) {
            const D2D1_POINT_2F arc1[3] = {{15.5f, 8.5f}, {17.0f, 12.0f}, {15.5f, 15.5f}};
            const D2D1_POINT_2F arc2[3] = {{18.3f, 6.2f}, {20.4f, 12.0f}, {18.3f, 17.8f}};
            DrawPolylineIcon(rt, factory, brush, roundStroke, sw, arc1, 3);
            DrawPolylineIcon(rt, factory, brush, roundStroke, sw, arc2, 3);
        } else {
            rt->DrawLine({16.0f, 9.0f}, {21.0f, 15.0f}, brush, sw, roundStroke);
            rt->DrawLine({21.0f, 9.0f}, {16.0f, 15.0f}, brush, sw, roundStroke);
        }
        break;
    }

    case IconKind::Microphone:
    case IconKind::MicMuted: {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(9.0f, 3.0f, 15.0f, 13.0f), 3.0f, 3.0f), brush, sw, roundStroke);
        const D2D1_POINT_2F cup[5] = {{5.5f, 11.0f}, {6.2f, 14.8f}, {12.0f, 17.0f}, {17.8f, 14.8f}, {18.5f, 11.0f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, cup, 5);
        rt->DrawLine({12.0f, 17.0f}, {12.0f, 21.0f}, brush, sw, roundStroke);
        rt->DrawLine({8.5f, 21.0f}, {15.5f, 21.0f}, brush, sw, roundStroke);
        if (kind == IconKind::MicMuted) rt->DrawLine({4.0f, 4.0f}, {20.0f, 20.0f}, brush, sw * 1.05f, roundStroke);
        break;
    }

    case IconKind::RecordDot:
        rt->FillEllipse(D2D1::Ellipse({12.0f, 12.0f}, 6.5f, 6.5f), brush);
        break;

    case IconKind::StopSquare:
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(6.2f, 6.2f, 17.8f, 17.8f), 2.0f, 2.0f), brush);
        break;

    case IconKind::Pause:
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(6.5f, 5.5f, 10.2f, 18.5f), 1.2f, 1.2f), brush);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(13.8f, 5.5f, 17.5f, 18.5f), 1.2f, 1.2f), brush);
        break;

    case IconKind::Play: {
        ComPtr<ID2D1PathGeometry> p;
        if (SUCCEEDED(factory->CreatePathGeometry(&p))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(p->Open(&sink))) {
                sink->BeginFigure({7.2f, 5.2f}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine({19.2f, 12.0f}); sink->AddLine({7.2f, 18.8f});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
                rt->FillGeometry(p.Get(), brush);
            }
        }
        break;
    }

    case IconKind::FolderOpen: {
        const D2D1_POINT_2F fld[7] = {
            {3.0f, 6.5f}, {3.0f, 19.0f}, {21.0f, 19.0f}, {21.0f, 8.5f}, {11.5f, 8.5f}, {9.5f, 6.0f}, {3.0f, 6.0f}
        };
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, fld, 7, true);
        break;
    }

    case IconKind::Refresh: {
        const D2D1_POINT_2F arc[6] = {{4.5f, 11.5f}, {6.2f, 6.2f}, {12.0f, 4.5f}, {17.8f, 6.2f}, {19.5f, 11.5f}, {17.0f, 17.5f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, arc, 6);
        const D2D1_POINT_2F tip[3] = {{16.5f, 8.0f}, {19.5f, 11.5f}, {21.5f, 7.8f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, tip, 3);
        break;
    }

    case IconKind::Eye:
    case IconKind::EyeOff: {
        const D2D1_POINT_2F top[5] = {{2.5f, 12.0f}, {6.5f, 6.5f}, {12.0f, 5.5f}, {17.5f, 6.5f}, {21.5f, 12.0f}};
        const D2D1_POINT_2F bot[5] = {{2.5f, 12.0f}, {6.5f, 17.5f}, {12.0f, 18.5f}, {17.5f, 17.5f}, {21.5f, 12.0f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, top, 5);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, bot, 5);
        rt->DrawEllipse(D2D1::Ellipse({12.0f, 12.0f}, 2.8f, 2.8f), brush, sw, roundStroke);
        if (kind == IconKind::EyeOff) rt->DrawLine({4.0f, 4.0f}, {20.0f, 20.0f}, brush, sw * 1.05f, roundStroke);
        break;
    }

    case IconKind::Sun: {
        rt->DrawEllipse(D2D1::Ellipse({12.0f, 12.0f}, 4.2f, 4.2f), brush, sw, roundStroke);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * (kPi / 4.0f);
            rt->DrawLine({12.0f + 7.2f * std::cos(a), 12.0f + 7.2f * std::sin(a)},
                         {12.0f + 9.4f * std::cos(a), 12.0f + 9.4f * std::sin(a)}, brush, sw, roundStroke);
        }
        break;
    }

    case IconKind::Moon: {
        ComPtr<ID2D1PathGeometry> p;
        if (SUCCEEDED(factory->CreatePathGeometry(&p))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(p->Open(&sink))) {
                sink->BeginFigure({19.2f, 14.5f}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddArc(D2D1::ArcSegment({10.5f, 4.8f}, {8.2f, 8.2f}, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                sink->AddArc(D2D1::ArcSegment({19.2f, 14.5f}, {6.2f, 6.2f}, 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
                rt->FillGeometry(p.Get(), brush);
            }
        }
        break;
    }

    case IconKind::TextType:
        rt->DrawLine({4.5f, 5.5f}, {19.5f, 5.5f}, brush, sw, roundStroke);
        rt->DrawLine({12.0f, 5.5f}, {12.0f, 19.0f}, brush, sw, roundStroke);
        rt->DrawLine({8.5f, 19.0f}, {15.5f, 19.0f}, brush, sw, roundStroke);
        break;

    case IconKind::Clock:
        rt->DrawEllipse(D2D1::Ellipse({12.0f, 12.0f}, 9.0f, 9.0f), brush, sw, roundStroke);
        rt->DrawLine({12.0f, 6.5f}, {12.0f, 12.0f}, brush, sw, roundStroke);
        rt->DrawLine({12.0f, 12.0f}, {16.0f, 14.2f}, brush, sw, roundStroke);
        break;

    case IconKind::BoxShape:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(4.0f, 4.0f, 20.0f, 20.0f), 3.0f, 3.0f),
                                 brush, sw, roundStroke);
        break;

    case IconKind::ImagePic: {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3.5f, 4.5f, 20.5f, 19.5f), 2.5f, 2.5f),
                                 brush, sw, roundStroke);
        rt->FillEllipse(D2D1::Ellipse({8.5f, 9.5f}, 1.5f, 1.5f), brush);
        const D2D1_POINT_2F mt[4] = {{4.5f, 18.0f}, {10.5f, 12.5f}, {14.5f, 16.0f}, {19.5f, 11.5f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, mt, 4);
        break;
    }

    case IconKind::Plus:
        rt->DrawLine({5.0f, 12.0f}, {19.0f, 12.0f}, brush, sw, roundStroke);
        rt->DrawLine({12.0f, 5.0f}, {12.0f, 19.0f}, brush, sw, roundStroke);
        break;

    case IconKind::Trash: {
        rt->DrawLine({4.5f, 6.5f}, {19.5f, 6.5f}, brush, sw, roundStroke);
        rt->DrawLine({9.0f, 4.0f}, {15.0f, 4.0f}, brush, sw, roundStroke);
        const D2D1_POINT_2F bin[4] = {{6.5f, 6.5f}, {7.5f, 20.0f}, {16.5f, 20.0f}, {17.5f, 6.5f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, bin, 4);
        break;
    }

    case IconKind::Camera:
    case IconKind::CameraOff: {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2.5f, 6.5f, 15.8f, 17.5f), 2.5f, 2.5f),
                                 brush, sw, roundStroke);
        const D2D1_POINT_2F lens[4] = {{15.8f, 10.2f}, {21.2f, 7.2f}, {21.2f, 16.8f}, {15.8f, 13.8f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, lens, 4, true);
        if (kind == IconKind::CameraOff) {
            rt->DrawLine({3.5f, 4.5f}, {20.5f, 19.5f}, brush, sw * 1.05f, roundStroke);
        }
        break;
    }

    case IconKind::FitScreen: {
        const D2D1_POINT_2F tl[3] = {{4.0f, 9.0f}, {4.0f, 4.0f}, {9.0f, 4.0f}};
        const D2D1_POINT_2F tr[3] = {{15.0f, 4.0f}, {20.0f, 4.0f}, {20.0f, 9.0f}};
        const D2D1_POINT_2F bl[3] = {{4.0f, 15.0f}, {4.0f, 20.0f}, {9.0f, 20.0f}};
        const D2D1_POINT_2F br[3] = {{15.0f, 20.0f}, {20.0f, 20.0f}, {20.0f, 15.0f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, tl, 3);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, tr, 3);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, bl, 3);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, br, 3);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8.5f, 8.5f, 15.5f, 15.5f), 1.0f, 1.0f), brush, sw * 0.9f, roundStroke);
        break;
    }

    case IconKind::Layers: {
        const D2D1_POINT_2F top[5] = {{12.0f, 3.5f}, {21.0f, 8.2f}, {12.0f, 13.0f}, {3.0f, 8.2f}, {12.0f, 3.5f}};
        const D2D1_POINT_2F mid[3] = {{3.0f, 12.2f}, {12.0f, 17.0f}, {21.0f, 12.2f}};
        const D2D1_POINT_2F bot[3] = {{3.0f, 16.2f}, {12.0f, 21.0f}, {21.0f, 16.2f}};
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, top, 5, true);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, mid, 3);
        DrawPolylineIcon(rt, factory, brush, roundStroke, sw, bot, 3);
        break;
    }
    }

    rt->SetTransform(oldXform);
}

}
