#pragma once

#include "common.h"
#include "renderer.h"

namespace snap {

class PinWindow {
public:
    static constexpr const wchar_t* kClassName = L"SnapTool_PinWindow";

    static void RegisterClassIfNeeded(HINSTANCE hInst) {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    static PinWindow* Create(HINSTANCE hInst, std::unique_ptr<Gdiplus::Bitmap> bmp,
                             int screenX, int screenY) {
        if (!bmp) return nullptr;
        RegisterClassIfNeeded(hInst);

        auto* self = new PinWindow(std::move(bmp));

        // 若图片尺寸（如长截图）超过当前显示器工作区 85%，自动按比例缩小初始显示窗口
        POINT pt = { screenX, screenY };
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi)) {
            int maxW = static_cast<int>((mi.rcWork.right - mi.rcWork.left) * 0.85f);
            int maxH = static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.85f);
            if (static_cast<int>(self->m_origW) > maxW || static_cast<int>(self->m_origH) > maxH) {
                float sx = (self->m_origW > 0) ? static_cast<float>(maxW) / self->m_origW : 1.0f;
                float sy = (self->m_origH > 0) ? static_cast<float>(maxH) / self->m_origH : 1.0f;
                self->m_scale = (std::max)(0.15f, (std::min)(sx, sy));
            }
        }

        int w = (std::max)(24, static_cast<int>(std::round(self->m_origW * self->m_scale))) + kBorder * 2;
        int h = (std::max)(24, static_cast<int>(std::round(self->m_origH * self->m_scale))) + kBorder * 2;
        int winX = screenX - kBorder;
        int winY = screenY - kBorder;
        if (mi.cbSize == sizeof(mi) && mi.rcWork.right > mi.rcWork.left) {
            winX = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(winX, static_cast<int>(mi.rcWork.right) - w));
            winY = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(winY, static_cast<int>(mi.rcWork.bottom) - h));
        }

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            kClassName, L"SnapPin",
            WS_POPUP | WS_VISIBLE,
            winX, winY, w, h,
            nullptr, nullptr, hInst, self);

        if (!hwnd) {
            delete self;
            return nullptr;
        }

        SetLayeredWindowAttributes(hwnd, 0, self->m_alpha, LWA_ALPHA);
        s_instances.push_back(self);
        return self;
    }

    static int GetCount() {
        return static_cast<int>(s_instances.size());
    }

    static void CloseAll() {
        auto copy = s_instances;
        for (auto* p : copy) {
            if (p && p->m_hwnd && IsWindow(p->m_hwnd)) {
                DestroyWindow(p->m_hwnd);
            }
        }
    }

