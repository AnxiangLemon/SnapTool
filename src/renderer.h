#pragma once

#include "common.h"

namespace snap {

// ============================================================
// 标注渲染（同时支持全屏实时预览 offset=(0,0) 与裁剪导出 offset=(sel.left, sel.top)）
// ============================================================
inline void DrawMosaicBlock(Gdiplus::Graphics& g, const ScreenCaptureData& screen,
                            int bx, int by, int blockSize, int ox, int oy) {
    int x0 = bx * blockSize;
    int y0 = by * blockSize;
    int x1 = (std::min)(x0 + blockSize, screen.vw);
    int y1 = (std::min)(y0 + blockSize, screen.vh);
    if (x0 >= screen.vw || y0 >= screen.vh || x1 <= x0 || y1 <= y0) return;

    uint64_t sumR = 0, sumG = 0, sumB = 0, cnt = 0;
    for (int y = y0; y < y1; ++y) {
        const uint32_t* row = &screen.pixels[static_cast<size_t>(y) * screen.vw];
        for (int x = x0; x < x1; ++x) {
            uint32_t px = row[x];
            sumB += (px & 0xFF);
            sumG += ((px >> 8) & 0xFF);
            sumR += ((px >> 16) & 0xFF);
            ++cnt;
        }
    }
    if (cnt == 0) return;
    Gdiplus::SolidBrush br(Gdiplus::Color(255,
                                          static_cast<BYTE>(sumR / cnt),
                                          static_cast<BYTE>(sumG / cnt),
                                          static_cast<BYTE>(sumB / cnt)));
    g.FillRectangle(&br, x0 - ox, y0 - oy, x1 - x0, y1 - y0);
}

inline void DrawAnnotation(Gdiplus::Graphics& g, const Annotation& a,
                           const ScreenCaptureData& screen, int ox, int oy) {
    switch (a.type) {
        case ToolType::Rect: {
            RECT r = NormalizeRect(a.start, a.end);
            if (RectWidth(r) < 1 && RectHeight(r) < 1) break;
            Gdiplus::Pen pen(ToGdipColor(a.color), static_cast<Gdiplus::REAL>(a.strokeWidth));
            pen.SetLineJoin(Gdiplus::LineJoinMiter);
            g.DrawRectangle(&pen, r.left - ox, r.top - oy, RectWidth(r), RectHeight(r));
            break;
        }
        case ToolType::Ellipse: {
            RECT r = NormalizeRect(a.start, a.end);
            if (RectWidth(r) < 1 && RectHeight(r) < 1) break;
            Gdiplus::Pen pen(ToGdipColor(a.color), static_cast<Gdiplus::REAL>(a.strokeWidth));
            g.DrawEllipse(&pen, r.left - ox, r.top - oy, RectWidth(r), RectHeight(r));
            break;
        }
        case ToolType::Arrow: {
            float x1 = static_cast<float>(a.start.x - ox);
            float y1 = static_cast<float>(a.start.y - oy);
            float x2 = static_cast<float>(a.end.x - ox);
            float y2 = static_cast<float>(a.end.y - oy);
            float dx = x2 - x1, dy = y2 - y1;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 3.0f) break;

            float ux = dx / len, uy = dy / len;
            float px = -uy, py = ux;

            float headLen = (std::min)(len * 0.55f, static_cast<float>(a.strokeWidth) * 3.8f + 9.0f);
            float headHalfW = headLen * 0.44f;
            float tailHalfW = (std::max)(1.0f, static_cast<float>(a.strokeWidth) * 0.28f);
            float neckHalfW = static_cast<float>(a.strokeWidth) * 0.65f + 1.2f;
            float neckDist = len - headLen * 0.78f;

            Gdiplus::PointF pts[7] = {
                { x1 + px * tailHalfW, y1 + py * tailHalfW },
                { x1 + ux * neckDist + px * neckHalfW, y1 + uy * neckDist + py * neckHalfW },
                { x1 + ux * (len - headLen) + px * headHalfW, y1 + uy * (len - headLen) + py * headHalfW },
                { x2, y2 },
                { x1 + ux * (len - headLen) - px * headHalfW, y1 + uy * (len - headLen) - py * headHalfW },
                { x1 + ux * neckDist - px * neckHalfW, y1 + uy * neckDist - py * neckHalfW },
                { x1 - px * tailHalfW, y1 - py * tailHalfW },
            };
            Gdiplus::SolidBrush br(ToGdipColor(a.color));
            g.FillPolygon(&br, pts, 7);
            break;
        }
        case ToolType::Pen:
        case ToolType::Highlight: {
            if (a.points.empty()) break;
            bool hi = (a.type == ToolType::Highlight);
            BYTE alpha = hi ? 105 : 255;
            float width = hi ? static_cast<float>(a.strokeWidth * 3 + 6) : static_cast<float>(a.strokeWidth);
            if (a.points.size() == 1) {
                Gdiplus::SolidBrush br(ToGdipColor(a.color, alpha));
                float r = width * 0.5f;
                g.FillEllipse(&br, a.points[0].x - ox - r, a.points[0].y - oy - r, width, width);
            } else {
                std::vector<Gdiplus::PointF> pts;
                pts.reserve(a.points.size());
                for (const auto& pt : a.points) {
                    pts.push_back(Gdiplus::PointF(static_cast<float>(pt.x - ox),
                                                  static_cast<float>(pt.y - oy)));
                }
                Gdiplus::Pen pen(ToGdipColor(a.color, alpha), width);
                pen.SetStartCap(Gdiplus::LineCapRound);
                pen.SetEndCap(Gdiplus::LineCapRound);
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                g.DrawLines(&pen, pts.data(), static_cast<INT>(pts.size()));
            }
            break;
        }
        case ToolType::Mosaic: {
            if (a.points.empty()) break;
            int blockSize = (std::max)(6, a.strokeWidth);
            int radiusBlocks = 1; // 每次覆盖约 (2*r+1) 块宽度
            auto prevMode = g.GetSmoothingMode();
            g.SetSmoothingMode(Gdiplus::SmoothingModeNone);

            // 去重已绘制的网格块，大幅提升性能
            std::vector<uint64_t> drawn;
            drawn.reserve(a.points.size() * 6);
            auto tryPaint = [&](int bx, int by) {
                if (bx < 0 || by < 0) return;
                uint64_t key = (static_cast<uint64_t>(bx) << 32) | static_cast<uint32_t>(by);
                for (uint64_t k : drawn) {
                    if (k == key) return;
                }
                drawn.push_back(key);
                DrawMosaicBlock(g, screen, bx, by, blockSize, ox, oy);
            };

            for (size_t i = 0; i < a.points.size(); ++i) {
                POINT p0 = a.points[i];
                POINT p1 = (i + 1 < a.points.size()) ? a.points[i + 1] : p0;
                int dist = (std::max)(std::abs(p1.x - p0.x), std::abs(p1.y - p0.y));
                int steps = (std::max)(1, dist / (std::max)(1, blockSize / 2));
                for (int s = 0; s <= steps; ++s) {
                    int px = p0.x + (p1.x - p0.x) * s / steps;
                    int py = p0.y + (p1.y - p0.y) * s / steps;
                    int cx = px / blockSize;
                    int cy = py / blockSize;
                    for (int dy = -radiusBlocks; dy <= radiusBlocks; ++dy) {
                        for (int dx = -radiusBlocks; dx <= radiusBlocks; ++dx) {
                            tryPaint(cx + dx, cy + dy);
                        }
                    }
                }
            }
            g.SetSmoothingMode(prevMode);
            break;
        }
        case ToolType::Step: {
            float radius = static_cast<float>(a.strokeWidth * 2 + 10);
            float cx = static_cast<float>(a.start.x - ox);
            float cy = static_cast<float>(a.start.y - oy);

            // 轻微投影
            Gdiplus::SolidBrush shadow(Gdiplus::Color(80, 0, 0, 0));
            g.FillEllipse(&shadow, cx - radius + 1.5f, cy - radius + 2.0f, radius * 2, radius * 2);

            // 主圆形 + 白色描边
            Gdiplus::SolidBrush fill(ToGdipColor(a.color));
            Gdiplus::Pen border(Gdiplus::Color(255, 255, 255, 255), 2.0f);
            g.FillEllipse(&fill, cx - radius, cy - radius, radius * 2, radius * 2);
            g.DrawEllipse(&border, cx - radius, cy - radius, radius * 2, radius * 2);

            // 居中数字
            wchar_t numBuf[16] = {};
            swprintf_s(numBuf, L"%d", a.stepNumber);
            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font font(&ff, radius * 0.95f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush txtBr(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::RectF rf(cx - radius, cy - radius + 0.5f, radius * 2, radius * 2);
            g.DrawString(numBuf, -1, &font, rf, &sf, &txtBr);
            break;
        }
        case ToolType::Text: {
            if (a.text.empty()) break;
            Gdiplus::FontFamily ff(L"Microsoft YaHei UI");
            Gdiplus::Font font(&ff, static_cast<Gdiplus::REAL>(a.fontSize),
                               Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            auto prevHint = g.GetTextRenderingHint();
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

            // 极淡阴影提高浅色/深色背景下的可读性
            Gdiplus::SolidBrush shadow(Gdiplus::Color(110, 0, 0, 0));
            Gdiplus::SolidBrush br(ToGdipColor(a.color));
            float tx = static_cast<float>(a.start.x - ox);
            float ty = static_cast<float>(a.start.y - oy);
            g.DrawString(a.text.c_str(), -1, &font, Gdiplus::PointF(tx + 1.0f, ty + 1.0f), &shadow);
            g.DrawString(a.text.c_str(), -1, &font, Gdiplus::PointF(tx, ty), &br);
            g.SetTextRenderingHint(prevHint);
            break;
        }
        default:
            break;
    }
}

// 将选区 + 所有标注合成为独立的 Gdiplus::Bitmap
inline std::unique_ptr<Gdiplus::Bitmap> RenderFinalBitmap(
    const ScreenCaptureData& screen,
    const RECT& selRect,
    const std::vector<Annotation>& annotations) {
    int w = RectWidth(selRect);
    int h = RectHeight(selRect);
    if (w <= 0 || h <= 0) return nullptr;

    auto bmp = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppARGB);
    if (bmp->GetLastStatus() != Gdiplus::Ok) return nullptr;

    // 直接拷贝选区原图像素（比 GDI BitBlt 更快更精准）
    Gdiplus::Rect lockRc(0, 0, w, h);
    Gdiplus::BitmapData bd = {};
    if (bmp->LockBits(&lockRc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        auto* dstBase = static_cast<uint8_t*>(bd.Scan0);
        for (int y = 0; y < h; ++y) {
            int sy = (std::max)(0, (std::min)(static_cast<int>(selRect.top) + y, screen.vh - 1));
            auto* dstRow = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(y) * bd.Stride);
            const uint32_t* srcRow = &screen.pixels[static_cast<size_t>(sy) * screen.vw];
            for (int x = 0; x < w; ++x) {
                int sx = (std::max)(0, (std::min)(static_cast<int>(selRect.left) + x, screen.vw - 1));
                dstRow[x] = srcRow[sx] | 0xFF000000u;
            }
        }
        bmp->UnlockBits(&bd);
    }

    // 叠加全部标注（自动裁剪在选区范围内）
    Gdiplus::Graphics g(bmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.SetClip(Gdiplus::Rect(0, 0, w, h));
    for (const auto& a : annotations) {
        DrawAnnotation(g, a, screen, selRect.left, selRect.top);
    }
    return bmp;
}

// ============================================================
// 圆角矩形路径辅助
// ============================================================
inline void AddRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

// ============================================================
// 绘制像素级放大镜与取色器
// ============================================================
inline void DrawMagnifier(Gdiplus::Graphics& g, const ScreenCaptureData& screen,
                          POINT cursorLocal, float scale) {
    const int gridCount = 11; // 11x11 采样
    const int cellPx = static_cast<int>(10 * scale);
    const int viewW = gridCount * cellPx;
    const int viewH = gridCount * cellPx;
    const int infoH = static_cast<int>(44 * scale);
    const int boxW = viewW;
    const int boxH = viewH + infoH;
    const int offset = static_cast<int>(20 * scale);

    int bx = cursorLocal.x + offset;
    int by = cursorLocal.y + offset;
    if (bx + boxW + 8 > screen.vw) bx = cursorLocal.x - boxW - offset;
    if (by + boxH + 8 > screen.vh) by = cursorLocal.y - boxH - offset;
    bx = (std::max)(8, (std::min)(bx, screen.vw - boxW - 8));
    by = (std::max)(8, (std::min)(by, screen.vh - boxH - 8));

    // 外框阴影
    Gdiplus::SolidBrush shadowBr(Gdiplus::Color(110, 0, 0, 0));
    g.FillRectangle(&shadowBr, bx + 2, by + 3, boxW, boxH);

    // 放大格绘制（关闭平滑保证像素锐利）
    auto prevMode = g.GetSmoothingMode();
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    int half = gridCount / 2;
    for (int gy = 0; gy < gridCount; ++gy) {
        for (int gx = 0; gx < gridCount; ++gx) {
            uint32_t px = screen.GetPixelSafe(cursorLocal.x + gx - half, cursorLocal.y + gy - half);
            Gdiplus::SolidBrush cellBr(Gdiplus::Color(255,
                                                      static_cast<BYTE>((px >> 16) & 0xFF),
                                                      static_cast<BYTE>((px >> 8) & 0xFF),
                                                      static_cast<BYTE>(px & 0xFF)));
            g.FillRectangle(&cellBr, bx + gx * cellPx, by + gy * cellPx, cellPx, cellPx);
        }
    }

    // 细网格线
    Gdiplus::Pen gridPen(Gdiplus::Color(60, 128, 128, 128), 1.0f);
    for (int i = 1; i < gridCount; ++i) {
        g.DrawLine(&gridPen, bx + i * cellPx, by, bx + i * cellPx, by + viewH);
        g.DrawLine(&gridPen, bx, by + i * cellPx, bx + viewW, by + i * cellPx);
    }

    // 中心十字与中心像素红框
    int midX = bx + half * cellPx;
    int midY = by + half * cellPx;
    Gdiplus::Pen crossPen(Gdiplus::Color(220, 0, 174, 255), 1.5f);
    g.DrawRectangle(&crossPen, midX, midY, cellPx, cellPx);

    // 信息区背景
    Gdiplus::SolidBrush infoBg(Gdiplus::Color(235, 24, 28, 36));
    g.FillRectangle(&infoBg, bx, by + viewH, boxW, infoH);

    // 边框
    Gdiplus::Pen borderPen(Gdiplus::Color(200, 255, 255, 255), 1.0f);
    g.DrawRectangle(&borderPen, bx, by, boxW, boxH);

    g.SetSmoothingMode(prevMode);

    // 文本信息：坐标 + 色值
    uint32_t cpx = screen.GetPixelSafe(cursorLocal.x, cursorLocal.y);
    int r = (cpx >> 16) & 0xFF;
    int gr = (cpx >> 8) & 0xFF;
    int b = cpx & 0xFF;

    wchar_t line1[64] = {}, line2[64] = {};
    swprintf_s(line1, L"POS (%d, %d)", cursorLocal.x, cursorLocal.y);
    swprintf_s(line2, L"#%02X%02X%02X  [C]复制", r, gr, b);

    Gdiplus::FontFamily ff(L"Consolas");
    Gdiplus::Font font(&ff, 11.0f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush txtBr(Gdiplus::Color(240, 235, 240, 248));
    Gdiplus::SolidBrush swatchBr(Gdiplus::Color(255, static_cast<BYTE>(r), static_cast<BYTE>(gr), static_cast<BYTE>(b)));

    int pad = static_cast<int>(6 * scale);
    g.DrawString(line1, -1, &font,
                 Gdiplus::PointF(static_cast<float>(bx + pad), static_cast<float>(by + viewH + 4 * scale)), &txtBr);

    int swSz = static_cast<int>(10 * scale);
    g.FillRectangle(&swatchBr, bx + pad, by + viewH + static_cast<int>(24 * scale), swSz, swSz);
    g.DrawRectangle(&borderPen, bx + pad, by + viewH + static_cast<int>(24 * scale), swSz, swSz);
    g.DrawString(line2, -1, &font,
                 Gdiplus::PointF(static_cast<float>(bx + pad + swSz + 5 * scale),
                                 static_cast<float>(by + viewH + 22 * scale)), &txtBr);
}

// ============================================================
// 高清矢量工具栏图标绘制
// ============================================================
enum class ToolbarAction {
    ToolRect,
    ToolEllipse,
    ToolArrow,
    ToolPen,
    ToolHighlight,
    ToolMosaic,
    ToolStep,
    ToolText,
    Undo,
    Redo,
    ScrollCapture,
    Pin,
    Save,
    Cancel,
    CopyDone
};

inline void DrawToolbarIcon(Gdiplus::Graphics& g, ToolbarAction act,
                            float cx, float cy, float s, Gdiplus::Color col) {
    Gdiplus::Pen pen(col, (std::max)(1.5f, 1.8f * s));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush br(col);

    switch (act) {
        case ToolbarAction::ToolRect:
            g.DrawRectangle(&pen, cx - 7 * s, cy - 5.5f * s, 14 * s, 11 * s);
            break;
        case ToolbarAction::ToolEllipse:
            g.DrawEllipse(&pen, cx - 7 * s, cy - 5.5f * s, 14 * s, 11 * s);
            break;
        case ToolbarAction::ToolArrow: {
            g.DrawLine(&pen, cx - 6.5f * s, cy + 6.5f * s, cx + 5.5f * s, cy - 5.5f * s);
            g.DrawLine(&pen, cx + 6 * s, cy - 6 * s, cx + 0.5f * s, cy - 5.5f * s);
            g.DrawLine(&pen, cx + 6 * s, cy - 6 * s, cx + 5.5f * s, cy - 0.5f * s);
            break;
        }
        case ToolbarAction::ToolPen: {
            Gdiplus::PointF pts[4] = {
                { cx - 7 * s, cy + 5 * s },
                { cx - 2 * s, cy - 5 * s },
                { cx + 2 * s, cy + 4 * s },
                { cx + 7 * s, cy - 4 * s },
            };
            g.DrawCurve(&pen, pts, 4, 0.5f);
            break;
        }
        case ToolbarAction::ToolHighlight: {
            Gdiplus::Pen thickPen(col, 3.6f * s);
            thickPen.SetStartCap(Gdiplus::LineCapFlat);
            thickPen.SetEndCap(Gdiplus::LineCapFlat);
            g.DrawLine(&thickPen, cx - 6 * s, cy + 3 * s, cx + 4 * s, cy - 5 * s);
            g.DrawLine(&pen, cx - 7 * s, cy + 6.5f * s, cx + 7 * s, cy + 6.5f * s);
            break;
        }
        case ToolbarAction::ToolMosaic: {
            float b = 4.2f * s;
            float ox = cx - 1.5f * b, oy = cy - 1.5f * b;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    if ((r + c) % 2 == 0) {
                        g.FillRectangle(&br, ox + c * b, oy + r * b, b - 0.6f, b - 0.6f);
                    } else {
                        Gdiplus::SolidBrush dim(Gdiplus::Color(90, col.GetR(), col.GetG(), col.GetB()));
                        g.FillRectangle(&dim, ox + c * b, oy + r * b, b - 0.6f, b - 0.6f);
                    }
                }
            }
            break;
        }
        case ToolbarAction::ToolStep: {
            g.DrawEllipse(&pen, cx - 7 * s, cy - 7 * s, 14 * s, 14 * s);
            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font font(&ff, 10.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(L"1", -1, &font, Gdiplus::RectF(cx - 7 * s, cy - 6.5f * s, 14 * s, 14 * s), &sf, &br);
            break;
        }
        case ToolbarAction::ToolText: {
            Gdiplus::FontFamily ff(L"Georgia");
            Gdiplus::Font font(&ff, 15.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(L"A", -1, &font, Gdiplus::RectF(cx - 8 * s, cy - 8 * s, 16 * s, 16 * s), &sf, &br);
            break;
        }
        case ToolbarAction::Undo: {
            g.DrawArc(&pen, cx - 5 * s, cy - 3 * s, 11 * s, 10 * s, 210, 180);
            g.DrawLine(&pen, cx - 5.5f * s, cy - 2 * s, cx - 1.5f * s, cy - 5.5f * s);
            g.DrawLine(&pen, cx - 5.5f * s, cy - 2 * s, cx - 1.5f * s, cy + 1.5f * s);
            break;
        }
        case ToolbarAction::Redo: {
            g.DrawArc(&pen, cx - 6 * s, cy - 3 * s, 11 * s, 10 * s, 150, 180);
            g.DrawLine(&pen, cx + 5.5f * s, cy - 2 * s, cx + 1.5f * s, cy - 5.5f * s);
            g.DrawLine(&pen, cx + 5.5f * s, cy - 2 * s, cx + 1.5f * s, cy + 1.5f * s);
            break;
        }
        case ToolbarAction::ScrollCapture: {
            // 长截图图标：长条框 + 向下滚动箭头
            g.DrawRectangle(&pen, cx - 5.5f * s, cy - 7 * s, 11 * s, 14 * s);
            g.DrawLine(&pen, cx, cy - 3.5f * s, cx, cy + 3.8f * s);
            g.DrawLine(&pen, cx, cy + 3.8f * s, cx - 3.0f * s, cy + 0.8f * s);
            g.DrawLine(&pen, cx, cy + 3.8f * s, cx + 3.0f * s, cy + 0.8f * s);
            break;
        }
        case ToolbarAction::Pin: {
            // 图钉图标
            g.DrawLine(&pen, cx - 4 * s, cy - 5 * s, cx + 4 * s, cy - 5 * s);
            g.DrawLine(&pen, cx - 2.5f * s, cy - 5 * s, cx - 3.5f * s, cy + 1 * s);
            g.DrawLine(&pen, cx + 2.5f * s, cy - 5 * s, cx + 3.5f * s, cy + 1 * s);
            g.DrawLine(&pen, cx - 5.5f * s, cy + 1 * s, cx + 5.5f * s, cy + 1 * s);
            g.DrawLine(&pen, cx, cy + 1 * s, cx, cy + 7 * s);
            break;
        }
        case ToolbarAction::Save: {
            // 下载/保存图标
            g.DrawLine(&pen, cx, cy - 6.5f * s, cx, cy + 2.5f * s);
            g.DrawLine(&pen, cx, cy + 2.5f * s, cx - 3.8f * s, cy - 1.2f * s);
            g.DrawLine(&pen, cx, cy + 2.5f * s, cx + 3.8f * s, cy - 1.2f * s);
            g.DrawLine(&pen, cx - 6.5f * s, cy + 3.5f * s, cx - 6.5f * s, cy + 6.5f * s);
            g.DrawLine(&pen, cx - 6.5f * s, cy + 6.5f * s, cx + 6.5f * s, cy + 6.5f * s);
            g.DrawLine(&pen, cx + 6.5f * s, cy + 6.5f * s, cx + 6.5f * s, cy + 3.5f * s);
            break;
        }
        case ToolbarAction::Cancel: {
            g.DrawLine(&pen, cx - 5.5f * s, cy - 5.5f * s, cx + 5.5f * s, cy + 5.5f * s);
            g.DrawLine(&pen, cx + 5.5f * s, cy - 5.5f * s, cx - 5.5f * s, cy + 5.5f * s);
            break;
        }
        case ToolbarAction::CopyDone: {
            Gdiplus::Pen okPen(col, (std::max)(2.0f, 2.2f * s));
            okPen.SetStartCap(Gdiplus::LineCapRound);
            okPen.SetEndCap(Gdiplus::LineCapRound);
            okPen.SetLineJoin(Gdiplus::LineJoinRound);
            g.DrawLine(&okPen, cx - 6.5f * s, cy + 0.5f * s, cx - 1.5f * s, cy + 5.5f * s);
            g.DrawLine(&okPen, cx - 1.5f * s, cy + 5.5f * s, cx + 7.0f * s, cy - 5.0f * s);
            break;
        }
    }
}

} // namespace snap
