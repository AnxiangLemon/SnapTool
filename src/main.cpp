#include "common.h"
#include "renderer.h"
#include "pin_window.h"
#include "capture_overlay.h"

#include <memory>

namespace {

static constexpr const wchar_t* kAppTitle      = L"SnapTool 桌面截图工具";
static constexpr const wchar_t* kHostWndClass  = L"SnapTool_NativeHostWnd";
static constexpr const wchar_t* kMutexName     = L"Local\\SnapTool_Native_SingleInstance";
static constexpr const wchar_t* kWakeupMsgName = L"SnapTool_Native_TriggerCapture";
static constexpr const wchar_t* kRunRegKey     = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr const wchar_t* kRunRegValue   = L"SnapToolNative";

enum : UINT {
    WM_TRAYICON   = WM_APP + 100,
    WM_DO_CAPTURE = WM_APP + 101,
};

enum : UINT_PTR {
    TIMER_DELAY_CAPTURE = 1001,
};

enum : int {
    HOTKEY_ID = 1,
};

enum : UINT {
    IDM_CAPTURE_NOW      = 2001,
    IDM_CAPTURE_DELAY_3S = 2002,
    IDM_CAPTURE_DELAY_5S = 2003,
    IDM_CLOSE_ALL_PINS   = 2010,
    IDM_HK_CTRL_ALT_A    = 2101,
    IDM_HK_ALT_A         = 2102,
    IDM_HK_F1            = 2103,
    IDM_HK_CTRL_SHIFT_A  = 2104,
    IDM_HK_PRINTSCREEN   = 2105,
    IDM_AUTOSTART        = 2201,
    IDM_ABOUT            = 2202,
    IDM_EXIT             = 2999,
};

struct HotkeyPreset {
    UINT menuId;
    UINT modifiers;
    UINT vk;
    const wchar_t* label;
    const wchar_t* iniKey;
};

static constexpr HotkeyPreset kHotkeyPresets[] = {
    { IDM_HK_CTRL_ALT_A,   MOD_CONTROL | MOD_ALT,   'A',         L"Ctrl + Alt + A",   L"ctrl_alt_a" },
    { IDM_HK_ALT_A,        MOD_ALT,                 'A',         L"Alt + A",          L"alt_a" },
    { IDM_HK_F1,           0,                       VK_F1,       L"F1",               L"f1" },
    { IDM_HK_CTRL_SHIFT_A, MOD_CONTROL | MOD_SHIFT, 'A',         L"Ctrl + Shift + A", L"ctrl_shift_a" },
    { IDM_HK_PRINTSCREEN,  0,                       VK_SNAPSHOT, L"PrintScreen",      L"printscreen" },
};

HWND g_hostWnd = nullptr;
HICON g_trayIcon = nullptr;
NOTIFYICONDATAW g_nid = {};
UINT g_wakeupMsg = 0;
UINT g_taskbarCreatedMsg = 0;
int g_hotkeyIdx = 0;
std::wstring g_configPath;
std::unique_ptr<snap::CaptureOverlay> g_overlay;

// ============================================================
// 配置与开机自启
// ============================================================
std::wstring GetConfigPath() {
    wchar_t base[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        std::wstring dir = std::wstring(base) + L"\\SnapTool";
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        return dir + L"\\config.ini";
    }
    return L"SnapTool.ini";
}

void LoadConfig() {
    wchar_t key[64] = {};
    GetPrivateProfileStringW(L"Settings", L"Hotkey", L"ctrl_alt_a", key, _countof(key), g_configPath.c_str());
    g_hotkeyIdx = 0;
    for (int i = 0; i < static_cast<int>(_countof(kHotkeyPresets)); ++i) {
        if (_wcsicmp(key, kHotkeyPresets[i].iniKey) == 0) {
            g_hotkeyIdx = i;
            break;
        }
    }
}

void SaveConfig() {
    WritePrivateProfileStringW(L"Settings", L"Hotkey", kHotkeyPresets[g_hotkeyIdx].iniKey, g_configPath.c_str());
}

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    wchar_t val[MAX_PATH * 2] = {};
    DWORD sz = sizeof(val);
    bool exists = (RegQueryValueExW(hKey, kRunRegValue, nullptr, nullptr, reinterpret_cast<BYTE*>(val), &sz) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

void ToggleAutoStart() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_SET_VALUE | KEY_READ, &hKey) != ERROR_SUCCESS) return;
    if (IsAutoStartEnabled()) {
        RegDeleteValueW(hKey, kRunRegValue);
    } else {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\"";
        RegSetValueExW(hKey, kRunRegValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(hKey);
}

// ============================================================
// 动态绘制高清 32x32 蓝色取景框托盘图标（零外部资源）
// ============================================================
HICON CreateAppIcon() {
    const int W = 32, H = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = W;
    bi.bV5Height = -H;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    uint32_t* pixels = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                      DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !pixels) return LoadIconW(nullptr, IDI_APPLICATION);

    memset(pixels, 0, W * H * sizeof(uint32_t));
    auto put = [&](int x, int y, uint32_t argb) {
        if (x >= 0 && x < W && y >= 0 && y < H) pixels[y * W + x] = argb;
    };

    for (int y = 2; y < H - 2; ++y) {
        for (int x = 2; x < W - 2; ++x) {
            int dx = (x < 7) ? (7 - x) : (x > 24 ? x - 24 : 0);
            int dy = (y < 7) ? (7 - y) : (y > 24 ? y - 24 : 0);
            if (dx * dx + dy * dy <= 25) {
                int r = 20 + (y * 16) / H;
                int g = 128 + (y * 36) / H;
                int b = 250 - (y * 20) / H;
                put(x, y, 0xFF000000u | (r << 16) | (g << 8) | b);
            }
        }
    }

    const uint32_t kWhite = 0xFFFFFFFFu;
    for (int i = 0; i < 6; ++i) {
        for (int t = 0; t < 2; ++t) {
            put(7 + i, 7 + t, kWhite);  put(7 + t, 7 + i, kWhite);
            put(24 - i, 7 + t, kWhite); put(24 - t, 7 + i, kWhite);
            put(7 + i, 24 - t, kWhite); put(7 + t, 24 - i, kWhite);
            put(24 - i, 24 - t, kWhite); put(24 - t, 24 - i, kWhite);
        }
    }
    for (int i = -3; i <= 3; ++i) {
        put(16 + i, 15, kWhite); put(16 + i, 16, kWhite);
        put(15, 16 + i, kWhite); put(16, 16 + i, kWhite);
    }

    HBITMAP hMask = CreateBitmap(W, H, 1, 1, nullptr);
    ICONINFO ii = { TRUE, 0, 0, hMask, hColor };
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hColor);
    DeleteObject(hMask);
    return hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
}