private:
    static constexpr int kBorder = 2;
    enum : UINT {
        IDM_PIN_COPY = 3001,
        IDM_PIN_SAVE = 3002,
        IDM_PIN_RESET_SCALE = 3003,
        IDM_PIN_TOGGLE_ALPHA = 3004,
        IDM_PIN_CLOSE = 3005,
        IDM_PIN_CLOSE_ALL = 3006,
    };

    PinWindow(std::unique_ptr<Gdiplus::Bitmap> bmp)
        : m_bmp(std::move(bmp)) {
        m_origW = m_bmp ? m_bmp->GetWidth() : 100;
        m_origH = m_bmp ? m_bmp->GetHeight() : 100;
    }

    ~PinWindow() {
        auto it = std::find(s_instances.begin(), s_instances.end(), this);
        if (it != s_instances.end()) s_instances.erase(it);
    }

    void ApplyScaleAroundCursor(float newScale, POINT screenPt) {
        newScale = (std::max)(0.15f, (std::min)(newScale, 5.0f));
        if (std::fabs(newScale - m_scale) < 0.001f) return;

        RECT rc = {};
        GetWindowRect(m_hwnd, &rc);
        float rx = (rc.right > rc.left) ? static_cast<float>(screenPt.x - rc.left) / (rc.right - rc.left) : 0.5f;
        float ry = (rc.bottom > rc.top) ? static_cast<float>(screenPt.y - rc.top) / (rc.bottom - rc.top) : 0.5f;
        rx = (std::max)(0.0f, (std::min)(rx, 1.0f));
        ry = (std::max)(0.0f, (std::min)(ry, 1.0f));

        m_scale = newScale;
        int newW = (std::max)(24, static_cast<int>(std::round(m_origW * m_scale))) + kBorder * 2;
        int newH = (std::max)(24, static_cast<int>(std::round(m_origH * m_scale))) + kBorder * 2;
        int newX = screenPt.x - static_cast<int>(std::round(newW * rx));
        int newY = screenPt.y - static_cast<int>(std::round(newH * ry));

        SetWindowPos(m_hwnd, HWND_TOPMOST, newX, newY, newW, newH, SWP_NOACTIVATE);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void FlashCopyFeedback() {
        m_flashTicks = 1;
        SetTimer(m_hwnd, 1, 220, nullptr);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void ShowContextMenu(POINT screenPt) {
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, IDM_PIN_COPY, L"复制图片 (Ctrl+C / 双击)");
        AppendMenuW(hMenu, MF_STRING, IDM_PIN_SAVE, L"另存为图片 (Ctrl+S)");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        wchar_t scaleText[64] = {};
        swprintf_s(scaleText, L"恢复原始大小 (当前 %d%%)", static_cast<int>(std::round(m_scale * 100.0f)));
        AppendMenuW(hMenu, MF_STRING, IDM_PIN_RESET_SCALE, scaleText);
        AppendMenuW(hMenu, MF_STRING | (m_alpha < 255 ? MF_CHECKED : 0), IDM_PIN_TOGGLE_ALPHA, L"半透明显示 (Ctrl+滚轮调节)");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_PIN_CLOSE, L"关闭贴图 (Esc / 中键)");
        if (s_instances.size() > 1) {
            AppendMenuW(hMenu, MF_STRING, IDM_PIN_CLOSE_ALL, L"关闭所有贴图");
        }

        SetForegroundWindow(m_hwnd);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, m_hwnd, nullptr);
        DestroyMenu(hMenu);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(m_hwnd, &ps);
                RECT rc;
                GetClientRect(m_hwnd, &rc);
                int cw = rc.right - rc.left;
                int ch = rc.bottom - rc.top;

                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP memBmp = CreateCompatibleBitmap(hdc, cw, ch);
                HGDIOBJ old = SelectObject(memDC, memBmp);

                Gdiplus::Graphics g(memDC);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

                // 边框颜色：复制闪绿 / 悬停亮蓝 / 默认科技蓝
                Gdiplus::Color borderCol = m_flashTicks > 0
                    ? Gdiplus::Color(255, 34, 197, 94)
                    : (m_hovered ? Gdiplus::Color(255, 56, 189, 248) : Gdiplus::Color(255, 37, 99, 235));
                Gdiplus::SolidBrush borderBr(borderCol);
                g.FillRectangle(&borderBr, 0, 0, cw, ch);

                if (m_bmp) {
                    g.DrawImage(m_bmp.get(), kBorder, kBorder, cw - kBorder * 2, ch - kBorder * 2);
                }

                BitBlt(hdc, 0, 0, cw, ch, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, old);
                DeleteObject(memBmp);
                DeleteDC(memDC);
                EndPaint(m_hwnd, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;

            case WM_LBUTTONDOWN: {
                SetFocus(m_hwnd);
                SetCapture(m_hwnd);
                m_dragging = true;
                GetCursorPos(&m_dragStartCursor);
                RECT wr;
                GetWindowRect(m_hwnd, &wr);
                m_dragStartWin = { wr.left, wr.top };
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (!m_hovered) {
                    m_hovered = true;
                    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
                    TrackMouseEvent(&tme);
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                }
                if (m_dragging) {
                    POINT cur;
                    GetCursorPos(&cur);
                    int nx = m_dragStartWin.x + (cur.x - m_dragStartCursor.x);
                    int ny = m_dragStartWin.y + (cur.y - m_dragStartCursor.y);
                    SetWindowPos(m_hwnd, HWND_TOPMOST, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                m_hovered = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;

            case WM_LBUTTONUP:
                if (m_dragging) {
                    m_dragging = false;
                    ReleaseCapture();
                }
                return 0;

            case WM_LBUTTONDBLCLK:
                if (CopyGdipBitmapToClipboard(m_hwnd, m_bmp.get())) {
                    FlashCopyFeedback();
                }
                return 0;

            case WM_MBUTTONUP:
                DestroyWindow(m_hwnd);
                return 0;

            case WM_RBUTTONUP: {
                POINT pt;
                GetCursorPos(&pt);
                ShowContextMenu(pt);
                return 0;
            }
            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (ctrl) {
                    int nextAlpha = static_cast<int>(m_alpha) + (delta > 0 ? 20 : -20);
                    m_alpha = static_cast<BYTE>((std::max)(60, (std::min)(255, nextAlpha)));
                    SetLayeredWindowAttributes(m_hwnd, 0, m_alpha, LWA_ALPHA);
                } else {
                    POINT pt;
                    GetCursorPos(&pt);
                    float factor = (delta > 0) ? 1.1f : (1.0f / 1.1f);
                    ApplyScaleAroundCursor(m_scale * factor, pt);
                }
                return 0;
            }
            case WM_KEYDOWN: {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (wParam == VK_ESCAPE) {
                    DestroyWindow(m_hwnd);
                    return 0;
                } else if (ctrl && wParam == 'C') {
                    if (CopyGdipBitmapToClipboard(m_hwnd, m_bmp.get())) {
                        FlashCopyFeedback();
                    }
                    return 0;
                } else if (ctrl && wParam == 'S') {
                    SaveGdipBitmapWithDialog(m_hwnd, m_bmp.get());
                    return 0;
                }
                break;
            }
            case WM_COMMAND: {
                UINT cmd = LOWORD(wParam);
                if (cmd == IDM_PIN_COPY) {
                    if (CopyGdipBitmapToClipboard(m_hwnd, m_bmp.get())) {
                        FlashCopyFeedback();
                    }
                } else if (cmd == IDM_PIN_SAVE) {
                    SaveGdipBitmapWithDialog(m_hwnd, m_bmp.get());
                } else if (cmd == IDM_PIN_RESET_SCALE) {
                    RECT wr;
                    GetWindowRect(m_hwnd, &wr);
                    POINT center = { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
                    ApplyScaleAroundCursor(1.0f, center);
                } else if (cmd == IDM_PIN_TOGGLE_ALPHA) {
                    m_alpha = (m_alpha == 255) ? 160 : 255;
                    SetLayeredWindowAttributes(m_hwnd, 0, m_alpha, LWA_ALPHA);
                } else if (cmd == IDM_PIN_CLOSE) {
                    DestroyWindow(m_hwnd);
                } else if (cmd == IDM_PIN_CLOSE_ALL) {
                    CloseAll();
                }
                return 0;
            }
            case WM_TIMER:
                if (wParam == 1) {
                    KillTimer(m_hwnd, 1);
                    m_flashTicks = 0;
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                }
                return 0;

            case WM_NCDESTROY:
                SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
                m_hwnd = nullptr;
                delete this;
                return 0;
        }
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        PinWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<PinWindow*>(cs->lpCreateParams);
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->HandleMessage(msg, wParam, lParam)
                    : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND m_hwnd = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> m_bmp;
    UINT m_origW = 0;
    UINT m_origH = 0;
    float m_scale = 1.0f;
    BYTE m_alpha = 255;
    bool m_hovered = false;
    bool m_dragging = false;
    int m_flashTicks = 0;
    POINT m_dragStartCursor = {};
    POINT m_dragStartWin = {};

    static inline std::vector<PinWindow*> s_instances;
};

} // namespace snap
