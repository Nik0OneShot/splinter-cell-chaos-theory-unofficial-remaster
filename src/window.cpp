// Window mode and alt-tab.
//
// - Per-monitor DPI awareness: without it, a scaled desktop (125%/150%...) makes Windows report a
//   smaller "virtual" desktop size, so the game ran fullscreen at a lower mode and the desktop got
//   rearranged on every alt-tab.
// - Borderless fullscreen (default): the game always opens exclusive fullscreen and minimises when
//   it loses focus. Here it starts in its window mode instead, the window covers the monitor and the
//   device renders at the monitor's resolution, so alt-tab is instant.
// Window logic ported from ThirteenAG's FusionFix (MIT): UWindowsViewport::OpenWindow forces
// StartupFullscreen (+D8h) = 1; ResizeViewport positions the window with MoveWindow (skipped when
// the viewport actor's ShowFlags has SHOW_ChildWindow 0x200); UD3DRenderDevice::SetRes(viewport,
// w, h, fullscreen, a5) creates/resets the device.
#include "pch.h"
#include "features.h"

namespace
{
    enum { kFullscreen = 0, kWindowed = 1, kBorderless = 2 };

    using ViewportWndProc_t = int(__fastcall*)(void* vp, void* edx, UINT msg, WPARAM wp, LPARAM lp);
    using SetRes_t = int(__fastcall*)(uintptr_t device, void* edx, uintptr_t viewport, int w, int h, int fullscreen, int a5);
    ViewportWndProc_t oViewportWndProc = nullptr;
    SetRes_t oSetRes = nullptr;

    UINT g_currentMsg = 0;
    DWORD g_suppressInputUntil = 0;   // drop key/mouse-button messages until this tick (after a video skip)
    uint8_t g_resetOffset = 0;
    int g_lastW = 0, g_lastH = 0;

    HWND ViewportWindow(uintptr_t viewport)
    {
        auto window = viewport ? *reinterpret_cast<uintptr_t*>(viewport + 0x1F0) : 0;
        return window ? *reinterpret_cast<HWND*>(window + 4) : nullptr;
    }

    MONITORINFO Monitor(HWND window)
    {
        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoW(window ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &info);
        return info;
    }

    RECT Frame(HWND window, LONG style, LONG exStyle)
    {
        using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
        using AdjustForDpi_t = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        static auto user32 = GetModuleHandleW(L"user32.dll");
        static auto getDpi = reinterpret_cast<GetDpiForWindow_t>(GetProcAddress(user32, "GetDpiForWindow"));
        static auto adjust = reinterpret_cast<AdjustForDpi_t>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        RECT frame{};
        if (getDpi && adjust)
            adjust(&frame, style, FALSE, exStyle, getDpi(window));
        else
            AdjustWindowRectEx(&frame, style, FALSE, exStyle);
        return frame;
    }

    void SquareCorners(HWND window)
    {
        using SetAttr_t = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        static auto setAttr = []() -> SetAttr_t
        {
            auto dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return dwm ? reinterpret_cast<SetAttr_t>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
        }();
        int pref = 1; // DWMWA_WINDOW_CORNER_PREFERENCE (33) = DWMWCP_DONOTROUND
        if (setAttr)
            setAttr(window, 33, &pref, sizeof(pref));
    }

