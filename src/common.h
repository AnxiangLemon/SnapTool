#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace snap {

static constexpr const wchar_t* kAppVersion = L"1.1.1";
static constexpr const wchar_t* kRepoUrl    = L"https://github.com/AnxiangLemon/SnapTool";

// ============================================================
// 基础常量与工具类型
// ============================================================
enum class ToolType {
    None = 0,
    Rect,
    Ellipse,
    Arrow,
    Pen,
    Highlight,
    Mosaic,
    Step,
    Text
};

struct ColorPreset {
    COLORREF rgb;
    const wchar_t* name;
};

static constexpr ColorPreset kColorPresets[] = {
    { RGB(239,  68,  68), L"红色" },
    { RGB(249, 115,  22), L"橙色" },
    { RGB(250, 204,  21), L"黄色" },
    { RGB( 34, 197,  94), L"绿色" },
    { RGB( 59, 130, 246), L"蓝色" },
    { RGB(168,  85, 247), L"紫色" },
    { RGB(255, 255, 255), L"白色" },
    { RGB( 30,  41,  59), L"深灰" },
};

static constexpr int kStrokePresets[3] = { 2, 4, 7 };
static constexpr int kFontPresets[3]   = { 15, 20, 28 };
static constexpr int kMosaicPresets[3] = { 8, 14, 22 };

struct Annotation {
    ToolType type = ToolType::None;
    COLORREF color = RGB(239, 68, 68);
    int strokeWidth = 4;      // 线宽 / 马赛克块大小 / 画笔粗细
    int fontSize = 20;        // 文字字号
    int stepNumber = 1;       // 序号
    POINT start = { 0, 0 };   // 相对虚拟桌面左上角 (0, 0) 的坐标
    POINT end = { 0, 0 };
    std::vector<POINT> points;
    std::wstring text;
};

struct ScreenCaptureData {
    int vx = 0;               // 虚拟桌面左上角 X
    int vy = 0;               // 虚拟桌面左上角 Y
    int vw = 0;               // 虚拟桌面宽
    int vh = 0;               // 虚拟桌面高
    HBITMAP hBitmap = nullptr;
    std::vector<uint32_t> pixels; // 自顶向下 BGRA (0xAARRGGBB)
    std::vector<RECT> windowRects; // 转换到虚拟桌面本地坐标 (0..vw, 0..vh)

    void Reset() {
        if (hBitmap) {
            DeleteObject(hBitmap);
            hBitmap = nullptr;
        }
        pixels.clear();
        windowRects.clear();
        vx = vy = vw = vh = 0;
    }

    ~ScreenCaptureData() { Reset(); }

    uint32_t GetPixelSafe(int x, int y) const {
        if (pixels.empty() || vw <= 0 || vh <= 0) return 0xFF000000u;
        x = (std::max)(0, (std::min)(x, vw - 1));
        y = (std::max)(0, (std::min)(y, vh - 1));
        return pixels[static_cast<size_t>(y) * vw + x];
    }
};

// ============================================================
// GDI+ 辅助函数
// ============================================================
inline Gdiplus::Color ToGdipColor(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline RECT NormalizeRect(POINT a, POINT b) {
    RECT r;
    r.left   = (std::min)(a.x, b.x);
    r.top    = (std::min)(a.y, b.y);
    r.right  = (std::max)(a.x, b.x);
    r.bottom = (std::max)(a.y, b.y);
    return r;
}

inline int RectWidth(const RECT& r)  { return r.right - r.left; }
inline int RectHeight(const RECT& r) { return r.bottom - r.top; }

inline bool PointInRect(POINT pt, const RECT& r) {
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

inline bool GetEncoderClsid(const wchar_t* mimeType, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<uint8_t> buf(size);
    auto* pInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(num, size, pInfo) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(pInfo[i].MimeType, mimeType) == 0) {
            *pClsid = pInfo[i].Clsid;
            return true;
        }
    }
    return false;
}

// ============================================================
// 桌面截屏与窗口枚举
// ============================================================
inline bool IsWindowCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != FALSE;
    }
    return false;
}

inline bool CaptureVirtualScreen(ScreenCaptureData& out, HWND excludeHwnd = nullptr) {
    out.Reset();
    out.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    out.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    out.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    out.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (out.vw <= 0 || out.vh <= 0) return false;

    // 枚举当前所有可见顶层窗口（按 Z 序从顶到底）
    struct EnumCtx {
        ScreenCaptureData* data;
        HWND exclude;
    } ctx{ &out, excludeHwnd };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (hwnd == c->exclude) return TRUE;
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        if (IsWindowCloaked(hwnd)) return TRUE;

        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) {
            // 允许部分真实可见浮窗，但过滤极小/不可交互工具窗
            wchar_t cls[64] = {};
            GetClassNameW(hwnd, cls, 64);
            if (wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0) {
                return TRUE;
            }
        }

        RECT rc = {};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
            GetWindowRect(hwnd, &rc);
        }
        if (rc.right - rc.left < 16 || rc.bottom - rc.top < 16) return TRUE;

        // 转换到虚拟桌面本地坐标并裁剪到屏幕范围
        RECT local;
        local.left   = (std::max)(0L, rc.left - c->data->vx);
        local.top    = (std::max)(0L, rc.top - c->data->vy);
        local.right  = (std::min)(static_cast<LONG>(c->data->vw), rc.right - c->data->vx);
        local.bottom = (std::min)(static_cast<LONG>(c->data->vh), rc.bottom - c->data->vy);
        if (local.right - local.left >= 16 && local.bottom - local.top >= 16) {
            c->data->windowRects.push_back(local);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    // 截取完整虚拟桌面到 32bpp 自顶向下 DIBSection
    HDC hScreenDC = GetDC(nullptr);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = out.vw;
    bmi.bmiHeader.biHeight = -out.vh; // 自顶向下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDib = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hDib || !pBits) {
        DeleteDC(hMemDC);
        ReleaseDC(nullptr, hScreenDC);
        return false;
    }

    HGDIOBJ oldObj = SelectObject(hMemDC, hDib);
    BitBlt(hMemDC, 0, 0, out.vw, out.vh, hScreenDC, out.vx, out.vy, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    SelectObject(hMemDC, oldObj);

    // 缓存一份像素并确保 Alpha 全部为 0xFF
    size_t count = static_cast<size_t>(out.vw) * out.vh;
    out.pixels.resize(count);
    auto* raw = static_cast<uint32_t*>(pBits);
    for (size_t i = 0; i < count; ++i) {
        raw[i] |= 0xFF000000u;
        out.pixels[i] = raw[i];
    }

    out.hBitmap = hDib;
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);
    return true;
}

