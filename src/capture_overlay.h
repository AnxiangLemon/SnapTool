#pragma once

#include "common.h"
#include "renderer.h"
#include "pin_window.h"
#include "scroll_capture.h"

namespace snap {

class CaptureOverlay {
public:
    static constexpr const wchar_t* kClassName = L"SnapTool_CaptureOverlay";

    CaptureOverlay(HINSTANCE hInst, ScrollCaptureSession* scrollSession = nullptr)
        : m_hInst(hInst), m_scrollSession(scrollSession) {
        RegisterClassIfNeeded(hInst);
        m_editDarkBg = CreateSolidBrush(RGB(28, 33, 44));
        m_editLightBg = CreateSolidBrush(RGB(245, 247, 250));
    }

    ~CaptureOverlay() {
        Close();
        if (m_editDarkBg) DeleteObject(m_editDarkBg);
        if (m_editLightBg) DeleteObject(m_editLightBg);
    }

    bool IsActive() const {
        return m_hwnd != nullptr && IsWindow(m_hwnd);
    }

    bool Start() {
        if (IsActive()) {
            SetForegroundWindow(m_hwnd);
            return true;
        }

        if (!CaptureVirtualScreen(m_screen, nullptr)) {
            return false;
        }

        // 重置状态
        m_phase = Phase::Hovering;
        m_dragMode = DragMode::None;
        m_selRect = { 0, 0, 0, 0 };
        m_hoverWinRect = { 0, 0, 0, 0 };
        m_tool = ToolType::None;
        m_annotations.clear();
        m_redoStack.clear();
        m_stepCounter = 1;
        m_isDrawing = false;
        m_hoverBtnIdx = -1;
        m_hoverColorIdx = -1;
        m_hoverSizeIdx = -1;
        m_toastText.clear();

        POINT cur = {};
        GetCursorPos(&cur);
        m_scale = GetDpiScaleForPoint(cur);
        m_cursorLocal = { cur.x - m_screen.vx, cur.y - m_screen.vy };
        UpdateHoverWindow(m_cursorLocal);

        // 创建覆盖完整虚拟桌面的无边框置顶窗口
        m_hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kClassName, L"SnapToolCapture",
            WS_POPUP | WS_CLIPCHILDREN,
            m_screen.vx, m_screen.vy, m_screen.vw, m_screen.vh,
            nullptr, nullptr, m_hInst, this);

        if (!m_hwnd) {
            m_screen.Reset();
            return false;
        }

        InitBackBuffer();
        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
        SetFocus(m_hwnd);
        Redraw();
        return true;
    }

    void Close() {
        if (GetCapture() == m_hwnd) ReleaseCapture();
        CancelTextEdit();
        if (m_hwnd && IsWindow(m_hwnd)) {
            HWND h = m_hwnd;
            m_hwnd = nullptr;
            DestroyWindow(h);
        }
        FreeBackBuffer();
        m_screen.Reset();
    }

private:
    enum class Phase { Hovering, Selecting, Editing };
    enum class DragMode {
        None, MoveSel,
        ResizeTL, ResizeT, ResizeTR, ResizeR,
        ResizeBR, ResizeB, ResizeBL, ResizeL
    };

    struct ToolbarBtn {
        ToolbarAction action;
        RECT rect;
        const wchar_t* tooltip;
        bool isSeparatorAfter = false;
    };

