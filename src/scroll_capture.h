#pragma once

#include "common.h"
#include "renderer.h"
#include "pin_window.h"

namespace snap {

// ============================================================
// 纯 C++ 零依赖垂直长图拼接引擎
// 支持：自动识别并剔除吸顶/吸底固定栏 + 行特征粗定位 + RGB 截断误差精匹配
// ============================================================
enum class StitchResult {
    FirstFrame,
    Stitched,
    NoChange,
    MatchFailed,
    MaxHeightReached
};

class ScrollStitcher {
public:
    static constexpr int kMaxHeight = 25000;

    void Reset(int width, int height) {
        m_width = width;
        m_frameH = height;
        m_height = 0;
        m_frameCount = 0;
        m_stickyTop = 0;
        m_stickyBottom = 0;
        m_stickyInitialized = false;
        m_pixels.clear();
        m_prevFrame.clear();
    }

    int Width() const { return m_width; }
    int Height() const { return m_height; }
    int FrameCount() const { return m_frameCount; }
    bool Empty() const { return m_pixels.empty() || m_width <= 0 || m_height <= 0; }
    const std::vector<uint32_t>& Pixels() const { return m_pixels; }

    StitchResult AppendFrame(const uint32_t* framePixels, int w, int h, int* outDy = nullptr) {
        if (outDy) *outDy = 0;
        if (!framePixels || w <= 0 || h <= 0) return StitchResult::MatchFailed;

        if (m_frameCount == 0) {
            Reset(w, h);
            size_t cnt = static_cast<size_t>(w) * h;
            m_pixels.assign(framePixels, framePixels + cnt);
            m_prevFrame.assign(framePixels, framePixels + cnt);
            m_height = h;
            m_frameCount = 1;
            return StitchResult::FirstFrame;
        }

        if (w != m_width || h != m_frameH) return StitchResult::MatchFailed;
        if (m_height >= kMaxHeight) return StitchResult::MaxHeightReached;

        // 水平有效匹配区间（避开左右边缘与右侧滚动条）
        int x0 = (std::min)(12, w / 12);
        int x1 = (std::max)(x0 + 1, w - (std::min)(28, w / 8));
        if (x1 - x0 < 16) {
            x0 = 0;
            x1 = w;
        }

        // 1. 先检查与上一帧是否基本无变化
        if (ComputeFrameDiff(m_prevFrame.data(), framePixels, w, h, x0, x1) < 1.2) {
            return StitchResult::NoChange;
        }

        // 2. 检测顶部/底部静止区域（吸顶导航栏、固定底栏）
        int curTop = DetectStickyTop(m_prevFrame.data(), framePixels, w, h, x0, x1);
        int curBot = DetectStickyBottom(m_prevFrame.data(), framePixels, w, h, x0, x1);

        // 限制固定栏最大占比，确保中间有足够滚动区
        int maxSticky = h / 3;
        curTop = (std::min)(curTop, maxSticky);
        curBot = (std::min)(curBot, maxSticky);

        // 避免将纯色空白边误判为固定底栏
        if (curBot > 0 && !HasBandTexture(framePixels, w, h - curBot, h, x0, x1)) {
            curBot = 0;
        }

        int useTop = curTop;
        int useBot = curBot;
        if (m_stickyInitialized) {
            useTop = (std::min)(m_stickyTop, curTop);
            useBot = (std::min)(m_stickyBottom, curBot);
        }

        int yTop = useTop;
        int yBot = h - useBot;
        int scrollH = yBot - yTop;
        if (scrollH < 40) {
            yTop = 0;
            yBot = h;
            useTop = 0;
            useBot = 0;
            scrollH = h;
        }

        // 3. 搜索垂直向下滚动偏移量 dy：frame[y] ≈ prev[y + dy]
        double bestErr = 1e9;
        int bestDy = FindBestVerticalShift(m_prevFrame.data(), framePixels, w, x0, x1, yTop, yBot, &bestErr);

        if (bestDy <= 0) {
            return StitchResult::NoChange;
        }
        // 误差阈值校验（RGB 三通道截断和均值，约等价单通道平均误差 <= 6）
        if (bestErr > 18.0) {
            return StitchResult::MatchFailed;
        }

        // 更新稳定固定栏高度
        if (!m_stickyInitialized) {
            m_stickyTop = useTop;
            m_stickyBottom = useBot;
            m_stickyInitialized = true;
        } else {
            m_stickyTop = useTop;
            m_stickyBottom = useBot;
        }

        // 4. 拼接新增像素行
        int bodyEnd = m_height - m_stickyBottom;
        if (bodyEnd < 0) bodyEnd = m_height;

        int maxAdd = kMaxHeight - (bodyEnd + m_stickyBottom);
        if (maxAdd <= 0) {
            return StitchResult::MaxHeightReached;
        }
        int addRows = (std::min)(bestDy, maxAdd);

        int newHeight = bodyEnd + addRows + m_stickyBottom;
        m_pixels.resize(static_cast<size_t>(newHeight) * m_width);

        // 拷贝新增的滚动内容行：frame[yBot - bestDy .. yBot - bestDy + addRows)
        int srcStart = yBot - bestDy;
        for (int r = 0; r < addRows; ++r) {
            const uint32_t* srcRow = framePixels + static_cast<size_t>(srcStart + r) * m_width;
            uint32_t* dstRow = m_pixels.data() + static_cast<size_t>(bodyEnd + r) * m_width;
            memcpy(dstRow, srcRow, static_cast<size_t>(m_width) * sizeof(uint32_t));
        }

        // 重新贴上底部固定栏
        if (m_stickyBottom > 0) {
            for (int r = 0; r < m_stickyBottom; ++r) {
                const uint32_t* srcRow = framePixels + static_cast<size_t>(h - m_stickyBottom + r) * m_width;
                uint32_t* dstRow = m_pixels.data() + static_cast<size_t>(bodyEnd + addRows + r) * m_width;
                memcpy(dstRow, srcRow, static_cast<size_t>(m_width) * sizeof(uint32_t));
            }
        }

        m_height = newHeight;
        m_prevFrame.assign(framePixels, framePixels + static_cast<size_t>(w) * h);
        ++m_frameCount;
        if (outDy) *outDy = addRows;

        return (m_height >= kMaxHeight) ? StitchResult::MaxHeightReached : StitchResult::Stitched;
    }