    void Place(HWND window, int width, int height)
    {
        if (IsIconic(window))
            ShowWindow(window, SW_RESTORE);
        const auto info = Monitor(window);
        LONG style = GetWindowLongW(window, GWL_STYLE) & ~(WS_MINIMIZE | WS_MAXIMIZE);
        LONG exStyle = GetWindowLongW(window, GWL_EXSTYLE);
        RECT rect = info.rcMonitor;
        if (g_settings.windowMode == kBorderless)
        {
            style = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
            exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
        }
        else
        {
            style = (style & ~(WS_POPUP | WS_MAXIMIZEBOX)) | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
            const RECT frame = Frame(window, style, exStyle);
            const RECT& work = info.rcWork;
            const RECT& area = info.rcMonitor;
            const int w = width + (frame.right - frame.left);
            const int h = height + (frame.bottom - frame.top);
            if (w <= work.right - work.left && h <= work.bottom - work.top)
            {
                rect.left = work.left + ((work.right - work.left) - w) / 2;
                rect.top = work.top + ((work.bottom - work.top) - h) / 2;
            }
            else
            {
                rect.left = area.left + ((area.right - area.left) - width) / 2 + frame.left;
                rect.top = area.top + ((area.bottom - area.top) - height) / 2 + frame.top;
            }
            rect.right = rect.left + w;
            rect.bottom = rect.top + h;
        }
        SetWindowLongW(window, GWL_STYLE, style);
        SetWindowLongW(window, GWL_EXSTYLE, exStyle);
        SetWindowPos(window, HWND_NOTOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        SquareCorners(window);
        LOG("Window: %s %dx%d at (%ld,%ld)", g_settings.windowMode == kBorderless ? "borderless" : "windowed",
            int(rect.right - rect.left), int(rect.bottom - rect.top), rect.left, rect.top);
    }

    // ResizeViewport, window path (esi = viewport, ebp/edi = width/height, ecx = viewport actor).
    void __cdecl OnResizeViewportWindow(Mem::Regs* r)
    {
        static uint8_t fakeActor[0x410] = {};
        *reinterpret_cast<uint32_t*>(fakeActor + 0x40C) = 0x200; // SHOW_ChildWindow: skip the game's MoveWindow
        r->ecx = reinterpret_cast<uint32_t>(fakeActor);
        if (g_currentMsg == WM_SIZE)
            return; // the player resized the window: leave it where it is
        const uintptr_t viewport = r->esi;
        if (HWND window = ViewportWindow(viewport))
        {
            Game::hWnd = window;
            ++*reinterpret_cast<int*>(viewport + 0x200); // the viewport's lock, so WM_SIZE doesn't set the resolution again
            Place(window, int(r->ebp), int(r->edi));
            --*reinterpret_cast<int*>(viewport + 0x200);
        }
    }

    int __fastcall hkSetRes(uintptr_t device, void* edx, uintptr_t viewport, int w, int h, int fullscreen, int a5)
    {
        if (!fullscreen && g_settings.windowMode == kBorderless)
        {
            const auto area = Monitor(ViewportWindow(viewport)).rcMonitor;
            w = area.right - area.left;
            h = area.bottom - area.top;
        }
        int result;
        if (!fullscreen && g_resetOffset && w > 0 && h > 0 && (w != g_lastW || h != g_lastH) && g_lastW)
        {
            // The windowed fast path would keep render targets of the previous size: request a full reset.
            auto request = reinterpret_cast<int*>(device + g_resetOffset);
            const int previous = *request;
            *request = 1;
            result = oSetRes(device, edx, viewport, w, h, fullscreen, a5);
            *request = previous;
        }
        else
            result = oSetRes(device, edx, viewport, w, h, fullscreen, a5);
        g_lastW = w; g_lastH = h;
        LOG("Window: SetRes %dx%d %s -> %d", w, h, fullscreen ? "fullscreen" : "windowed", result);
        return result;
    }

    // UWindowsViewport::SetMouseCapture(Capture, Clip, OnlyFocus) is vtable slot +ECh. The game releases
    // the mouse when focus goes to another process, but only captures it again on a click or key press
    // (both paths check +210h "captured" == 0 and Client +DCh == 0 first).
    void RecaptureMouse(void* vp)
    {
        auto v = static_cast<uint8_t*>(vp);
        if (*reinterpret_cast<int*>(v + 0x210) != 0)
            return; // already captured
        auto client = *reinterpret_cast<uint8_t**>(v + 0x18);
        if (!client || *reinterpret_cast<int*>(client + 0xDC) != 0)
            return;
        using SetMouseCapture_t = void(__fastcall*)(void* vp, void* edx, int capture, int clip, int onlyFocus);
        const auto setCapture = reinterpret_cast<SetMouseCapture_t>((*reinterpret_cast<void***>(vp))[0xEC / 4]);
        setCapture(vp, nullptr, 1, 1, 0);
        static int count = 0;
        if (++count <= 20)
            LOG("Window: mouse re-captured after the window was activated (%d)", count);
    }

    int __fastcall hkViewportWndProc(void* vp, void* edx, UINT msg, WPARAM wp, LPARAM lp)
    {
        // The press that skipped a video must not also act on the next screen.
        if (g_suppressInputUntil && ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_LBUTTONDOWN && msg <= WM_XBUTTONDBLCLK)))
        {
            if (int(GetTickCount() - g_suppressInputUntil) < 0)
                return 0;
            g_suppressInputUntil = 0;
        }
        if (g_settings.windowMode != kFullscreen)
        {
            // Alt+Enter would switch to exclusive fullscreen, which this build can't leave again.
            if (msg == WM_SYSCHAR && wp == VK_RETURN)
                return 0;
            if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))
                return 0;
        }
        const UINT last = g_currentMsg;
        g_currentMsg = msg;
        const int r = oViewportWndProc(vp, edx, msg, wp, lp);
        g_currentMsg = last;
        // Back from alt-tab: capture the mouse again right away instead of waiting for a click.
        if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE && !HIWORD(wp) && g_settings.recaptureMouse)
            RecaptureMouse(vp);
        if (msg == WM_GETMINMAXINFO && lp)
            reinterpret_cast<MINMAXINFO*>(lp)->ptMaxTrackSize = { GetSystemMetrics(SM_CXVIRTUALSCREEN) * 2, GetSystemMetrics(SM_CYVIRTUALSCREEN) * 2 };
        return r;
    }
}