// ============================================================
// 托盘提示与全局热键
// ============================================================
void AddTrayIcon() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hostWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_trayIcon;
    std::wstring tip = std::wstring(kAppTitle) + L"\n快捷键: " + kHotkeyPresets[g_hotkeyIdx].label;
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void UpdateTrayTooltip() {
    std::wstring tip = std::wstring(kAppTitle) + L"\n快捷键: " + kHotkeyPresets[g_hotkeyIdx].label;
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void ShowTrayBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool ApplyHotkey(int idx, bool notify) {
    if (idx < 0 || idx >= static_cast<int>(_countof(kHotkeyPresets))) return false;
    UnregisterHotKey(g_hostWnd, HOTKEY_ID);

    const auto& p = kHotkeyPresets[idx];
    if (!RegisterHotKey(g_hostWnd, HOTKEY_ID, p.modifiers | MOD_NOREPEAT, p.vk)) {
        const auto& old = kHotkeyPresets[g_hotkeyIdx];
        RegisterHotKey(g_hostWnd, HOTKEY_ID, old.modifiers | MOD_NOREPEAT, old.vk);
        if (notify) {
            std::wstring msg = std::wstring(L"快捷键 ") + p.label + L" 已被其他程序占用，请更换其他组合。";
            ShowTrayBalloon(kAppTitle, msg.c_str());
        }
        return false;
    }

    g_hotkeyIdx = idx;
    SaveConfig();
    UpdateTrayTooltip();
    if (notify) {
        std::wstring msg = std::wstring(L"截图快捷键已切换为：") + p.label;
        ShowTrayBalloon(kAppTitle, msg.c_str());
    }
    return true;
}

void ShowTrayMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    HMENU hHotkeys = CreatePopupMenu();

    std::wstring capLabel = std::wstring(L"立即截图 (") + kHotkeyPresets[g_hotkeyIdx].label + L")";
    AppendMenuW(hMenu, MF_STRING, IDM_CAPTURE_NOW, capLabel.c_str());
    SetMenuDefaultItem(hMenu, IDM_CAPTURE_NOW, FALSE);
    AppendMenuW(hMenu, MF_STRING, IDM_CAPTURE_DELAY_3S, L"延时 3 秒截图");
    AppendMenuW(hMenu, MF_STRING, IDM_CAPTURE_DELAY_5S, L"延时 5 秒截图");

    int pinCount = snap::PinWindow::GetCount();
    if (pinCount > 0) {
        wchar_t pinBuf[64] = {};
        swprintf_s(pinBuf, L"关闭所有贴图 (%d)", pinCount);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CLOSE_ALL_PINS, pinBuf);
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < static_cast<int>(_countof(kHotkeyPresets)); ++i) {
        UINT flags = MF_STRING | (i == g_hotkeyIdx ? MF_CHECKED : 0);
        AppendMenuW(hHotkeys, flags, kHotkeyPresets[i].menuId, kHotkeyPresets[i].label);
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hHotkeys), L"快捷键设置");

    UINT autoFlags = MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0);
    AppendMenuW(hMenu, autoFlags, IDM_AUTOSTART, L"开机自动启动");
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"关于 SnapTool");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(hHotkeys);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_wakeupMsg && g_wakeupMsg != 0) {
        PostMessageW(hwnd, WM_DO_CAPTURE, 0, 0);
        return 0;
    }
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg != 0) {
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
        case WM_HOTKEY:
            if (wParam == HOTKEY_ID) {
                PostMessageW(hwnd, WM_DO_CAPTURE, 0, 0);
            }
            return 0;

        case WM_DO_CAPTURE:
            if (g_overlay) g_overlay->Start();
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_DELAY_CAPTURE) {
                KillTimer(hwnd, TIMER_DELAY_CAPTURE);
                if (g_overlay) g_overlay->Start();
            }
            return 0;

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_COMMAND: {
            UINT cmd = LOWORD(wParam);
            if (cmd == IDM_CAPTURE_NOW) {
                SetTimer(hwnd, TIMER_DELAY_CAPTURE, 180, nullptr);
            } else if (cmd == IDM_CAPTURE_DELAY_3S) {
                SetTimer(hwnd, TIMER_DELAY_CAPTURE, 3000, nullptr);
            } else if (cmd == IDM_CAPTURE_DELAY_5S) {
                SetTimer(hwnd, TIMER_DELAY_CAPTURE, 5000, nullptr);
            } else if (cmd == IDM_CLOSE_ALL_PINS) {
                snap::PinWindow::CloseAll();
            } else if (cmd == IDM_AUTOSTART) {
                ToggleAutoStart();
            } else if (cmd == IDM_ABOUT) {
                std::wstring msg =
                    std::wstring(L"SnapTool 桌面截图工具 v") + snap::kAppVersion + L"\n\n"
                    L"作者：AnxiangLemon\n"
                    L"项目：" + snap::kRepoUrl + L"\n"
                    L"特性：纯原生 Win32/GDI+ 单文件，零外部依赖\n\n"
                    L"常用快捷键：\n"
                    L"  " + std::wstring(kHotkeyPresets[g_hotkeyIdx].label) + L" — 开始截图\n"
                    L"  Enter / 双击选区 — 完成并复制\n"
                    L"  T — 贴图到桌面\n"
                    L"  Ctrl + S — 另存为图片\n"
                    L"  C（悬停态）— 复制当前像素色值\n"
                    L"  Esc / 右键 — 取消或退出";
                MessageBoxW(hwnd, msg.c_str(), L"关于 SnapTool", MB_OK | MB_ICONINFORMATION);
            } else if (cmd == IDM_EXIT) {
                DestroyWindow(hwnd);
            } else {
                for (int i = 0; i < static_cast<int>(_countof(kHotkeyPresets)); ++i) {
                    if (cmd == kHotkeyPresets[i].menuId) {
                        ApplyHotkey(i, true);
                        break;
                    }
                }
            }
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_DELAY_CAPTURE);
            UnregisterHotKey(hwnd, HOTKEY_ID);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_overlay) g_overlay->Close();
            snap::PinWindow::CloseAll();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================