    std::unique_ptr<Gdiplus::Bitmap> BuildBitmap() const {
        if (Empty()) return nullptr;
        auto bmp = std::make_unique<Gdiplus::Bitmap>(m_width, m_height, PixelFormat32bppARGB);
        if (bmp->GetLastStatus() != Gdiplus::Ok) return nullptr;

        Gdiplus::Rect rc(0, 0, m_width, m_height);
        Gdiplus::BitmapData bd = {};
        if (bmp->LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
            auto* dstBase = static_cast<uint8_t*>(bd.Scan0);
            for (int y = 0; y < m_height; ++y) {
                auto* dstRow = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(y) * bd.Stride);
                const uint32_t* srcRow = m_pixels.data() + static_cast<size_t>(y) * m_width;
                for (int x = 0; x < m_width; ++x) {
                    dstRow[x] = srcRow[x] | 0xFF000000u;
                }
            }
            bmp->UnlockBits(&bd);
        }
        return bmp;
    }

private:
    static inline int PixelDiffRGB(uint32_t a, uint32_t b) {
        int db = static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF);
        int dg = static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF);
        int dr = static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF);
        return std::abs(dr) + std::abs(dg) + std::abs(db);
    }

    static inline uint8_t PixelLuma(uint32_t px) {
        int b = px & 0xFF;
        int g = (px >> 8) & 0xFF;
        int r = (px >> 16) & 0xFF;
        return static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
    }

    static double ComputeFrameDiff(const uint32_t* a, const uint32_t* b, int w, int h, int x0, int x1) {
        int yStep = (std::max)(1, h / 64);
        int xStep = (std::max)(1, (x1 - x0) / 64);
        uint64_t sum = 0, cnt = 0;
        for (int y = 0; y < h; y += yStep) {
            const uint32_t* ra = a + static_cast<size_t>(y) * w;
            const uint32_t* rb = b + static_cast<size_t>(y) * w;
            for (int x = x0; x < x1; x += xStep) {
                sum += static_cast<uint64_t>(PixelDiffRGB(ra[x], rb[x]));
                ++cnt;
            }
        }
        return cnt > 0 ? static_cast<double>(sum) / static_cast<double>(cnt) : 0.0;
    }

    static double ComputeRowDiff(const uint32_t* ra, const uint32_t* rb, int x0, int x1) {
        int xStep = (std::max)(1, (x1 - x0) / 96);
        uint64_t sum = 0, cnt = 0;
        for (int x = x0; x < x1; x += xStep) {
            sum += static_cast<uint64_t>(PixelDiffRGB(ra[x], rb[x]));
            ++cnt;
        }
        return cnt > 0 ? static_cast<double>(sum) / static_cast<double>(cnt) : 0.0;
    }

    static int DetectStickyTop(const uint32_t* prev, const uint32_t* curr, int w, int h, int x0, int x1) {
        int y = 0;
        while (y < h) {
            double d = ComputeRowDiff(prev + static_cast<size_t>(y) * w,
                                      curr + static_cast<size_t>(y) * w, x0, x1);
            if (d > 2.5) break;
            ++y;
        }
        // 只有顶部静止带具备一定高度且不是偶发 1~2 像素边框时才判定为吸顶栏
        return (y >= 8) ? y : 0;
    }

    static int DetectStickyBottom(const uint32_t* prev, const uint32_t* curr, int w, int h, int x0, int x1) {
        int cnt = 0;
        while (cnt < h) {
            int y = h - 1 - cnt;
            double d = ComputeRowDiff(prev + static_cast<size_t>(y) * w,
                                      curr + static_cast<size_t>(y) * w, x0, x1);
            if (d > 2.5) break;
            ++cnt;
        }
        return (cnt >= 8) ? cnt : 0;
    }

    static bool HasBandTexture(const uint32_t* img, int w, int y0, int y1, int x0, int x1) {
        if (y1 <= y0 || x1 <= x0 + 2) return false;
        int yStep = (std::max)(1, (y1 - y0) / 16);
        int xStep = (std::max)(2, (x1 - x0) / 48);
        uint64_t gradSum = 0, cnt = 0;
        for (int y = y0; y < y1; y += yStep) {
            const uint32_t* row = img + static_cast<size_t>(y) * w;
            for (int x = x0 + xStep; x < x1; x += xStep) {
                gradSum += static_cast<uint64_t>(PixelDiffRGB(row[x], row[x - xStep]));
                ++cnt;
            }
        }
        return cnt > 0 && (static_cast<double>(gradSum) / static_cast<double>(cnt)) > 4.0;
    }

    static int FindBestVerticalShift(const uint32_t* prev, const uint32_t* curr,
                                     int w, int x0, int x1, int yTop, int yBot,
                                     double* outBestErr) {
        int scrollH = yBot - yTop;
        int minOverlap = (std::max)(24, scrollH / 6);
        int maxDy = scrollH - minOverlap;
        if (maxDy < 1) return 0;

        // 阶段 1：预计算每行 64 点灰度签名，做高速粗搜索
        static constexpr int kSigCols = 64;
        int spanX = x1 - x0;
        int sampleX[kSigCols];
        for (int i = 0; i < kSigCols; ++i) {
            sampleX[i] = x0 + (i * spanX) / kSigCols;
        }

        std::vector<uint8_t> prevSig(static_cast<size_t>(scrollH) * kSigCols);
        std::vector<uint8_t> currSig(static_cast<size_t>(scrollH) * kSigCols);
        for (int r = 0; r < scrollH; ++r) {
            const uint32_t* pr = prev + static_cast<size_t>(yTop + r) * w;
            const uint32_t* cr = curr + static_cast<size_t>(yTop + r) * w;
            uint8_t* ps = prevSig.data() + static_cast<size_t>(r) * kSigCols;
            uint8_t* cs = currSig.data() + static_cast<size_t>(r) * kSigCols;
            for (int i = 0; i < kSigCols; ++i) {
                ps[i] = PixelLuma(pr[sampleX[i]]);
                cs[i] = PixelLuma(cr[sampleX[i]]);
            }
        }

        struct Cand { int dy; double err; };
        std::vector<Cand> cands;
        cands.reserve(static_cast<size_t>(maxDy / 2 + 2));

        int coarseStep = (maxDy > 160) ? 2 : 1;
        for (int dy = 1; dy <= maxDy; dy += coarseStep) {
            int overlap = scrollH - dy;
            int rowsToCheck = (std::min)(overlap, 56);
            uint64_t errSum = 0;
            uint64_t cnt = 0;
            for (int k = 0; k < rowsToCheck; ++k) {
                int r = (rowsToCheck == 1) ? 0 : (k * (overlap - 1)) / (rowsToCheck - 1);
                const uint8_t* cs = currSig.data() + static_cast<size_t>(r) * kSigCols;
                const uint8_t* ps = prevSig.data() + static_cast<size_t>(r + dy) * kSigCols;
                for (int i = 0; i < kSigCols; ++i) {
                    int d = std::abs(static_cast<int>(cs[i]) - static_cast<int>(ps[i]));
                    errSum += static_cast<uint64_t>(d < 25 ? d : 25); // 截断抗噪
                }
                cnt += kSigCols;
            }
            double e = cnt > 0 ? static_cast<double>(errSum) / static_cast<double>(cnt) : 1e9;
            cands.push_back({ dy, e });
        }

        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            if (std::fabs(a.err - b.err) < 0.05) return a.dy < b.dy;
            return a.err < b.err;
        });

        // 选取最多 6 个相距 >= 4px 的粗搜候选点做逐像素 RGB 精匹配
        std::vector<int> topDys;
        for (const auto& c : cands) {
            bool tooClose = false;
            for (int d : topDys) {
                if (std::abs(d - c.dy) < 4) {
                    tooClose = true;
                    break;
                }
            }
            if (!tooClose) {
                topDys.push_back(c.dy);
                if (topDys.size() >= 6) break;
            }
        }

        // 阶段 2：全 RGB 精匹配（截断误差，抗光标闪烁/小动图）
        int bestDy = 0;
        double bestErr = 1e9;
        int xStep = (spanX > 900) ? 3 : 2;

        auto evalFine = [&](int dy) -> double {
            int overlap = scrollH - dy;
            int rowsToCheck = (std::min)(overlap, 72);
            uint64_t errSum = 0;
            uint64_t cnt = 0;
            for (int k = 0; k < rowsToCheck; ++k) {
                int r = (rowsToCheck == 1) ? 0 : (k * (overlap - 1)) / (rowsToCheck - 1);
                const uint32_t* cr = curr + static_cast<size_t>(yTop + r) * w;
                const uint32_t* pr = prev + static_cast<size_t>(yTop + r + dy) * w;
                for (int x = x0; x < x1; x += xStep) {
                    int d = PixelDiffRGB(cr[x], pr[x]);
                    errSum += static_cast<uint64_t>(d < 60 ? d : 60);
                    ++cnt;
                }
            }
            return cnt > 0 ? static_cast<double>(errSum) / static_cast<double>(cnt) : 1e9;
        };

        for (int centerDy : topDys) {
            int lo = (std::max)(1, centerDy - 3);
            int hi = (std::min)(maxDy, centerDy + 3);
            for (int dy = lo; dy <= hi; ++dy) {
                double err = evalFine(dy);
                if (err + 0.25 < bestErr || (std::fabs(err - bestErr) <= 0.25 && dy < bestDy)) {
                    bestErr = err;
                    bestDy = dy;
                }
            }
        }

        if (outBestErr) *outBestErr = bestErr;
        return bestDy;
    }

    int m_width = 0;
    int m_frameH = 0;
    int m_height = 0;
    int m_frameCount = 0;
    int m_stickyTop = 0;
    int m_stickyBottom = 0;
    bool m_stickyInitialized = false;
    std::vector<uint32_t> m_pixels;
    std::vector<uint32_t> m_prevFrame;
};