// ============================================================
// 剪贴板与文件保存
// ============================================================
inline bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* p = GlobalLock(hMem);
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
    return hMem != nullptr;
}

inline bool CopyGdipBitmapToClipboard(HWND owner, Gdiplus::Bitmap* bmp) {
    if (!bmp) return false;
    UINT w = bmp->GetWidth();
    UINT h = bmp->GetHeight();
    if (w == 0 || h == 0) return false;

    Gdiplus::Rect rc(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    Gdiplus::BitmapData bd = {};
    if (bmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) {
        return false;
    }

    // 使用标准自底向上 24bpp CF_DIB（所有 Windows 软件 100% 兼容，不会出现透明变黑）
    UINT stride24 = ((w * 3 + 3) & ~3u);
    size_t pixelBytes = static_cast<size_t>(stride24) * h;
    size_t totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
    if (!hMem) {
        bmp->UnlockBits(&bd);
        return false;
    }

    auto* dst = static_cast<uint8_t*>(GlobalLock(hMem));
    auto* hdr = reinterpret_cast<BITMAPINFOHEADER*>(dst);
    memset(hdr, 0, sizeof(BITMAPINFOHEADER));
    hdr->biSize = sizeof(BITMAPINFOHEADER);
    hdr->biWidth = static_cast<LONG>(w);
    hdr->biHeight = static_cast<LONG>(h); // 正数：自底向上
    hdr->biPlanes = 1;
    hdr->biBitCount = 24;
    hdr->biCompression = BI_RGB;
    hdr->biSizeImage = static_cast<DWORD>(pixelBytes);

    uint8_t* dstPixels = dst + sizeof(BITMAPINFOHEADER);
    const auto* srcBase = static_cast<const uint8_t*>(bd.Scan0);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* srcRow = srcBase + static_cast<size_t>(h - 1 - y) * bd.Stride;
        uint8_t* dstRow = dstPixels + static_cast<size_t>(y) * stride24;
        for (UINT x = 0; x < w; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 0]; // B
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // G
            dstRow[x * 3 + 2] = srcRow[x * 4 + 2]; // R
        }
        // 对齐填充清零
        for (UINT p = w * 3; p < stride24; ++p) dstRow[p] = 0;
    }

    GlobalUnlock(hMem);
    bmp->UnlockBits(&bd);

    if (!OpenClipboard(owner)) {
        GlobalFree(hMem);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

inline bool SaveGdipBitmapWithDialog(HWND owner, Gdiplus::Bitmap* bmp) {
    if (!bmp) return false;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t fileName[MAX_PATH] = {};
    swprintf_s(fileName, L"Screenshot_%04d%02d%02d_%02d%02d%02d.png",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    wchar_t initDir[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, initDir);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"PNG 无损图片 (*.png)\0*.png\0JPEG 图片 (*.jpg)\0*.jpg\0BMP 位图 (*.bmp)\0*.bmp\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initDir;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) return false;

    std::wstring path(fileName);
    std::wstring ext;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        ext = path.substr(dot);
        for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    }

    CLSID clsid = {};
    if (ext == L".jpg" || ext == L".jpeg") {
        if (!GetEncoderClsid(L"image/jpeg", &clsid)) return false;
        Gdiplus::EncoderParameters ep = {};
        ULONG quality = 95;
        ep.Count = 1;
        ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
        ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ep.Parameter[0].Value = &quality;
        return bmp->Save(path.c_str(), &clsid, &ep) == Gdiplus::Ok;
    } else if (ext == L".bmp") {
        if (!GetEncoderClsid(L"image/bmp", &clsid)) return false;
        return bmp->Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
    } else {
        if (!GetEncoderClsid(L"image/png", &clsid)) return false;
        return bmp->Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
    }
}

// ============================================================
// 获取系统窗口缩放比（按鼠标所在显示器）
// ============================================================
inline float GetDpiScaleForPoint(POINT screenPt) {
    HMONITOR hMon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    if (HMODULE hShcore = LoadLibraryW(L"shcore.dll")) {
        using FnGetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        auto fn = reinterpret_cast<FnGetDpi>(reinterpret_cast<void*>(GetProcAddress(hShcore, "GetDpiForMonitor")));
        UINT dx = 96, dy = 96;
        if (fn && SUCCEEDED(fn(hMon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy))) {
            FreeLibrary(hShcore);
            return static_cast<float>(dx) / 96.0f;
        }
        FreeLibrary(hShcore);
    }
    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

} // namespace snap