void Window::SuppressInputFor(DWORD ms)
{
    g_suppressInputUntil = GetTickCount() + ms;
    if (!g_suppressInputUntil) g_suppressInputUntil = 1;
}

void Window::InitEarly()
{
    // Must happen before the game creates its window and before any desktop size is read.
    using SetContext_t = BOOL(WINAPI*)(HANDLE);
    BOOL ok = FALSE;
    if (auto set = reinterpret_cast<SetContext_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        ok = set(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (!ok)
        ok = SetProcessDPIAware();
    LOG("Window: DPI awareness %s, desktop %dx%d", ok ? "enabled" : "unchanged (already set)", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    // The startup videos play in a loop that doesn't process window messages. In exclusive fullscreen
    // nobody notices, but a (borderless) window gets "ghosted" by Windows after 5 s: the picture
    // freezes, clicks bring up "not responding" and the ghost takes the focus away from the game.
    DisableProcessWindowsGhosting();
    LOG("Window: window ghosting disabled");
}

void Window::Init()
{
    const int mode = std::clamp(g_settings.windowMode, 0, 2);
    static const char* names[] = { "exclusive fullscreen", "windowed", "borderless fullscreen" };
    LOG("Window: mode %s", names[mode]);

    if (auto a = Mem::Find("ViewportWndProc", "81 EC 8C 03 00 00"))
        Mem::Detour("ViewportWndProc", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkViewportWndProc), reinterpret_cast<void**>(&oViewportWndProc));

    if (mode == kFullscreen)
        return;

    // OpenWindow: StartupFullscreen = 1 -> start in the window mode.
    if (auto a = Mem::Find("startup fullscreen", "C7 85 D8 00 00 00 01 00 00 00 39 1D"))
        Mem::Write<uint32_t>(a + 6, 0);

    if (auto a = Mem::Find("resize viewport window", "FF 15 ? ? ? ? 8B 4E 30 8B 81 0C 04 00 00 F6 C4 02 75"))
        Mem::MidHook("resize viewport window", a + 9, 6, &OnResizeViewportWindow, Mem::MidMode::OriginalAfter);

    if (auto a = Mem::Find("device reset request", "8B 46 ? 85 C0 75 ? 8B 86 3C 4D 00 00 85 C0 74 ? 85 ED 75 ? 8B 86 B8 46 00 00"))
        g_resetOffset = Mem::Read<uint8_t>(a + 2);

    if (auto a = Mem::Find("UD3DRenderDevice::SetRes", "A1 ? ? ? ? 81 EC 80 00 00 00 85 C0 56 8B F1 75 39"))
        Mem::Detour("SetRes", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkSetRes), reinterpret_cast<void**>(&oSetRes));
}