// ============================================================
// 长截图实时采集会话：
//   - 4 条外部细边框窗（严格在选区外侧，零遮挡、零鼠标拦截）
//   - 悬浮控制条（显示尺寸/状态 + 自动滚动 / 贴图 / 保存 / 完成复制 / 取消）
//   - 侧边实时拼接缩略图预览窗
// ============================================================
class ScrollCaptureSession {
public:
    static constexpr const wchar_t* kBorderClass  = L"SnapTool_ScrollBorder";
    static constexpr const wchar_t* kBarClass     = L"SnapTool_ScrollBar";
    static constexpr const wchar_t* kPreviewClass = L"SnapTool_ScrollPreview";

    explicit ScrollCaptureSession(HINSTANCE hInst) : m_hInst(hInst) {
        RegisterClassesIfNeeded(hInst);
    }

    ~ScrollCaptureSession() {
        Close();
    }

    bool IsActive() const {
        return m_barWnd != nullptr && IsWindow(m_barWnd);
    }

    bool Start(RECT screenRect, float scale) {
        Close();
        int w = RectWidth(screenRect);
        int h = RectHeight(screenRect);
        if (w < 32 || h < 32) return false;

        m_rect = screenRect;
        m_scale = (scale > 0.5f) ? scale : 1.0f;
        m_autoScroll = false;
        m_noChangeCount = 0;
        m_flashTicks = 0;
        m_hoverBtn = -1;
        m_statusNote = L"滚轮下滑或点自动";
        m_stitcher.Reset(w, h);

        if (!InitCaptureBuffer(w, h)) {
            return false;
        }

        CreateBorderWindows();
        CreateControlBar();
        CreatePreviewWindow();

        // 延迟 80ms 抓首帧，确保原截图遮罩窗口已完全消失
        SetTimer(m_barWnd, TIMER_INIT_CAPTURE, 80, nullptr);
        return true;
    }