    static void RegisterClassIfNeeded(HINSTANCE hInst) {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    void InitBackBuffer() {
        FreeBackBuffer();
        HDC hdc = GetDC(m_hwnd);
        m_backDC = CreateCompatibleDC(hdc);
        m_backBmp = CreateCompatibleBitmap(hdc, m_screen.vw, m_screen.vh);
        m_backOld = SelectObject(m_backDC, m_backBmp);

        m_srcDC = CreateCompatibleDC(hdc);
        m_srcOld = SelectObject(m_srcDC, m_screen.hBitmap);
        ReleaseDC(m_hwnd, hdc);
    }

    void FreeBackBuffer() {
        if (m_srcDC) {
            SelectObject(m_srcDC, m_srcOld);
            DeleteDC(m_srcDC);
            m_srcDC = nullptr;
            m_srcOld = nullptr;
        }
        if (m_backDC) {
            SelectObject(m_backDC, m_backOld);
            DeleteObject(m_backBmp);
            DeleteDC(m_backDC);
            m_backDC = nullptr;
            m_backBmp = nullptr;
            m_backOld = nullptr;
        }
    }

    void UpdateHoverWindow(POINT localPt) {
        m_hoverWinRect = { 0, 0, m_screen.vw, m_screen.vh };
        for (const auto& r : m_screen.windowRects) {
            if (PointInRect(localPt, r)) {
                m_hoverWinRect = r;
                break;
            }
        }
    }

    // 8 个选区手柄命中检测
    DragMode HitTestSelection(POINT pt) const {
        if (m_phase != Phase::Editing) return DragMode::None;
        int tol = static_cast<int>(7 * m_scale);
        int l = m_selRect.left, t = m_selRect.top, r = m_selRect.right, b = m_selRect.bottom;
        int mx = (l + r) / 2, my = (t + b) / 2;

        auto nearPt = [&](int x, int y) {
            return std::abs(pt.x - x) <= tol && std::abs(pt.y - y) <= tol;
        };

        if (nearPt(l, t)) return DragMode::ResizeTL;
        if (nearPt(r, t)) return DragMode::ResizeTR;
        if (nearPt(r, b)) return DragMode::ResizeBR;
        if (nearPt(l, b)) return DragMode::ResizeBL;
        if (nearPt(mx, t) || (std::abs(pt.y - t) <= tol && pt.x > l + tol && pt.x < r - tol)) return DragMode::ResizeT;
        if (nearPt(mx, b) || (std::abs(pt.y - b) <= tol && pt.x > l + tol && pt.x < r - tol)) return DragMode::ResizeB;
        if (nearPt(l, my) || (std::abs(pt.x - l) <= tol && pt.y > t + tol && pt.y < b - tol)) return DragMode::ResizeL;
        if (nearPt(r, my) || (std::abs(pt.x - r) <= tol && pt.y > t + tol && pt.y < b - tol)) return DragMode::ResizeR;

        if (PointInRect(pt, m_selRect) && m_tool == ToolType::None) {
            return DragMode::MoveSel;
        }
        return DragMode::None;
    }

    HCURSOR GetCursorForDragMode(DragMode dm) const {
        switch (dm) {
            case DragMode::ResizeTL:
            case DragMode::ResizeBR: return LoadCursorW(nullptr, IDC_SIZENWSE);
            case DragMode::ResizeTR:
            case DragMode::ResizeBL: return LoadCursorW(nullptr, IDC_SIZENESW);
            case DragMode::ResizeT:
            case DragMode::ResizeB:  return LoadCursorW(nullptr, IDC_SIZENS);
            case DragMode::ResizeL:
            case DragMode::ResizeR:  return LoadCursorW(nullptr, IDC_SIZEWE);
            case DragMode::MoveSel:  return LoadCursorW(nullptr, IDC_SIZEALL);
            default:                 return LoadCursorW(nullptr, IDC_CROSS);
        }
    }

    // 计算浮动工具栏与二级属性栏布局
    void ComputeToolbarLayout() {
        m_toolbarBtns.clear();
        m_colorRects.clear();
        m_sizeRects.clear();
        m_toolbarRect = { 0, 0, 0, 0 };
        m_subBarRect = { 0, 0, 0, 0 };
        if (m_phase != Phase::Editing) return;

        struct Def { ToolbarAction act; const wchar_t* tip; bool sep; };
        static constexpr Def kDefs[] = {
            { ToolbarAction::ToolRect,      L"矩形",                 false },
            { ToolbarAction::ToolEllipse,   L"椭圆",                 false },
            { ToolbarAction::ToolArrow,     L"箭头",                 false },
            { ToolbarAction::ToolPen,       L"画笔",                 false },
            { ToolbarAction::ToolHighlight, L"荧光笔",               false },
            { ToolbarAction::ToolMosaic,    L"马赛克",               false },
            { ToolbarAction::ToolStep,      L"序号标记",             false },
            { ToolbarAction::ToolText,      L"文字",                 true  },
            { ToolbarAction::Undo,          L"撤销 (Ctrl+Z)",        false },
            { ToolbarAction::Redo,          L"重做 (Ctrl+Y)",        true  },
            { ToolbarAction::ScrollCapture, L"长截图 (L)",           false },
            { ToolbarAction::Pin,           L"贴到桌面 (T)",         false },
            { ToolbarAction::Save,          L"保存图片 (Ctrl+S)",    false },
            { ToolbarAction::Cancel,        L"取消 (Esc)",           false },
            { ToolbarAction::CopyDone,      L"完成并复制 (Enter)",   false },
        };

        int btnW = static_cast<int>(32 * m_scale);
        int btnH = static_cast<int>(30 * m_scale);
        int padX = static_cast<int>(8 * m_scale);
        int padY = static_cast<int>(5 * m_scale);
        int gap  = static_cast<int>(3 * m_scale);
        int sepW = static_cast<int>(10 * m_scale);

        int totalW = padX * 2;
        for (size_t i = 0; i < _countof(kDefs); ++i) {
            totalW += btnW;
            if (i + 1 < _countof(kDefs)) totalW += kDefs[i].sep ? sepW : gap;
        }
        int barH = btnH + padY * 2;

        // 确定工具栏位置：优先选区右下方，放不下放右上方，再放不下贴内侧底部
        int margin = static_cast<int>(8 * m_scale);
        bool hasSub = (m_tool != ToolType::None);
        int subH = hasSub ? static_cast<int>(36 * m_scale) : 0;
        int needH = barH + (hasSub ? (subH + static_cast<int>(6 * m_scale)) : 0);

        int bx = m_selRect.right - totalW;
        bx = (std::max)(8, (std::min)(bx, m_screen.vw - totalW - 8));

        int by = m_selRect.bottom + margin;
        bool placeSubBelow = true;
        if (by + needH + 8 > m_screen.vh) {
            if (m_selRect.top - margin - needH >= 8) {
                by = m_selRect.top - margin - barH;
                placeSubBelow = false;
            } else {
                by = (std::max)(8, static_cast<int>(m_selRect.bottom) - margin - barH);
                placeSubBelow = false;
            }
        }

        m_toolbarRect = { bx, by, bx + totalW, by + barH };
        int cx = bx + padX;
        for (size_t i = 0; i < _countof(kDefs); ++i) {
            ToolbarBtn tb;
            tb.action = kDefs[i].act;
            tb.tooltip = kDefs[i].tip;
            tb.isSeparatorAfter = kDefs[i].sep;
            tb.rect = { cx, by + padY, cx + btnW, by + padY + btnH };
            m_toolbarBtns.push_back(tb);
            cx += btnW + (kDefs[i].sep ? sepW : gap);
        }

        // 二级属性栏（粗细/字号 + 颜色）
        if (hasSub) {
            int subPadX = static_cast<int>(10 * m_scale);
            int szItemW = static_cast<int>(24 * m_scale);
            int colItemW = static_cast<int>(22 * m_scale);
            bool showColors = (m_tool != ToolType::Mosaic);

            int subW = subPadX * 2 + szItemW * 3;
            if (showColors) {
                subW += static_cast<int>(14 * m_scale) + colItemW * static_cast<int>(_countof(kColorPresets));
            }
            int sy = placeSubBelow
                ? (m_toolbarRect.bottom + static_cast<int>(6 * m_scale))
                : (m_toolbarRect.top - static_cast<int>(6 * m_scale) - subH);
            sy = (std::max)(8, (std::min)(sy, m_screen.vh - subH - 8));
            int sx = (std::max)(8, (std::min)(bx, m_screen.vw - subW - 8));

            m_subBarRect = { sx, sy, sx + subW, sy + subH };

            int ix = sx + subPadX;
            int cy = sy + subH / 2;
            for (int i = 0; i < 3; ++i) {
                m_sizeRects.push_back({ ix, cy - szItemW / 2, ix + szItemW, cy + szItemW / 2 });
                ix += szItemW;
            }
            if (showColors) {
                ix += static_cast<int>(14 * m_scale);
                for (size_t i = 0; i < _countof(kColorPresets); ++i) {
                    m_colorRects.push_back({ ix, cy - colItemW / 2, ix + colItemW, cy + colItemW / 2 });
                    ix += colItemW;
                }
            }
        }
    }

    // ============================================================
    // 文字输入框管理（原生 EDIT 控件，完美支持中文输入法）
    // ============================================================
    void BeginTextEdit(POINT localPt) {
        CommitTextEdit();
        m_textOrigin = localPt;

        int fontPx = static_cast<int>(kFontPresets[m_sizeIdx] * m_scale);
        m_textFont = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

        int editW = (std::max)(static_cast<int>(140 * m_scale), (std::min)(static_cast<int>(360 * m_scale),
                               static_cast<int>(m_selRect.right - localPt.x - 4)));
        int editH = (std::max)(static_cast<int>(36 * m_scale), fontPx + static_cast<int>(16 * m_scale));
        int ex = (std::max)(m_selRect.left + 2, (std::min)(localPt.x, m_selRect.right - editW - 2));
        int ey = (std::max)(m_selRect.top + 2, (std::min)(localPt.y, m_selRect.bottom - editH - 2));
        m_textOrigin = { ex, ey };

        m_textEdit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            ex, ey, editW, editH,
            m_hwnd, reinterpret_cast<HMENU>(101), m_hInst, nullptr);

        if (m_textEdit) {
            SendMessageW(m_textEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_textFont), TRUE);
            SetWindowSubclassProc(m_textEdit);
            SetFocus(m_textEdit);
        }
    }

    void SetWindowSubclassProc(HWND hEdit) {
        SetWindowLongPtrW(hEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_origEditProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CaptureOverlay::EditSubclassProc)));
    }

    static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

        if (msg == WM_KEYDOWN) {
            if (wParam == VK_ESCAPE) {
                self->CancelTextEdit();
                self->Redraw();
                return 0;
            }
            if (wParam == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) {
                self->CommitTextEdit();
                self->Redraw();
                return 0;
            }
        }
        LRESULT res = CallWindowProcW(self->m_origEditProc, hwnd, msg, wParam, lParam);
        if (msg == WM_CHAR || msg == WM_KEYUP) {
            self->AutoResizeTextEdit();
        }
        return res;
    }

    void AutoResizeTextEdit() {
        if (!m_textEdit) return;
        int lines = static_cast<int>(SendMessageW(m_textEdit, EM_GETLINECOUNT, 0, 0));
        int fontPx = static_cast<int>(kFontPresets[m_sizeIdx] * m_scale);
        int needH = (std::max)(1, lines) * (fontPx + 6) + 14;
        RECT rc;
        GetWindowRect(m_textEdit, &rc);
        int curW = rc.right - rc.left;
        int maxH = (std::max)(40, static_cast<int>(m_selRect.bottom - m_textOrigin.y - 2));
        SetWindowPos(m_textEdit, nullptr, 0, 0, curW, (std::min)(needH, maxH), SWP_NOMOVE | SWP_NOZORDER);
    }

    void CommitTextEdit() {
        if (!m_textEdit) return;
        int len = GetWindowTextLengthW(m_textEdit);
        if (len > 0) {
            std::wstring txt(static_cast<size_t>(len), L'\0');
            GetWindowTextW(m_textEdit, &txt[0], len + 1);
            // 去除首尾空白校验
            bool hasNonSpace = false;
            for (wchar_t c : txt) {
                if (c != L' ' && c != L'\r' && c != L'\n' && c != L'\t') {
                    hasNonSpace = true;
                    break;
                }
            }
            if (hasNonSpace) {
                Annotation a;
                a.type = ToolType::Text;
                a.color = kColorPresets[m_colorIdx].rgb;
                a.fontSize = static_cast<int>(kFontPresets[m_sizeIdx] * m_scale);
                a.start = { m_textOrigin.x + 3, m_textOrigin.y + 3 };
                a.text = std::move(txt);
                m_annotations.push_back(std::move(a));
                m_redoStack.clear();
            }
        }
        CancelTextEdit();
    }

    void CancelTextEdit() {
        if (m_textEdit) {
            HWND h = m_textEdit;
            m_textEdit = nullptr;
            DestroyWindow(h);
        }
        if (m_textFont) {
            DeleteObject(m_textFont);
            m_textFont = nullptr;
        }
        if (m_hwnd && IsWindow(m_hwnd)) {
            SetFocus(m_hwnd);
        }
    }

    // ============================================================
    // 导出动作：复制 / 保存 / 钉图
    // ============================================================
    void ActionCopyAndFinish() {
        CommitTextEdit();
        if (m_phase != Phase::Editing || RectWidth(m_selRect) <= 0 || RectHeight(m_selRect) <= 0) {
            Close();
            return;
        }
        auto bmp = RenderFinalBitmap(m_screen, m_selRect, m_annotations);
        if (bmp) {
            CopyGdipBitmapToClipboard(m_hwnd, bmp.get());
        }
        Close();
    }

    void ActionSaveAndFinish() {
        CommitTextEdit();
        if (m_phase != Phase::Editing || RectWidth(m_selRect) <= 0 || RectHeight(m_selRect) <= 0) return;
        auto bmp = RenderFinalBitmap(m_screen, m_selRect, m_annotations);
        if (!bmp) return;

        // 暂时取消置顶避免遮挡系统文件对话框
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        bool saved = SaveGdipBitmapWithDialog(m_hwnd, bmp.get());
        if (saved) {
            Close();
        } else {
            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(m_hwnd);
            Redraw();
        }
    }

    void ActionPinAndFinish() {
        CommitTextEdit();
        if (m_phase != Phase::Editing || RectWidth(m_selRect) <= 0 || RectHeight(m_selRect) <= 0) return;
        auto bmp = RenderFinalBitmap(m_screen, m_selRect, m_annotations);
        int sx = m_screen.vx + m_selRect.left;
        int sy = m_screen.vy + m_selRect.top;
        Close();
        if (bmp) {
            PinWindow::Create(m_hInst, std::move(bmp), sx, sy);
        }
    }

    void ActionStartScrollCapture() {
        CommitTextEdit();
        if (m_phase != Phase::Editing || RectWidth(m_selRect) < 32 || RectHeight(m_selRect) < 32) return;
        RECT screenRect = {
            m_screen.vx + m_selRect.left,
            m_screen.vy + m_selRect.top,
            m_screen.vx + m_selRect.right,
            m_screen.vy + m_selRect.bottom
        };
        float scale = m_scale;
        Close();
        if (m_scrollSession) {
            m_scrollSession->Start(screenRect, scale);
        }
    }

    void ActionUndo() {
        CommitTextEdit();
        if (m_annotations.empty()) return;
        if (m_annotations.back().type == ToolType::Step && m_stepCounter > 1) {
            --m_stepCounter;
        }
        m_redoStack.push_back(std::move(m_annotations.back()));
        m_annotations.pop_back();
        Redraw();
    }

    void ActionRedo() {
        CommitTextEdit();
        if (m_redoStack.empty()) return;
        if (m_redoStack.back().type == ToolType::Step) {
            ++m_stepCounter;
        }
        m_annotations.push_back(std::move(m_redoStack.back()));
        m_redoStack.pop_back();
        Redraw();
    }

    void HandleToolbarClick(ToolbarAction act) {
        auto toggleTool = [&](ToolType t) {
            CommitTextEdit();
            m_tool = (m_tool == t) ? ToolType::None : t;
            Redraw();
        };
        switch (act) {
            case ToolbarAction::ToolRect:      toggleTool(ToolType::Rect); break;
            case ToolbarAction::ToolEllipse:   toggleTool(ToolType::Ellipse); break;
            case ToolbarAction::ToolArrow:     toggleTool(ToolType::Arrow); break;
            case ToolbarAction::ToolPen:       toggleTool(ToolType::Pen); break;
            case ToolbarAction::ToolHighlight: toggleTool(ToolType::Highlight); break;
            case ToolbarAction::ToolMosaic:    toggleTool(ToolType::Mosaic); break;
            case ToolbarAction::ToolStep:      toggleTool(ToolType::Step); break;
            case ToolbarAction::ToolText:      toggleTool(ToolType::Text); break;
            case ToolbarAction::Undo:          ActionUndo(); break;
            case ToolbarAction::Redo:          ActionRedo(); break;
            case ToolbarAction::ScrollCapture: ActionStartScrollCapture(); break;
            case ToolbarAction::Pin:           ActionPinAndFinish(); break;
            case ToolbarAction::Save:          ActionSaveAndFinish(); break;
            case ToolbarAction::Cancel:        Close(); break;
            case ToolbarAction::CopyDone:      ActionCopyAndFinish(); break;
        }
    }

    bool IsToolActive(ToolbarAction act) const {
        return (act == ToolbarAction::ToolRect && m_tool == ToolType::Rect) ||
               (act == ToolbarAction::ToolEllipse && m_tool == ToolType::Ellipse) ||
               (act == ToolbarAction::ToolArrow && m_tool == ToolType::Arrow) ||
               (act == ToolbarAction::ToolPen && m_tool == ToolType::Pen) ||
               (act == ToolbarAction::ToolHighlight && m_tool == ToolType::Highlight) ||
               (act == ToolbarAction::ToolMosaic && m_tool == ToolType::Mosaic) ||
               (act == ToolbarAction::ToolStep && m_tool == ToolType::Step) ||
               (act == ToolbarAction::ToolText && m_tool == ToolType::Text);
    }

    // ============================================================
    // 全屏双缓冲绘制
    // ============================================================
    void Redraw() {
        if (!m_hwnd || !m_backDC || !m_srcDC) return;
        ComputeToolbarLayout();

        // 1. 拷贝原始桌面底图
        BitBlt(m_backDC, 0, 0, m_screen.vw, m_screen.vh, m_srcDC, 0, 0, SRCCOPY);

        Gdiplus::Graphics g(m_backDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        // 确定亮显区域（选区或悬停窗口）
        RECT activeRect = (m_phase == Phase::Hovering) ? m_hoverWinRect : m_selRect;
        bool hasActive = (RectWidth(activeRect) > 0 && RectHeight(activeRect) > 0);

        // 2. 绘制半透明暗色遮罩（留空选区）
        Gdiplus::SolidBrush maskBr(Gdiplus::Color(125, 0, 0, 0));
        if (!hasActive) {
            g.FillRectangle(&maskBr, 0, 0, m_screen.vw, m_screen.vh);
        } else {
            // 仅在选区外四块矩形铺暗色遮罩，避免全屏 Path 性能开销
            if (activeRect.top > 0)
                g.FillRectangle(&maskBr, 0, 0, m_screen.vw, activeRect.top);
            if (activeRect.bottom < m_screen.vh)
                g.FillRectangle(&maskBr, 0, activeRect.bottom, m_screen.vw, m_screen.vh - activeRect.bottom);
            if (activeRect.left > 0)
                g.FillRectangle(&maskBr, 0, activeRect.top, activeRect.left, RectHeight(activeRect));
            if (activeRect.right < m_screen.vw)
                g.FillRectangle(&maskBr, activeRect.right, activeRect.top, m_screen.vw - activeRect.right, RectHeight(activeRect));
        }

        // 3. 在选区内部裁剪绘制全部标注 + 正在绘制的临时标注
        if (m_phase == Phase::Editing && hasActive) {
            g.SetClip(Gdiplus::Rect(m_selRect.left, m_selRect.top, RectWidth(m_selRect), RectHeight(m_selRect)));
            for (const auto& a : m_annotations) {
                DrawAnnotation(g, a, m_screen, 0, 0);
            }
            if (m_isDrawing) {
                DrawAnnotation(g, m_currentDraw, m_screen, 0, 0);
            }
            g.ResetClip();
        }

        // 4. 选区边框 + 8 个调整手柄
        if (hasActive) {
            float borderW = (m_phase == Phase::Hovering) ? (2.5f * m_scale) : (1.5f * m_scale);
            Gdiplus::Pen selPen(Gdiplus::Color(255, 34, 158, 255), borderW);
            g.DrawRectangle(&selPen,
                            static_cast<float>(activeRect.left), static_cast<float>(activeRect.top),
                            static_cast<float>(RectWidth(activeRect)), static_cast<float>(RectHeight(activeRect)));

            if (m_phase == Phase::Editing) {
                float hr = 4.0f * m_scale;
                int l = m_selRect.left, t = m_selRect.top, r = m_selRect.right, b = m_selRect.bottom;
                int mx = (l + r) / 2, my = (t + b) / 2;
                POINT handles[8] = { {l,t}, {mx,t}, {r,t}, {r,my}, {r,b}, {mx,b}, {l,b}, {l,my} };
                Gdiplus::SolidBrush hFill(Gdiplus::Color(255, 34, 158, 255));
                Gdiplus::Pen hBorder(Gdiplus::Color(255, 255, 255, 255), 1.4f);
                for (const auto& hp : handles) {
                    g.FillEllipse(&hFill, hp.x - hr, hp.y - hr, hr * 2, hr * 2);
                    g.DrawEllipse(&hBorder, hp.x - hr, hp.y - hr, hr * 2, hr * 2);
                }
            }

            // 尺寸徽章（左上角上方）
            wchar_t szText[64] = {};
            swprintf_s(szText, L"%d × %d", RectWidth(activeRect), RectHeight(activeRect));
            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font font(&ff, 12.0f * m_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::RectF measure;
            g.MeasureString(szText, -1, &font, Gdiplus::PointF(0, 0), &measure);
            int badgeW = static_cast<int>(measure.Width + 14 * m_scale);
            int badgeH = static_cast<int>(measure.Height + 6 * m_scale);
            int bx = activeRect.left;
            int by = activeRect.top - badgeH - static_cast<int>(6 * m_scale);
            if (by < 6) by = activeRect.top + static_cast<int>(6 * m_scale);
            bx = (std::max)(6, (std::min)(bx, m_screen.vw - badgeW - 6));

            Gdiplus::GraphicsPath bp;
            AddRoundedRectPath(bp, static_cast<float>(bx), static_cast<float>(by),
                               static_cast<float>(badgeW), static_cast<float>(badgeH), 4.0f * m_scale);
            Gdiplus::SolidBrush badgeBg(Gdiplus::Color(210, 20, 24, 32));
            Gdiplus::SolidBrush badgeTxt(Gdiplus::Color(255, 240, 245, 255));
            g.FillPath(&badgeBg, &bp);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(szText, -1, &font,
                         Gdiplus::RectF(static_cast<float>(bx), static_cast<float>(by),
                                        static_cast<float>(badgeW), static_cast<float>(badgeH)), &sf, &badgeTxt);
        }

        // 5. 浮动工具栏与二级属性栏
        if (m_phase == Phase::Editing && RectWidth(m_toolbarRect) > 0) {
            DrawToolbar(g);
        }

        // 6. 像素放大镜（悬停 / 拖拽选区 / 调整手柄时显示）
        bool showMag = (m_phase == Phase::Hovering) ||
                       (m_phase == Phase::Selecting) ||
                       (m_dragMode >= DragMode::ResizeTL && m_dragMode <= DragMode::ResizeL);
        if (showMag) {
            DrawMagnifier(g, m_screen, m_cursorLocal, m_scale);
        }

        // 7. 顶部轻量提示条（仅悬停态或临时 Toast）
        if (m_phase == Phase::Hovering || !m_toastText.empty()) {
            std::wstring tip = !m_toastText.empty()
                ? m_toastText
                : L"拖拽框选区域  ·  单击选中窗口  ·  按 C 复制色值  ·  右键 / ESC 退出";
            DrawTopHintBanner(g, tip);
        }

        // 一次性翻页到屏幕（零闪烁）
        HDC hdc = GetDC(m_hwnd);
        BitBlt(hdc, 0, 0, m_screen.vw, m_screen.vh, m_backDC, 0, 0, SRCCOPY);
        ReleaseDC(m_hwnd, hdc);
    }

    void DrawTopHintBanner(Gdiplus::Graphics& g, const std::wstring& text) {
        // 定位到当前鼠标所在显示器的顶部居中
        POINT screenCur = { m_screen.vx + m_cursorLocal.x, m_screen.vy + m_cursorLocal.y };
        HMONITOR hMon = MonitorFromPoint(screenCur, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);
        int monCenterX = (mi.rcMonitor.left + mi.rcMonitor.right) / 2 - m_screen.vx;
        int monTopY = mi.rcMonitor.top - m_screen.vy + static_cast<int>(16 * m_scale);

        Gdiplus::FontFamily ff(L"Microsoft YaHei UI");
        Gdiplus::Font font(&ff, 12.5f * m_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::RectF mr;
        g.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &mr);

        float w = mr.Width + 28 * m_scale;
        float h = mr.Height + 12 * m_scale;
        float x = monCenterX - w * 0.5f;
        float y = static_cast<float>(monTopY);

        Gdiplus::GraphicsPath path;
        AddRoundedRectPath(path, x, y, w, h, h * 0.5f);
        Gdiplus::SolidBrush bg(Gdiplus::Color(215, 22, 26, 36));
        Gdiplus::Pen border(Gdiplus::Color(80, 255, 255, 255), 1.0f);
        Gdiplus::SolidBrush txtBr(Gdiplus::Color(245, 241, 245, 249));
        g.FillPath(&bg, &path);
        g.DrawPath(&border, &path);

        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(text.c_str(), -1, &font, Gdiplus::RectF(x, y, w, h), &sf, &txtBr);
    }

    void DrawToolbar(Gdiplus::Graphics& g) {
        // 主工具栏圆角阴影背景
        auto drawBarBg = [&](const RECT& r) {
            float x = static_cast<float>(r.left), y = static_cast<float>(r.top);
            float w = static_cast<float>(RectWidth(r)), h = static_cast<float>(RectHeight(r));
            float rad = 7.0f * m_scale;

            Gdiplus::GraphicsPath shPath;
            AddRoundedRectPath(shPath, x + 1, y + 3, w, h, rad);
            Gdiplus::SolidBrush shBr(Gdiplus::Color(100, 0, 0, 0));
            g.FillPath(&shBr, &shPath);

            Gdiplus::GraphicsPath bgPath;
            AddRoundedRectPath(bgPath, x, y, w, h, rad);
            Gdiplus::SolidBrush bgBr(Gdiplus::Color(242, 28, 33, 44));
            Gdiplus::Pen bdPen(Gdiplus::Color(90, 255, 255, 255), 1.0f);
            g.FillPath(&bgBr, &bgPath);
            g.DrawPath(&bdPen, &bgPath);
        };

        drawBarBg(m_toolbarRect);

        Gdiplus::Pen sepPen(Gdiplus::Color(70, 255, 255, 255), 1.0f);
        for (size_t i = 0; i < m_toolbarBtns.size(); ++i) {
            const auto& b = m_toolbarBtns[i];
            bool active = IsToolActive(b.action);
            bool hovered = (static_cast<int>(i) == m_hoverBtnIdx);
            bool disabled = (b.action == ToolbarAction::Undo && m_annotations.empty()) ||
                            (b.action == ToolbarAction::Redo && m_redoStack.empty());

            if (active || (hovered && !disabled)) {
                Gdiplus::GraphicsPath hp;
                AddRoundedRectPath(hp, static_cast<float>(b.rect.left), static_cast<float>(b.rect.top),
                                   static_cast<float>(RectWidth(b.rect)), static_cast<float>(RectHeight(b.rect)),
                                   5.0f * m_scale);
                Gdiplus::SolidBrush hbr(active
                    ? Gdiplus::Color(230, 37, 99, 235)
                    : Gdiplus::Color(90, 255, 255, 255));
                g.FillPath(&hbr, &hp);
            }

            Gdiplus::Color iconCol(255, 235, 240, 248);
            if (disabled) {
                iconCol = Gdiplus::Color(95, 160, 170, 185);
            } else if (b.action == ToolbarAction::CopyDone) {
                iconCol = Gdiplus::Color(255, 52, 211, 153);
            } else if (b.action == ToolbarAction::Cancel) {
                iconCol = Gdiplus::Color(255, 248, 113, 113);
            } else if (active) {
                iconCol = Gdiplus::Color(255, 255, 255, 255);
            }

            float cx = (b.rect.left + b.rect.right) * 0.5f;
            float cy = (b.rect.top + b.rect.bottom) * 0.5f;
            DrawToolbarIcon(g, b.action, cx, cy, m_scale, iconCol);

            if (b.isSeparatorAfter && i + 1 < m_toolbarBtns.size()) {
                float sx = static_cast<float>(b.rect.right) + 5.0f * m_scale;
                g.DrawLine(&sepPen, sx, b.rect.top + 5 * m_scale, sx, b.rect.bottom - 5 * m_scale);
            }
        }

        // 悬停按钮文字提示
        if (m_hoverBtnIdx >= 0 && m_hoverBtnIdx < static_cast<int>(m_toolbarBtns.size())) {
            const auto& hb = m_toolbarBtns[m_hoverBtnIdx];
            Gdiplus::FontFamily ff(L"Microsoft YaHei UI");
            Gdiplus::Font font(&ff, 11.5f * m_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::RectF mr;
            g.MeasureString(hb.tooltip, -1, &font, Gdiplus::PointF(0, 0), &mr);
            float tw = mr.Width + 14 * m_scale;
            float th = mr.Height + 6 * m_scale;
            float tx = (hb.rect.left + hb.rect.right) * 0.5f - tw * 0.5f;
            tx = (std::max)(6.0f, (std::min)(tx, static_cast<float>(m_screen.vw) - tw - 6.0f));
            float ty = static_cast<float>(m_toolbarRect.top) - th - 6 * m_scale;
            if (ty < 6.0f) ty = static_cast<float>(m_toolbarRect.bottom) + 6 * m_scale;

            Gdiplus::GraphicsPath tp;
            AddRoundedRectPath(tp, tx, ty, tw, th, 4.0f * m_scale);
            Gdiplus::SolidBrush tbg(Gdiplus::Color(235, 15, 18, 26));
            Gdiplus::SolidBrush ttxt(Gdiplus::Color(255, 240, 245, 255));
            g.FillPath(&tbg, &tp);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(hb.tooltip, -1, &font, Gdiplus::RectF(tx, ty, tw, th), &sf, &ttxt);
        }

        // 二级属性栏（粗细圆点 + 颜色色块）
        if (RectWidth(m_subBarRect) > 0) {
            drawBarBg(m_subBarRect);
            const float dotRadii[3] = { 3.0f * m_scale, 5.0f * m_scale, 7.2f * m_scale };
            for (int i = 0; i < static_cast<int>(m_sizeRects.size()); ++i) {
                const RECT& r = m_sizeRects[i];
                float cx = (r.left + r.right) * 0.5f;
                float cy = (r.top + r.bottom) * 0.5f;
                if (i == m_sizeIdx || i == m_hoverSizeIdx) {
                    Gdiplus::SolidBrush hl(i == m_sizeIdx
                        ? Gdiplus::Color(220, 37, 99, 235)
                        : Gdiplus::Color(70, 255, 255, 255));
                    float hr = 10.5f * m_scale;
                    g.FillEllipse(&hl, cx - hr, cy - hr, hr * 2, hr * 2);
                }
                Gdiplus::SolidBrush dotBr(Gdiplus::Color(255, 240, 245, 255));
                float dr = dotRadii[i];
                g.FillEllipse(&dotBr, cx - dr, cy - dr, dr * 2, dr * 2);
            }

            if (!m_colorRects.empty()) {
                float sx = static_cast<float>(m_sizeRects.back().right) + 7.0f * m_scale;
                g.DrawLine(&sepPen, sx, m_subBarRect.top + 8 * m_scale, sx, m_subBarRect.bottom - 8 * m_scale);

                for (int i = 0; i < static_cast<int>(m_colorRects.size()); ++i) {
                    const RECT& r = m_colorRects[i];
                    float cx = (r.left + r.right) * 0.5f;
                    float cy = (r.top + r.bottom) * 0.5f;
                    float cr = 7.2f * m_scale;

                    Gdiplus::SolidBrush cbr(ToGdipColor(kColorPresets[i].rgb));
                    g.FillEllipse(&cbr, cx - cr, cy - cr, cr * 2, cr * 2);

                    if (i == m_colorIdx) {
                        Gdiplus::Pen selP(Gdiplus::Color(255, 255, 255, 255), 2.0f);
                        float orad = cr + 2.0f * m_scale;
                        g.DrawEllipse(&selP, cx - orad, cy - orad, orad * 2, orad * 2);
                    } else if (i == m_hoverColorIdx) {
                        Gdiplus::Pen hovP(Gdiplus::Color(180, 255, 255, 255), 1.3f);
                        float orad = cr + 1.5f * m_scale;
                        g.DrawEllipse(&hovP, cx - orad, cy - orad, orad * 2, orad * 2);
                    }
                }
            }
        }
    }

    // ============================================================
    // 窗口消息处理
    // ============================================================
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(m_hwnd, &ps);
                if (m_backDC) {
                    BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
                           ps.rcPaint.right - ps.rcPaint.left,
                           ps.rcPaint.bottom - ps.rcPaint.top,
                           m_backDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                }
                EndPaint(m_hwnd, &ps);
                return 0;
            }

            case WM_CTLCOLOREDIT: {
                HDC hdcEdit = reinterpret_cast<HDC>(wParam);
                COLORREF fg = kColorPresets[m_colorIdx].rgb;
                int luma = (GetRValue(fg) * 299 + GetGValue(fg) * 587 + GetBValue(fg) * 114) / 1000;
                SetTextColor(hdcEdit, fg);
                if (luma < 90) {
                    SetBkColor(hdcEdit, RGB(245, 247, 250));
                    return reinterpret_cast<LRESULT>(m_editLightBg);
                } else {
                    SetBkColor(hdcEdit, RGB(28, 33, 44));
                    return reinterpret_cast<LRESULT>(m_editDarkBg);
                }
            }

            case WM_CAPTURECHANGED: {
                if (m_phase == Phase::Selecting) {
                    m_phase = Phase::Hovering;
                }
                m_dragMode = DragMode::None;
                m_isDrawing = false;
                return 0;
            }

            case WM_SETCURSOR: {
                if (LOWORD(lParam) == HTCLIENT) {
                    if (m_phase == Phase::Editing) {
                        if (PointInRect(m_cursorLocal, m_toolbarRect) || PointInRect(m_cursorLocal, m_subBarRect)) {
                            SetCursor(LoadCursorW(nullptr, IDC_HAND));
                            return TRUE;
                        }
                        if (m_tool == ToolType::Text && PointInRect(m_cursorLocal, m_selRect)) {
                            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                            return TRUE;
                        }
                        DragMode dm = (m_dragMode != DragMode::None) ? m_dragMode : HitTestSelection(m_cursorLocal);
                        if (dm != DragMode::None) {
                            SetCursor(GetCursorForDragMode(dm));
                            return TRUE;
                        }
                    }
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                    return TRUE;
                }
                break;
            }

            case WM_LBUTTONDOWN: {
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                m_cursorLocal = pt;

                if (m_phase == Phase::Hovering) {
                    m_phase = Phase::Selecting;
                    m_selectAnchor = pt;
                    m_selRect = { pt.x, pt.y, pt.x, pt.y };
                    SetCapture(m_hwnd);
                    Redraw();
                    return 0;
                }

                if (m_phase == Phase::Editing) {
                    // 优先判断工具栏点击
                    if (PointInRect(pt, m_toolbarRect)) {
                        for (const auto& b : m_toolbarBtns) {
                            if (PointInRect(pt, b.rect)) {
                                HandleToolbarClick(b.action);
                                return 0;
                            }
                        }
                        return 0;
                    }
                    // 判断二级属性栏点击
                    if (PointInRect(pt, m_subBarRect)) {
                        for (int i = 0; i < static_cast<int>(m_sizeRects.size()); ++i) {
                            if (PointInRect(pt, m_sizeRects[i])) {
                                m_sizeIdx = i;
                                if (m_textEdit) {
                                    // 同步更新正在输入的字号
                                    int fontPx = static_cast<int>(kFontPresets[m_sizeIdx] * m_scale);
                                    if (m_textFont) DeleteObject(m_textFont);
                                    m_textFont = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
                                    SendMessageW(m_textEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_textFont), TRUE);
                                    AutoResizeTextEdit();
                                }
                                Redraw();
                                return 0;
                            }
                        }
                        for (int i = 0; i < static_cast<int>(m_colorRects.size()); ++i) {
                            if (PointInRect(pt, m_colorRects[i])) {
                                m_colorIdx = i;
                                if (m_textEdit) InvalidateRect(m_textEdit, nullptr, TRUE);
                                Redraw();
                                return 0;
                            }
                        }
                        return 0;
                    }

                    // 点击输入框外部则提交文字
                    if (m_textEdit) {
                        CommitTextEdit();
                        Redraw();
                        if (m_tool == ToolType::Text && PointInRect(pt, m_selRect)) {
                            BeginTextEdit(pt);
                        }
                        return 0;
                    }

                    // 优先判断选区 8 手柄调整或拖动
                    DragMode dm = HitTestSelection(pt);
                    if (dm != DragMode::None) {
                        m_dragMode = dm;
                        m_dragStartPt = pt;
                        m_dragStartSel = m_selRect;
                        SetCapture(m_hwnd);
                        Redraw();
                        return 0;
                    }

                    // 选区内启动绘图工具
                    if (m_tool != ToolType::None && PointInRect(pt, m_selRect)) {
                        if (m_tool == ToolType::Text) {
                            BeginTextEdit(pt);
                            return 0;
                        }
                        if (m_tool == ToolType::Step) {
                            Annotation a;
                            a.type = ToolType::Step;
                            a.color = kColorPresets[m_colorIdx].rgb;
                            a.strokeWidth = kStrokePresets[m_sizeIdx];
                            a.stepNumber = m_stepCounter++;
                            a.start = pt;
                            m_annotations.push_back(std::move(a));
                            m_redoStack.clear();
                            Redraw();
                            return 0;
                        }

                        m_isDrawing = true;
                        m_currentDraw = Annotation{};
                        m_currentDraw.type = m_tool;
                        m_currentDraw.color = kColorPresets[m_colorIdx].rgb;
                        m_currentDraw.strokeWidth = (m_tool == ToolType::Mosaic)
                            ? kMosaicPresets[m_sizeIdx]
                            : kStrokePresets[m_sizeIdx];
                        m_currentDraw.start = pt;
                        m_currentDraw.end = pt;
                        if (m_tool == ToolType::Pen || m_tool == ToolType::Highlight || m_tool == ToolType::Mosaic) {
                            m_currentDraw.points.push_back(pt);
                        }
                        SetCapture(m_hwnd);
                        Redraw();
                        return 0;
                    }
                }
                return 0;
            }

            case WM_MOUSEMOVE: {
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                m_cursorLocal = pt;

                if (m_phase == Phase::Hovering) {
                    UpdateHoverWindow(pt);
                    Redraw();
                    return 0;
                }

                if (m_phase == Phase::Selecting) {
                    m_selRect = NormalizeRect(m_selectAnchor, pt);
                    Redraw();
                    return 0;
                }

                if (m_phase == Phase::Editing) {
                    if (m_dragMode != DragMode::None) {
                        int dx = pt.x - m_dragStartPt.x;
                        int dy = pt.y - m_dragStartPt.y;
                        RECT r = m_dragStartSel;
                        if (m_dragMode == DragMode::MoveSel) {
                            int w = RectWidth(r), h = RectHeight(r);
                            int nl = (std::max)(0, (std::min)(static_cast<int>(r.left + dx), m_screen.vw - w));
                            int nt = (std::max)(0, (std::min)(static_cast<int>(r.top + dy), m_screen.vh - h));
                            m_selRect = { nl, nt, nl + w, nt + h };
                        } else {
                            if (m_dragMode == DragMode::ResizeTL || m_dragMode == DragMode::ResizeL || m_dragMode == DragMode::ResizeBL)
                                r.left += dx;
                            if (m_dragMode == DragMode::ResizeTR || m_dragMode == DragMode::ResizeR || m_dragMode == DragMode::ResizeBR)
                                r.right += dx;
                            if (m_dragMode == DragMode::ResizeTL || m_dragMode == DragMode::ResizeT || m_dragMode == DragMode::ResizeTR)
                                r.top += dy;
                            if (m_dragMode == DragMode::ResizeBL || m_dragMode == DragMode::ResizeB || m_dragMode == DragMode::ResizeBR)
                                r.bottom += dy;

                            RECT nr = NormalizeRect({ r.left, r.top }, { r.right, r.bottom });
                            nr.left   = (std::max)(0L, nr.left);
                            nr.top    = (std::max)(0L, nr.top);
                            nr.right  = (std::min)(static_cast<LONG>(m_screen.vw), nr.right);
                            nr.bottom = (std::min)(static_cast<LONG>(m_screen.vh), nr.bottom);
                            if (RectWidth(nr) >= 4 && RectHeight(nr) >= 4) {
                                m_selRect = nr;
                            }
                        }
                        Redraw();
                        return 0;
                    }

                    if (m_isDrawing) {
                        POINT clamped = {
                            (std::max)(m_selRect.left, (std::min)(pt.x, m_selRect.right)),
                            (std::max)(m_selRect.top,  (std::min)(pt.y, m_selRect.bottom))
                        };
                        if (m_currentDraw.type == ToolType::Pen ||
                            m_currentDraw.type == ToolType::Highlight ||
                            m_currentDraw.type == ToolType::Mosaic) {
                            if (m_currentDraw.points.empty() ||
                                std::abs(clamped.x - m_currentDraw.points.back().x) >= 2 ||
                                std::abs(clamped.y - m_currentDraw.points.back().y) >= 2) {
                                m_currentDraw.points.push_back(clamped);
                            }
                        } else {
                            m_currentDraw.end = clamped;
                        }
                        Redraw();
                        return 0;
                    }

                    // 更新工具栏按钮悬停高亮
                    int prevBtn = m_hoverBtnIdx, prevCol = m_hoverColorIdx, prevSz = m_hoverSizeIdx;
                    m_hoverBtnIdx = -1;
                    m_hoverColorIdx = -1;
                    m_hoverSizeIdx = -1;
                    if (PointInRect(pt, m_toolbarRect)) {
                        for (int i = 0; i < static_cast<int>(m_toolbarBtns.size()); ++i) {
                            if (PointInRect(pt, m_toolbarBtns[i].rect)) {
                                m_hoverBtnIdx = i;
                                break;
                            }
                        }
                    } else if (PointInRect(pt, m_subBarRect)) {
                        for (int i = 0; i < static_cast<int>(m_sizeRects.size()); ++i) {
                            if (PointInRect(pt, m_sizeRects[i])) { m_hoverSizeIdx = i; break; }
                        }
                        for (int i = 0; i < static_cast<int>(m_colorRects.size()); ++i) {
                            if (PointInRect(pt, m_colorRects[i])) { m_hoverColorIdx = i; break; }
                        }
                    }
                    if (prevBtn != m_hoverBtnIdx || prevCol != m_hoverColorIdx || prevSz != m_hoverSizeIdx) {
                        Redraw();
                    }
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                m_cursorLocal = pt;

                if (m_phase == Phase::Selecting) {
                    ReleaseCapture();
                    RECT r = NormalizeRect(m_selectAnchor, pt);
                    if (RectWidth(r) < 6 && RectHeight(r) < 6) {
                        // 单击未拖拽：直接选中当前悬停的窗口
                        m_selRect = m_hoverWinRect;
                    } else {
                        m_selRect = r;
                    }
                    if (RectWidth(m_selRect) >= 4 && RectHeight(m_selRect) >= 4) {
                        m_phase = Phase::Editing;
                    } else {
                        m_phase = Phase::Hovering;
                    }
                    Redraw();
                    return 0;
                }

                if (m_dragMode != DragMode::None) {
                    ReleaseCapture();
                    m_dragMode = DragMode::None;
                    Redraw();
                    return 0;
                }

                if (m_isDrawing) {
                    ReleaseCapture();
                    m_isDrawing = false;
                    bool valid = false;
                    if (m_currentDraw.type == ToolType::Pen ||
                        m_currentDraw.type == ToolType::Highlight ||
                        m_currentDraw.type == ToolType::Mosaic) {
                        valid = !m_currentDraw.points.empty();
                    } else {
                        valid = (std::abs(m_currentDraw.end.x - m_currentDraw.start.x) >= 3 ||
                                 std::abs(m_currentDraw.end.y - m_currentDraw.start.y) >= 3);
                    }
                    if (valid) {
                        m_annotations.push_back(std::move(m_currentDraw));
                        m_redoStack.clear();
                    }
                    Redraw();
                    return 0;
                }
                return 0;
            }

            case WM_LBUTTONDBLCLK: {
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                if (m_phase == Phase::Editing && PointInRect(pt, m_selRect) &&
                    !PointInRect(pt, m_toolbarRect) && !PointInRect(pt, m_subBarRect) &&
                    m_tool != ToolType::Text) {
                    ActionCopyAndFinish();
                    return 0;
                }
                return 0;
            }

            case WM_RBUTTONUP: {
                if (m_textEdit) {
                    CancelTextEdit();
                    Redraw();
                    return 0;
                }
                if (m_phase == Phase::Editing) {
                    // 微信/QQ 肌肉记忆：选区后右键一次取消选区重选，未选区时右键直接退出
                    m_phase = Phase::Hovering;
                    m_tool = ToolType::None;
                    m_annotations.clear();
                    m_redoStack.clear();
                    m_stepCounter = 1;
                    UpdateHoverWindow(m_cursorLocal);
                    Redraw();
                    return 0;
                }
                Close();
                return 0;
            }

            case WM_KEYDOWN: {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                if (wParam == VK_ESCAPE) {
                    if (m_textEdit) {
                        CancelTextEdit();
                        Redraw();
                    } else {
                        Close();
                    }
                    return 0;
                }
                if (wParam == VK_RETURN && m_phase == Phase::Editing && !m_textEdit) {
                    ActionCopyAndFinish();
                    return 0;
                }
                if (ctrl && wParam == 'Z') {
                    ActionUndo();
                    return 0;
                }
                if (ctrl && wParam == 'Y') {
                    ActionRedo();
                    return 0;
                }
                if (ctrl && wParam == 'C') {
                    if (m_phase == Phase::Editing) ActionCopyAndFinish();
                    return 0;
                }
                if (ctrl && wParam == 'S') {
                    if (m_phase == Phase::Editing) ActionSaveAndFinish();
                    return 0;
                }
                if (wParam == 'C' && !ctrl && m_phase == Phase::Hovering) {
                    uint32_t px = m_screen.GetPixelSafe(m_cursorLocal.x, m_cursorLocal.y);
                    wchar_t hex[32] = {};
                    swprintf_s(hex, L"#%02X%02X%02X", (px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
                    CopyTextToClipboard(m_hwnd, hex);
                    m_toastText = std::wstring(L"已复制色值 ") + hex + L" 到剪贴板";
                    SetTimer(m_hwnd, 2, 1500, nullptr);
                    Redraw();
                    return 0;
                }
                if (wParam == 'T' && !ctrl && m_phase == Phase::Editing && !m_textEdit) {
                    ActionPinAndFinish();
                    return 0;
                }
                if (wParam == 'L' && !ctrl && m_phase == Phase::Editing && !m_textEdit) {
                    ActionStartScrollCapture();
                    return 0;
                }
                // 方向键微调选区位置
                if (m_phase == Phase::Editing && !m_textEdit &&
                    (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN)) {
                    int step = shift ? 10 : 1;
                    int dx = (wParam == VK_LEFT) ? -step : (wParam == VK_RIGHT ? step : 0);
                    int dy = (wParam == VK_UP) ? -step : (wParam == VK_DOWN ? step : 0);
                    int w = RectWidth(m_selRect), h = RectHeight(m_selRect);
                    int nl = (std::max)(0, (std::min)(static_cast<int>(m_selRect.left + dx), m_screen.vw - w));
                    int nt = (std::max)(0, (std::min)(static_cast<int>(m_selRect.top + dy), m_screen.vh - h));
                    m_selRect = { nl, nt, nl + w, nt + h };
                    Redraw();
                    return 0;
                }
                return 0;
            }

            case WM_TIMER:
                if (wParam == 2) {
                    KillTimer(m_hwnd, 2);
                    m_toastText.clear();
                    Redraw();
                }
                return 0;

            case WM_DESTROY:
                CancelTextEdit();
                FreeBackBuffer();
                m_hwnd = nullptr;
                return 0;
        }
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        CaptureOverlay* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<CaptureOverlay*>(cs->lpCreateParams);
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->HandleMessage(msg, wParam, lParam)
                    : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HINSTANCE m_hInst = nullptr;
    ScrollCaptureSession* m_scrollSession = nullptr;
    HWND m_hwnd = nullptr;
    ScreenCaptureData m_screen;
    HDC m_backDC = nullptr;
    HBITMAP m_backBmp = nullptr;
    HGDIOBJ m_backOld = nullptr;
    HDC m_srcDC = nullptr;
    HGDIOBJ m_srcOld = nullptr;

    float m_scale = 1.0f;
    Phase m_phase = Phase::Hovering;
    DragMode m_dragMode = DragMode::None;
    POINT m_cursorLocal = {};
    POINT m_selectAnchor = {};
    POINT m_dragStartPt = {};
    RECT m_dragStartSel = {};
    RECT m_hoverWinRect = {};
    RECT m_selRect = {};

    ToolType m_tool = ToolType::None;
    int m_colorIdx = 0; // 默认红色
    int m_sizeIdx = 1;  // 默认中等粗细/字号
    int m_stepCounter = 1;
    bool m_isDrawing = false;
    Annotation m_currentDraw;
    std::vector<Annotation> m_annotations;
    std::vector<Annotation> m_redoStack;

    RECT m_toolbarRect = {};
    RECT m_subBarRect = {};
    std::vector<ToolbarBtn> m_toolbarBtns;
    std::vector<RECT> m_sizeRects;
    std::vector<RECT> m_colorRects;
    int m_hoverBtnIdx = -1;
    int m_hoverColorIdx = -1;
    int m_hoverSizeIdx = -1;

    HWND m_textEdit = nullptr;
    HFONT m_textFont = nullptr;
    HBRUSH m_editDarkBg = nullptr;
    HBRUSH m_editLightBg = nullptr;
    WNDPROC m_origEditProc = nullptr;
    POINT m_textOrigin = {};
    std::wstring m_toastText;
};

} // namespace snap