// 命令行无交互端到端自测（--selftest）：验证截屏、全部标注渲染、PNG编码与贴图创建
// ============================================================
int RunSelfTest(HINSTANCE hInst) {
    snap::ScreenCaptureData screen;
    if (!snap::CaptureVirtualScreen(screen, nullptr)) return 10;
    if (screen.vw <= 0 || screen.vh <= 0 || screen.pixels.empty()) return 11;

    RECT sel = { 0, 0, (std::min)(400, screen.vw), (std::min)(300, screen.vh) };
    std::vector<snap::Annotation> anns;

    snap::Annotation aRect;
    aRect.type = snap::ToolType::Rect; aRect.start = { 20, 20 }; aRect.end = { 160, 120 };
    anns.push_back(aRect);

    snap::Annotation aEll;
    aEll.type = snap::ToolType::Ellipse; aEll.color = RGB(59, 130, 246); aEll.start = { 180, 20 }; aEll.end = { 320, 110 };
    anns.push_back(aEll);

    snap::Annotation aArr;
    aArr.type = snap::ToolType::Arrow; aArr.color = RGB(34, 197, 94); aArr.start = { 40, 180 }; aArr.end = { 220, 140 };
    anns.push_back(aArr);

    snap::Annotation aPen;
    aPen.type = snap::ToolType::Pen; aPen.points = { { 30, 220 }, { 80, 200 }, { 130, 240 } };
    anns.push_back(aPen);

    snap::Annotation aHi;
    aHi.type = snap::ToolType::Highlight; aHi.color = RGB(250, 204, 21); aHi.points = { { 150, 220 }, { 280, 220 } };
    anns.push_back(aHi);

    snap::Annotation aMos;
    aMos.type = snap::ToolType::Mosaic; aMos.strokeWidth = 12; aMos.points = { { 240, 160 }, { 340, 200 } };
    anns.push_back(aMos);

    snap::Annotation aStep;
    aStep.type = snap::ToolType::Step; aStep.stepNumber = 1; aStep.start = { 60, 60 };
    anns.push_back(aStep);

    snap::Annotation aTxt;
    aTxt.type = snap::ToolType::Text; aTxt.start = { 80, 250 }; aTxt.fontSize = 20; aTxt.text = L"SnapTool 自测OK";
    anns.push_back(aTxt);

    auto bmp = snap::RenderFinalBitmap(screen, sel, anns);
    if (!bmp || bmp->GetWidth() != static_cast<UINT>(snap::RectWidth(sel))) return 12;

    CLSID pngClsid = {};
    if (!snap::GetEncoderClsid(L"image/png", &pngClsid)) return 13;

    // 测试贴图创建与立即销毁
    auto* pin = snap::PinWindow::Create(hInst, std::move(bmp), 100, 100);
    if (!pin || snap::PinWindow::GetCount() != 1) return 14;
    snap::PinWindow::CloseAll();
    if (snap::PinWindow::GetCount() != 0) return 15;

    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    // Per-Monitor DPI V2 感知
    using FnSetDpiCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE hUser32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<FnSetDpiCtx>(reinterpret_cast<void*>(GetProcAddress(hUser32, "SetProcessDpiAwarenessContext")));
        if (fn) {
            fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
    }

    // 初始化 GDI+
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput gdipInput;
    if (Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr) != Gdiplus::Ok) {
        return 1;
    }

    if (lpCmdLine && wcsstr(lpCmdLine, L"--selftest")) {
        int rc = RunSelfTest(hInst);
        Gdiplus::GdiplusShutdown(gdipToken);
        return rc;
    }

    g_wakeupMsg = RegisterWindowMessageW(kWakeupMsgName);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // 单实例保护：已运行时再次双击直接拉起截图
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kHostWndClass, nullptr)) {
            PostMessageW(existing, g_wakeupMsg, 0, 0);
        }
        if (hMutex) CloseHandle(hMutex);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 0;
    }

    g_configPath = GetConfigPath();
    LoadConfig();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kHostWndClass;
    RegisterClassExW(&wc);

    g_hostWnd = CreateWindowExW(0, kHostWndClass, kAppTitle, 0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_hostWnd) {
        if (hMutex) CloseHandle(hMutex);
        Gdiplus::GdiplusShutdown(gdipToken);
        return 2;
    }

    g_overlay = std::make_unique<snap::CaptureOverlay>(hInst);
    g_trayIcon = CreateAppIcon();
    AddTrayIcon();

    bool hkOk = ApplyHotkey(g_hotkeyIdx, false);
    if (!hkOk) {
        for (int i = 0; i < static_cast<int>(_countof(kHotkeyPresets)); ++i) {
            if (i != g_hotkeyIdx && ApplyHotkey(i, false)) {
                hkOk = true;
                break;
            }
        }
    }

    if (hkOk) {
        std::wstring tip = std::wstring(L"已就绪，按 ") + kHotkeyPresets[g_hotkeyIdx].label + L" 开始截图。";
        ShowTrayBalloon(kAppTitle, tip.c_str());
    } else {
        ShowTrayBalloon(kAppTitle, L"默认快捷键均被占用，请点击托盘图标打开菜单切换快捷键。");
    }

    if (lpCmdLine && wcsstr(lpCmdLine, L"--capture")) {
        PostMessageW(g_hostWnd, WM_DO_CAPTURE, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_overlay.reset();
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    if (hMutex) CloseHandle(hMutex);
    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}