    void Close() {
        if (m_barWnd && IsWindow(m_barWnd)) {
            KillTimer(m_barWnd, TIMER_INIT_CAPTURE);
            KillTimer(m_barWnd, TIMER_POLL_CAPTURE);
            KillTimer(m_barWnd, TIMER_AUTO_SCROLL);
            KillTimer(m_barWnd, TIMER_FLASH);
            HWND h = m_barWnd;
            m_barWnd = nullptr;
            DestroyWindow(h);
        }
        if (m_previewWnd && IsWindow(m_previewWnd)) {
            HWND h = m_previewWnd;
            m_previewWnd = nullptr;
            DestroyWindow(h);
        }
        for (int i = 0; i < 4; ++i) {
            if (m_borderWnds[i] && IsWindow(m_borderWnds[i])) {
                DestroyWindow(m_borderWnds[i]);
            }
            m_borderWnds[i] = nullptr;
        }
        FreeCaptureBuffer();
        m_autoScroll = false;
    }

private:
    enum : UINT_PTR {
        TIMER_INIT_CAPTURE = 1,
        TIMER_POLL_CAPTURE = 2,
        TIMER_AUTO_SCROLL  = 3,
        TIMER_FLASH        = 4,
    };

    enum class BarAction {
        ToggleAuto,
        Pin,
        Save,
        CopyDone,
        Cancel
    };

    struct BarButton {
        BarAction action;
        RECT rect;
        const wchar_t* label;
    };

    static void RegisterClassesIfNeeded(HINSTANCE hInst) {
        static bool registered = false;
        if (registered) return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = BorderWndProc;
        wc.lpszClassName = kBorderClass;
        RegisterClassExW(&wc);

        wc.lpfnWndProc = BarWndProc;
        wc.lpszClassName = kBarClass;
        RegisterClassExW(&wc);

        wc.lpfnWndProc = PreviewWndProc;
        wc.lpszClassName = kPreviewClass;
        RegisterClassExW(&wc);

        registered = true;
    }

    static void ExcludeWindowFromCapture(HWND hwnd) {
        if (!hwnd) return;
        // WDA_EXCLUDEFROMCAPTURE = 0x00000011 (Windows 10 2004+)
        SetWindowDisplayAffinity(hwnd, 0x00000011);
    }

    bool InitCaptureBuffer(int w, int h) {
        FreeCaptureBuffer();
        HDC hScreen = GetDC(nullptr);
        m_capDC = CreateCompatibleDC(hScreen);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // 自顶向下
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        m_capBits = nullptr;
        m_capBmp = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &m_capBits, nullptr, 0);
        ReleaseDC(nullptr, hScreen);
        if (!m_capBmp || !m_capBits) {
            FreeCaptureBuffer();
            return false;
        }
        m_capOld = SelectObject(m_capDC, m_capBmp);
        return true;
    }

    void FreeCaptureBuffer() {
        if (m_capDC) {
            if (m_capOld) SelectObject(m_capDC, m_capOld);
            if (m_capBmp) DeleteObject(m_capBmp);
            DeleteDC(m_capDC);
            m_capDC = nullptr;
            m_capBmp = nullptr;
            m_capOld = nullptr;
            m_capBits = nullptr;
        }
    }

    void CreateBorderWindows() {
        const int t = (std::max)(2, static_cast<int>(std::round(3.0f * m_scale)));
        RECT rects[4] = {
            { m_rect.left - t, m_rect.top - t, m_rect.right + t, m_rect.top },      // Top
            { m_rect.left - t, m_rect.bottom,  m_rect.right + t, m_rect.bottom + t }, // Bottom
            { m_rect.left - t, m_rect.top,     m_rect.left,      m_rect.bottom },   // Left
            { m_rect.right,    m_rect.top,     m_rect.right + t, m_rect.bottom },   // Right
        };
        for (int i = 0; i < 4; ++i) {
            m_borderWnds[i] = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                kBorderClass, L"",
                WS_POPUP | WS_VISIBLE,
                rects[i].left, rects[i].top, RectWidth(rects[i]), RectHeight(rects[i]),
                nullptr, nullptr, m_hInst, this);
            ExcludeWindowFromCapture(m_borderWnds[i]);
        }
    }

    void InvalidateBorders() {
        for (int i = 0; i < 4; ++i) {
            if (m_borderWnds[i]) InvalidateRect(m_borderWnds[i], nullptr, FALSE);
        }
    }

    void CreateControlBar() {
        HMONITOR hMon = MonitorFromRect(&m_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);

        int barH = static_cast<int>(42 * m_scale);
        int barW = static_cast<int>(640 * m_scale);
        int maxBarW = RectWidth(mi.rcWork) - 16;
        if (maxBarW > 320) barW = (std::min)(barW, maxBarW);

        int margin = static_cast<int>(10 * m_scale);
        int bx = (m_rect.left + m_rect.right - barW) / 2;
        bx = (std::max)(static_cast<int>(mi.rcWork.left) + 8,
                        (std::min)(bx, static_cast<int>(mi.rcWork.right) - barW - 8));

        int by = m_rect.bottom + margin;
        if (by + barH + 8 > mi.rcWork.bottom) {
            by = m_rect.top - margin - barH;
            if (by < mi.rcWork.top + 8) {
                by = mi.rcWork.bottom - barH - 12;
            }
        }

        m_barW = barW;
        m_barH = barH;
        m_barWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kBarClass, L"SnapToolScrollBar",
            WS_POPUP | WS_VISIBLE,
            bx, by, barW, barH,
            nullptr, nullptr, m_hInst, this);
        ExcludeWindowFromCapture(m_barWnd);
        LayoutBarButtons();
    }

    void CreatePreviewWindow() {
        HMONITOR hMon = MonitorFromRect(&m_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);

        int gap = static_cast<int>(12 * m_scale);
        int pw = static_cast<int>(160 * m_scale);
        int ph = (std::min)(RectHeight(m_rect), static_cast<int>(RectHeight(mi.rcWork) * 0.72f));
        ph = (std::max)(static_cast<int>(160 * m_scale), ph);

        int px = m_rect.right + gap;
        if (px + pw + 8 > mi.rcWork.right) {
            px = m_rect.left - gap - pw;
            if (px < mi.rcWork.left + 8) {
                // 屏幕两侧空间都不足时，不显示侧边预览窗以免遮挡选区
                return;
            }
        }
        int py = (std::max)(static_cast<int>(mi.rcWork.top) + 8,
                            (std::min)(static_cast<int>(m_rect.top), static_cast<int>(mi.rcWork.bottom) - ph - 8));

        m_previewW = pw;
        m_previewH = ph;
        m_previewWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kPreviewClass, L"SnapToolScrollPreview",
            WS_POPUP | WS_VISIBLE,
            px, py, pw, ph,
            nullptr, nullptr, m_hInst, this);
        ExcludeWindowFromCapture(m_previewWnd);
    }

    void LayoutBarButtons() {
        m_barBtns.clear();
        int padX = static_cast<int>(8 * m_scale);
        int btnH = static_cast<int>(26 * m_scale);
        int by = (m_barH - btnH) / 2;
        int gap = static_cast<int>(6 * m_scale);

        struct Spec { BarAction act; const wchar_t* label; int w; };
        Spec specs[] = {
            { BarAction::Cancel,     L"取消",       static_cast<int>(48 * m_scale) },
            { BarAction::CopyDone,   L"完成复制",   static_cast<int>(76 * m_scale) },
            { BarAction::Save,       L"保存",       static_cast<int>(50 * m_scale) },
            { BarAction::Pin,        L"贴图",       static_cast<int>(50 * m_scale) },
            { BarAction::ToggleAuto, m_autoScroll ? L"暂停滚动" : L"自动滚动", static_cast<int>(76 * m_scale) },
        };

        int rx = m_barW - padX;
        for (const auto& sp : specs) {
            BarButton b;
            b.action = sp.act;
            b.label = sp.label;
            b.rect = { rx - sp.w, by, rx, by + btnH };
            m_barBtns.push_back(b);
            rx -= sp.w + gap;
        }
        m_statusAreaRight = rx - static_cast<int>(4 * m_scale);
    }

    StitchResult CaptureAndStitchOnce() {
        if (!m_capDC || !m_capBits) return StitchResult::MatchFailed;
        int w = RectWidth(m_rect);
        int h = RectHeight(m_rect);

        HDC hScreen = GetDC(nullptr);
        BitBlt(m_capDC, 0, 0, w, h, hScreen, m_rect.left, m_rect.top, SRCCOPY);
        GdiFlush();
        ReleaseDC(nullptr, hScreen);

        size_t cnt = static_cast<size_t>(w) * h;
        auto* raw = static_cast<uint32_t*>(m_capBits);
        for (size_t i = 0; i < cnt; ++i) {
            raw[i] |= 0xFF000000u;
        }

        int dy = 0;
        StitchResult res = m_stitcher.AppendFrame(raw, w, h, &dy);
        if (res == StitchResult::FirstFrame || res == StitchResult::Stitched) {
            m_noChangeCount = 0;
            if (res == StitchResult::Stitched) {
                m_flashTicks = 1;
                SetTimer(m_barWnd, TIMER_FLASH, 200, nullptr);
                InvalidateBorders();
                if (!m_autoScroll) {
                    m_statusNote = L"拼接成功，继续下滑";
                }
            }
            if (m_previewWnd) InvalidateRect(m_previewWnd, nullptr, FALSE);
            if (m_barWnd) InvalidateRect(m_barWnd, nullptr, FALSE);
        } else if (res == StitchResult::MaxHeightReached) {
            StopAutoScroll();
            m_statusNote = L"已达上限，请保存/复制";
            if (m_barWnd) InvalidateRect(m_barWnd, nullptr, FALSE);
        } else if (res == StitchResult::MatchFailed) {
            m_statusNote = L"滚动过快，请慢一点";
            if (m_barWnd) InvalidateRect(m_barWnd, nullptr, FALSE);
        }
        return res;
    }

    void StartAutoScroll() {
        if (m_autoScroll) return;
        m_autoScroll = true;
        m_noChangeCount = 0;
        m_statusNote = L"自动滚动中...";
        LayoutBarButtons();
        if (m_barWnd) {
            SetTimer(m_barWnd, TIMER_AUTO_SCROLL, 280, nullptr);
            InvalidateRect(m_barWnd, nullptr, FALSE);
        }
    }

    void StopAutoScroll() {
        if (!m_autoScroll) return;
        m_autoScroll = false;
        if (m_barWnd) {
            KillTimer(m_barWnd, TIMER_AUTO_SCROLL);
            LayoutBarButtons();
            InvalidateRect(m_barWnd, nullptr, FALSE);
        }
    }

    void PerformAutoScrollStep() {
        // 先抓取当前画面（承接上一次滚轮后的稳定帧）
        StitchResult res = CaptureAndStitchOnce();
        if (res == StitchResult::MaxHeightReached) {
            return;
        }
        if (res == StitchResult::NoChange) {
            ++m_noChangeCount;
            if (m_noChangeCount >= 3) {
                StopAutoScroll();
                m_statusNote = L"已到底部，请点完成";
                if (m_barWnd) InvalidateRect(m_barWnd, nullptr, FALSE);
                return;
            }
        } else if (res == StitchResult::Stitched) {
            m_noChangeCount = 0;
        }

        // 将鼠标定位到选区中心附近（若当前鼠标在选区外），发送向下滚轮事件
        POINT cur = {};
        GetCursorPos(&cur);
        if (!PointInRect(cur, m_rect)) {
            int cx = (m_rect.left + m_rect.right) / 2;
            int cy = (m_rect.top + m_rect.bottom) / 2;
            SetCursorPos(cx, cy);
        }

        INPUT inp = {};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inp.mi.mouseData = static_cast<DWORD>(-3 * WHEEL_DELTA);
        SendInput(1, &inp, sizeof(INPUT));
    }

    void FinishCopy() {
        StopAutoScroll();
        auto bmp = m_stitcher.BuildBitmap();
        HWND owner = m_barWnd;
        if (bmp) {
            bool ok = CopyGdipBitmapToClipboard(owner, bmp.get());
            if (!ok) {
                // 超大图剪贴板分配失败时，引导直接保存文件
                SetWindowPos(m_barWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                SaveGdipBitmapWithDialog(owner, bmp.get());
            }
        }
        Close();
    }

    void FinishSave() {
        StopAutoScroll();
        auto bmp = m_stitcher.BuildBitmap();
        if (!bmp) {
            Close();
            return;
        }
        // 隐藏边框与预览避免干扰保存对话框
        for (int i = 0; i < 4; ++i) {
            if (m_borderWnds[i]) ShowWindow(m_borderWnds[i], SW_HIDE);
        }
        if (m_previewWnd) ShowWindow(m_previewWnd, SW_HIDE);
        SetWindowPos(m_barWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        bool saved = SaveGdipBitmapWithDialog(m_barWnd, bmp.get());
        if (saved) {
            Close();
        } else {
            for (int i = 0; i < 4; ++i) {
                if (m_borderWnds[i]) ShowWindow(m_borderWnds[i], SW_SHOWNA);
            }
            if (m_previewWnd) ShowWindow(m_previewWnd, SW_SHOWNA);
            SetWindowPos(m_barWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    void FinishPin() {
        StopAutoScroll();
        auto bmp = m_stitcher.BuildBitmap();
        int sx = m_rect.left;
        int sy = m_rect.top;
        Close();
        if (bmp) {
            PinWindow::Create(m_hInst, std::move(bmp), sx, sy);
        }
    }

    void HandleBarClick(BarAction act) {
        switch (act) {
            case BarAction::ToggleAuto:
                if (m_autoScroll) {
                    StopAutoScroll();
                    m_statusNote = L"已暂停，可手动下滑";
                    if (m_barWnd) InvalidateRect(m_barWnd, nullptr, FALSE);
                } else {
                    StartAutoScroll();
                }
                break;
            case BarAction::Pin:      FinishPin(); break;
            case BarAction::Save:     FinishSave(); break;
            case BarAction::CopyDone: FinishCopy(); break;
            case BarAction::Cancel:   Close(); break;
        }
    }

    void PaintControlBar(HDC hdc) {
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, m_barW, m_barH);
        HGDIOBJ old = SelectObject(memDC, memBmp);

        Gdiplus::Graphics g(memDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        // 背景与描边
        Gdiplus::SolidBrush bgBr(Gdiplus::Color(255, 24, 29, 40));
        Gdiplus::Pen bdPen(Gdiplus::Color(255, 56, 189, 248), 1.2f);
        g.FillRectangle(&bgBr, 0, 0, m_barW, m_barH);
        g.DrawRectangle(&bdPen, 0.5f, 0.5f, static_cast<float>(m_barW - 1), static_cast<float>(m_barH - 1));

        // 左侧状态指示点 + 尺寸 + 提示文字
        float dotR = 4.0f * m_scale;
        float dotX = 12.0f * m_scale;
        float dotY = m_barH * 0.5f;
        Gdiplus::SolidBrush dotBr(m_autoScroll
            ? Gdiplus::Color(255, 52, 211, 153)
            : (m_flashTicks > 0 ? Gdiplus::Color(255, 52, 211, 153) : Gdiplus::Color(255, 56, 189, 248)));
        g.FillEllipse(&dotBr, dotX - dotR, dotY - dotR, dotR * 2, dotR * 2);

        wchar_t infoBuf[128] = {};
        if (m_stitcher.Height() > 0) {
            swprintf_s(infoBuf, L"%d×%d · %s", m_stitcher.Width(), m_stitcher.Height(), m_statusNote.c_str());
        } else {
            swprintf_s(infoBuf, L"长截图准备中...");
        }

        Gdiplus::FontFamily ff(L"Microsoft YaHei UI");
        Gdiplus::Font infoFont(&ff, 12.0f * m_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush txtBr(Gdiplus::Color(245, 235, 242, 250));
        Gdiplus::StringFormat sfLeft;
        sfLeft.SetAlignment(Gdiplus::StringAlignmentNear);
        sfLeft.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sfLeft.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        sfLeft.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        float textX = dotX + dotR + 8.0f * m_scale;
        float textW = (std::max)(20.0f, static_cast<float>(m_statusAreaRight) - textX);
        g.DrawString(infoBuf, -1, &infoFont, Gdiplus::RectF(textX, 0.5f, textW, static_cast<float>(m_barH)), &sfLeft, &txtBr);

        // 右侧按钮
        Gdiplus::Font btnFont(&ff, 11.5f * m_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::StringFormat sfCenter;
        sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
        sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        for (int i = 0; i < static_cast<int>(m_barBtns.size()); ++i) {
            const auto& b = m_barBtns[i];
            bool hov = (i == m_hoverBtn);
            Gdiplus::Color fillCol(255, 45, 55, 72);
            Gdiplus::Color textCol(255, 240, 245, 255);

            if (b.action == BarAction::CopyDone) {
                fillCol = hov ? Gdiplus::Color(255, 16, 185, 129) : Gdiplus::Color(255, 5, 150, 105);
            } else if (b.action == BarAction::Cancel) {
                fillCol = hov ? Gdiplus::Color(255, 220, 38, 38) : Gdiplus::Color(255, 75, 40, 48);
                textCol = hov ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 252, 165, 165);
            } else if (b.action == BarAction::ToggleAuto && m_autoScroll) {
                fillCol = hov ? Gdiplus::Color(255, 37, 99, 235) : Gdiplus::Color(255, 29, 78, 216);
            } else if (hov) {
                fillCol = Gdiplus::Color(255, 68, 82, 106);
            }

            Gdiplus::GraphicsPath p;
            AddRoundedRectPath(p, static_cast<float>(b.rect.left), static_cast<float>(b.rect.top),
                               static_cast<float>(RectWidth(b.rect)), static_cast<float>(RectHeight(b.rect)),
                               4.0f * m_scale);
            Gdiplus::SolidBrush br(fillCol);
            Gdiplus::SolidBrush tbr(textCol);
            g.FillPath(&br, &p);
            g.DrawString(b.label, -1, &btnFont,
                         Gdiplus::RectF(static_cast<float>(b.rect.left), static_cast<float>(b.rect.top),
                                        static_cast<float>(RectWidth(b.rect)), static_cast<float>(RectHeight(b.rect))),
                         &sfCenter, &tbr);
        }

        BitBlt(hdc, 0, 0, m_barW, m_barH, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, old);
        DeleteObject(memBmp);
        DeleteDC(memDC);
    }

    void PaintPreview(HDC hdc) {
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, m_previewW, m_previewH);
        HGDIOBJ old = SelectObject(memDC, memBmp);

        Gdiplus::Graphics g(memDC);
        g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, 20, 24, 34));
        g.FillRectangle(&bg, 0, 0, m_previewW, m_previewH);

        int headerH = static_cast<int>(22 * m_scale);
        Gdiplus::FontFamily ff(L"Microsoft YaHei UI");
        Gdiplus::Font hdrFont(&ff, 10.5f * m_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush hdrTxt(Gdiplus::Color(220, 186, 204, 224));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(L"长图预览", -1, &hdrFont,
                     Gdiplus::RectF(0, 0, static_cast<float>(m_previewW), static_cast<float>(headerH)),
                     &sf, &hdrTxt);

        int pad = 4;
        int availW = m_previewW - pad * 2;
        int availH = m_previewH - headerH - pad * 2;

        if (!m_stitcher.Empty() && availW > 8 && availH > 8) {
            int sw = m_stitcher.Width();
            int sh = m_stitcher.Height();

            // 按宽度等比缩放；若缩放后高于预览框，则始终展示最新拼接的底部区域
            float scaleW = static_cast<float>(availW) / static_cast<float>(sw);
            int scaledH = static_cast<int>(std::round(sh * scaleW));
            int drawW = availW;
            int drawH = (std::min)(availH, scaledH);

            int srcY = 0;
            int srcH = sh;
            if (scaledH > availH) {
                srcH = (std::max)(1, static_cast<int>(std::round(availH / scaleW)));
                srcY = (std::max)(0, sh - srcH);
            }

            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = sw;
            bmi.bmiHeader.biHeight = -srcH;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(memDC, HALFTONE);
            StretchDIBits(memDC,
                          pad, headerH + pad, drawW, drawH,
                          0, 0, sw, srcH,
                          m_stitcher.Pixels().data() + static_cast<size_t>(srcY) * sw,
                          &bmi, DIB_RGB_COLORS, SRCCOPY);
        }

        Gdiplus::Pen bd(Gdiplus::Color(255, 56, 189, 248), 1.2f);
        g.DrawRectangle(&bd, 0.5f, 0.5f, static_cast<float>(m_previewW - 1), static_cast<float>(m_previewH - 1));

        BitBlt(hdc, 0, 0, m_previewW, m_previewH, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, old);
        DeleteObject(memBmp);
        DeleteDC(memDC);
    }

    static LRESULT CALLBACK BorderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ScrollCaptureSession* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<ScrollCaptureSession*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ScrollCaptureSession*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            COLORREF col = (self && self->m_flashTicks > 0) ? RGB(52, 211, 153) : RGB(34, 158, 255);
            HBRUSH br = CreateSolidBrush(col);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ScrollCaptureSession* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<ScrollCaptureSession*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ScrollCaptureSession*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            self->PaintPreview(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK BarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ScrollCaptureSession* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<ScrollCaptureSession*>(cs->lpCreateParams);
            self->m_barWnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ScrollCaptureSession*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->HandleBarMessage(msg, wParam, lParam)
                    : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT HandleBarMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;

            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(m_barWnd, &ps);
                PaintControlBar(hdc);
                EndPaint(m_barWnd, &ps);
                return 0;
            }

            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT) {
                    SetCursor(LoadCursorW(nullptr, m_hoverBtn >= 0 ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;

            case WM_MOUSEMOVE: {
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                int prev = m_hoverBtn;
                m_hoverBtn = -1;
                for (int i = 0; i < static_cast<int>(m_barBtns.size()); ++i) {
                    if (PointInRect(pt, m_barBtns[i].rect)) {
                        m_hoverBtn = i;
                        break;
                    }
                }
                if (!m_trackingMouse) {
                    m_trackingMouse = true;
                    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_barWnd, 0 };
                    TrackMouseEvent(&tme);
                }
                if (prev != m_hoverBtn) {
                    InvalidateRect(m_barWnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_MOUSELEAVE:
                m_trackingMouse = false;
                if (m_hoverBtn != -1) {
                    m_hoverBtn = -1;
                    InvalidateRect(m_barWnd, nullptr, FALSE);
                }
                return 0;

            case WM_LBUTTONUP: {
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                for (const auto& b : m_barBtns) {
                    if (PointInRect(pt, b.rect)) {
                        HandleBarClick(b.action);
                        return 0;
                    }
                }
                return 0;
            }

            case WM_TIMER: {
                if (wParam == TIMER_INIT_CAPTURE) {
                    KillTimer(m_barWnd, TIMER_INIT_CAPTURE);
                    CaptureAndStitchOnce();
                    // 启动 120ms 定时检测，支持用户直接手动滚轮实时拼接
                    SetTimer(m_barWnd, TIMER_POLL_CAPTURE, 120, nullptr);
                    return 0;
                }
                if (wParam == TIMER_POLL_CAPTURE) {
                    // 按 ESC 随时取消长截图
                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                        Close();
                        return 0;
                    }
                    if (!m_autoScroll) {
                        CaptureAndStitchOnce();
                    }
                    return 0;
                }
                if (wParam == TIMER_AUTO_SCROLL) {
                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                        Close();
                        return 0;
                    }
                    PerformAutoScrollStep();
                    return 0;
                }
                if (wParam == TIMER_FLASH) {
                    KillTimer(m_barWnd, TIMER_FLASH);
                    m_flashTicks = 0;
                    InvalidateBorders();
                    if (m_barWnd) InvalidateRect(m_barWnd, nullptr, FALSE);
                    return 0;
                }
                return 0;
            }
        }
        return DefWindowProcW(m_barWnd, msg, wParam, lParam);
    }

    HINSTANCE m_hInst = nullptr;
    HWND m_borderWnds[4] = {};
    HWND m_barWnd = nullptr;
    HWND m_previewWnd = nullptr;

    RECT m_rect = {};
    float m_scale = 1.0f;
    int m_barW = 0;
    int m_barH = 0;
    int m_statusAreaRight = 0;
    int m_previewW = 0;
    int m_previewH = 0;

    HDC m_capDC = nullptr;
    HBITMAP m_capBmp = nullptr;
    HGDIOBJ m_capOld = nullptr;
    void* m_capBits = nullptr;

    ScrollStitcher m_stitcher;
    std::vector<BarButton> m_barBtns;
    int m_hoverBtn = -1;
    bool m_trackingMouse = false;
    bool m_autoScroll = false;
    int m_noChangeCount = 0;
    int m_flashTicks = 0;
    std::wstring m_statusNote;
};

} // namespace snap
