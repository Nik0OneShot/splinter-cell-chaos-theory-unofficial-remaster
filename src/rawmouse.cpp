// Raw mouse input.
//
// Vanilla (UseDIMouse=False): UWindowsViewport::ViewportWndProc drains WM_MOUSEMOVE, sends the
// pixel delta as IK_MouseX/IK_MouseY axis events and warps the cursor back to the window centre
// with SetCursorPos. With high polling rate mice the warp races real movement, so deltas get
// lost or doubled (erratic aim, "strange" mouse).
//
// Here a dedicated thread reads WM_INPUT on its own message-only window, so mouse polling
// never floods the game's message queue. Deltas are accumulated and injected once per frame at
// the start of UGameEngine::Tick, but only while the viewport has captured the mouse (exactly when
// vanilla would send axis events: fullscreen flag +204h bit 0, or capture count +218h != -1).
// The vanilla IK_MouseX/IK_MouseY axis events are dropped; the cursor warp is left alone.
//
// Menus: USCMagmaInteraction::ProcessInputAxisState adds every IK_MouseX/Y delta straight to its
// cursor in 640x480 units, so one unit is height/480 screen pixels. While the mouse is captured
// (in-game screens with a cursor: hacking, keypads...) the vanilla deltas come from the cursor warp
// above and are just as erratic (lost/doubled steps, and a one-sided drift), so that cursor gets the
// raw counts instead, scaled by the Windows pointer speed (SPI_GETMOUSESPEED, 10 = 1:1, no
// acceleration) and converted to menu units, so it tracks like the desktop cursor. Gameplay keeps the
// raw deltas at RawSensitivity.
#include "pch.h"
#include "features.h"
#include "perf.h"

namespace
{
    constexpr int kIK_MouseX = 0xE4;
    constexpr int kIK_MouseY = 0xE5;
    constexpr int kIST_Axis = 4;

    using CauseInputEvent_t = int(__fastcall*)(void* viewport, void* edx, int key, int action, float delta);
    using EngineTick_t = void(__fastcall*)(void* engine, void* edx, float dt);

    CauseInputEvent_t oCauseInputEvent = nullptr;
    EngineTick_t oEngineTick = nullptr;

    std::atomic<void*> g_viewport{ nullptr };
    std::atomic<long long> g_dx{ 0 }, g_dy{ 0 };
    std::atomic<bool> g_active{ false };
    bool g_injecting = false;
    DWORD g_pid = 0;

    float g_sysDx = 0.0f, g_sysDy = 0.0f;      // Windows cursor movement (vanilla deltas), game thread only
    float g_menuDx = 0.0f, g_menuDy = 0.0f;    // menu-unit deltas waiting for the Magma cursor

    // Windows pointer speed as a multiplier (Control Panel slider 1-20, 10 = 1:1), re-read every 2 s.
    float PointerSpeed()
    {
        static float factor = 1.0f;
        static DWORD next = 0;
        const DWORD now = GetTickCount();
        if (now >= next)
        {
            next = now + 2000;
            int speed = 10;
            if (SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0))
            {
                static const float kTable[21] = { 1.0f, 1 / 32.0f, 1 / 16.0f, 1 / 8.0f, 2 / 8.0f, 3 / 8.0f, 4 / 8.0f, 5 / 8.0f, 6 / 8.0f,
                                                   7 / 8.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f, 2.75f, 3.0f, 3.25f, 3.5f };
                factor = kTable[std::clamp(speed, 1, 20)];
            }
        }
        return factor;
    }

    bool Captured(void* vp)
    {
        const auto v = reinterpret_cast<const uint8_t*>(vp);
        return (v[0x204] & 1) || *reinterpret_cast<const int*>(v + 0x218) != -1;
    }

    float MenuScale()
    {
        RECT rc{};
        if (Game::hWnd && GetClientRect(Game::hWnd, &rc) && rc.bottom > 0)
            return 480.0f / float(rc.bottom) * g_settings.menuCursorSpeed;
        return g_settings.menuCursorSpeed;
    }

    // Menu cursor (Magma): replace this axis event's delta with the desktop-like menu delta.
    // The menu keeps its cursor in whole 640x480 units and truncates (cursor + delta), so small
    // movements would be dropped (slow vertical moves did nothing, diagonals snapped to straight lines).
    // Only whole units are handed over; the fraction is carried to the next event.
    float g_menuRemX = 0.0f, g_menuRemY = 0.0f;

    // [Debug] LogCursor: the Magma cursor (short 640x480 units at [esp+40h] / [esp+42h] in this function) is
    // checked against what we handed it last time; when it moved by something else (the game setting it), or one
    // step is unusually large, a [CURSOR] line says so with the time since the previous mouse event.
    struct CursorTrack { int last = -100000; int sent = 0; DWORD tick = 0; };
    CursorTrack g_track[2];
    int g_cursorLines = 0;
    void CheckCursor(int axis, int cur, float handed)
    {
        if (!g_settings.logCursor) return;
        CursorTrack& t = g_track[axis];
        const DWORD now = GetTickCount();
        const char* name = axis ? "Y" : "X";
        static bool s_loggedCaptured = false;
        void* vp = g_viewport.load();
        if (!s_loggedCaptured && vp && Captured(vp))
        {
            s_loggedCaptured = true;
            LOG("[CURSOR] an in-game cursor (mouse captured) is moving through the menu-cursor code, at %d", cur);
        }
        if (t.last > -100000 && now - t.tick < 1000 && g_cursorLines < 80)
        {
            const int expected = t.last + t.sent;
            if (std::abs(cur - expected) > 3 && cur > 0 && cur < (axis ? 480 : 640))
            {
                ++g_cursorLines;
                LOG("[CURSOR] %s moved by the game: expected %d, is %d (%+d), %lu ms since the last mouse step", name, expected, cur,
                    cur - expected, now - t.tick);
            }
            if (std::fabs(handed) > 25.0f)
            {
                ++g_cursorLines;
                LOG("[CURSOR] %s big mouse step %+.0f units at %d, %lu ms since the last one", name, handed, cur, now - t.tick);
            }
        }
        t.last = cur;
        t.sent = static_cast<int>(handed);
        t.tick = now;
    }

    // In-game cursor screens (hacking, keypads): each injected movement notes whether it reached the menu-cursor
    // code with a 3D device screen as the page (esi at the hook: the page whose flag 400h at +398h makes the game
    // pick parts with a ray), and where the cursor is. The widescreen HUD code uses that to keep the cursor out of
    // its corner-panel shift. (Other pages can take the cursor during normal play too, unseen: e.g. after an
    // alt-tab, so the flag is what counts.)
    bool g_menuHit = false;
    bool g_cursorScreen = false;
    int g_cursorPos[2] = { 320, 240 };
    // Where the cursor of any page is (briefing, menus, the in-game pages): kept for every cursor movement, not just
    // on device screens, so the widescreen HUD can tell the cursor image apart from the corner panels.
    int g_anyCursor[2] = { 320, 240 };
    bool g_anyCursorKnown = false;

    void ApplyMenuDelta(Mem::Regs* r, float& pending, float& remainder, int axis)
    {
        float& delta = r->Stack<float>(0x44);
        float wanted;
        if (g_active)
        {
            wanted = pending;   // every mouse axis event reaching the menu was injected by us
            pending = 0.0f;
        }
        else
            wanted = delta * MenuScale(); // raw input off: scale the vanilla pixel delta
        const float total = remainder + wanted;
        const float whole = std::trunc(total);
        remainder = total - whole;
        delta = whole;
        // where the game puts the cursor after this event: cursor + delta, kept between 0 and the page size
        // ([esp+10h] wide / [esp+14h] high, read by the clamp right after this)
        int pos = r->Stack<short>(axis ? 0x42 : 0x40) + static_cast<int>(whole);
        const int limit = r->Stack<int>(axis ? 0x14 : 0x10);
        if (pos < 0) pos = 0;
        else if (limit > 0 && pos > limit) pos = limit;
        g_anyCursor[axis] = pos;
        g_anyCursorKnown = true;
        bool device = false;
        __try { device = r->esi && (*reinterpret_cast<const uint32_t*>(r->esi + 0x398) & 0x400) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (device)
        {
            g_menuHit = true;
            g_cursorPos[axis] = pos;
        }
        CheckCursor(axis, r->Stack<short>(axis ? 0x42 : 0x40), whole);
    }
    void __cdecl OnMenuAxisX(Mem::Regs* r) { ApplyMenuDelta(r, g_menuDx, g_menuRemX, 0); }
    void __cdecl OnMenuAxisY(Mem::Regs* r) { ApplyMenuDelta(r, g_menuDy, g_menuRemY, 1); }

    bool GameHasFocus()
    {
        HWND fg = GetForegroundWindow();
        if (!fg)
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == g_pid;
    }

    LRESULT CALLBACK RawWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_INPUT)
        {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            {
                if (GameHasFocus())
                {
                    g_dx += ri.data.mouse.lLastX;
                    g_dy += ri.data.mouse.lLastY;
                }
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    DWORD WINAPI RawThread(LPVOID)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = RawWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SCCTRemasterRawMouse";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        RAWINPUTDEVICE rid{ 0x01, 0x02, RIDEV_INPUTSINK, hwnd };
        if (!hwnd || !RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        {
            // Fallback: an invisible top-level window instead of a message-only one.
            LOG("RawMouse: message-only registration failed (%lu), trying hidden window", GetLastError());
            if (hwnd) DestroyWindow(hwnd);
            hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
            rid.hwndTarget = hwnd;
            if (!hwnd || !RegisterRawInputDevices(&rid, 1, sizeof(rid)))
            {
                LOG("RawMouse: RegisterRawInputDevices failed (%lu), vanilla mouse stays active", GetLastError());
                return 0;
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        g_active = true;
        LOG("RawMouse: listening for raw mouse input");

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            DispatchMessageW(&msg);
        return 0;
    }

    int __fastcall hkCauseInputEvent(void* viewport, void* edx, int key, int action, float delta)
    {
        g_viewport = viewport;
        if (g_active && !g_injecting && action == kIST_Axis && (key == kIK_MouseX || key == kIK_MouseY))
        {
            // Vanilla WM_MOUSEMOVE delta (Windows cursor pixels): kept for the menu cursor only.
            (key == kIK_MouseX ? g_sysDx : g_sysDy) += delta;
            return 1;
        }
        return oCauseInputEvent(viewport, edx, key, action, delta);
    }

    void __fastcall hkEngineTick(void* engine, void* edx, float dt)
    {
        void* vp = g_viewport.load();
        if (g_active && vp)
        {
            const long long dx = g_dx.exchange(0);
            const long long dy = g_dy.exchange(0);
            g_sysDx = g_sysDy = 0.0f;   // vanilla (warped) deltas: not used while raw input is on
            if (Captured(vp))
            {
                // in-game cursors (hacking, keypads...): raw counts as desktop pixels, not the warped vanilla deltas
                const float scale = MenuScale() * PointerSpeed();
                g_menuDx = float(dx) * scale;
                g_menuDy = float(dy) * scale;
                const float sens = g_settings.rawSensitivity;
                // While a cursor screen is open, both axes get an event every tick (zero-length when not moving that
                // way): the game only re-checks what's under the cursor on mouse events, and each event recomputes
                // only its own axis's pixel position, keeping the other from the last event of that axis (+3C8h /
                // +3CCh). A hacking screen's lists scroll under a resting cursor, and the game re-centres the cursor
                // (e.g. after a number locks in) without updating those. Once the screen is closed the events no
                // longer reach the cursor code and this stops.
                const bool screen = g_cursorScreen;
                g_menuHit = false;
                g_injecting = true;
                if (dx || screen) oCauseInputEvent(vp, nullptr, kIK_MouseX, kIST_Axis, float(dx) * sens);
                if (dy || screen) oCauseInputEvent(vp, nullptr, kIK_MouseY, kIST_Axis, float(dy) * sens);
                g_injecting = false;
                if (dx || dy || screen) g_cursorScreen = g_menuHit;
            }
            else
                g_cursorScreen = false;
            g_menuDx = g_menuDy = 0.0f;
        }
        Perf::EngineTickBegin();
        oEngineTick(engine, edx, dt);
        Perf::EngineTickEnd();
    }
}

namespace
{
    // Hacking screen clicks (EHackingInteraction, native; vtable 1110BCC8). KeyEvent (10C679B0) handles a left click
    // on its *release*: it looks again for what's under the cursor (FindBoxIPAtCursorPos, vtable +2BCh, then
    // FindScannedIPAtCursorPos, +2C0h: line checks along the cursor ray) and only if that hits an unlocked number
    // does it confirm the hovered one (gameplay +4C4h, set by mouse-move events). With the lists scrolling and the
    // cursor moving on, the release often lands in a gap or on another number: the click did nothing, or confirmed
    // a different one. Now the press is the click (its action arg is changed to the release's, 3, and the real
    // release to 2, which KeyEvent ignores), and if that check misses, a number that was under the cursor within
    // the last 150 ms still counts (the tip just slipped off it or it just scrolled past).
    constexpr DWORD kHackGraceMs = 150;
    void* g_scanSelf = nullptr;
    int g_scanIdx = -1;
    DWORD g_scanTick = 0;
    int g_hackLines = 0;

    int FindScanned(void* self)
    {
        using Fn = int(__thiscall*)(void*);
        return reinterpret_cast<Fn>((*reinterpret_cast<void***>(self))[0x2C0 / 4])(self);
    }

    void __cdecl OnHackLevel(Mem::Regs* r)
    {
        if (Clusterfuck::Active()) r->ecx = (r->ecx & ~0xFFu) | 8u;
    }

    // A new hack (another gameplay object, or the same one set up again): its level, time and locks.
    void NoteHack(void* self)
    {
        static void* lastGp = nullptr;
        static float lastDur = -1, lastStep = -1;
        __try
        {
            uint8_t* gp = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(self) + 0x2F0);
            if (!gp) return;
            const float step = *reinterpret_cast<float*>(gp + 0x4B0), dur = *reinterpret_cast<float*>(gp + 0x4B4);
            const int locks = *reinterpret_cast<int*>(gp + 0x4B8);
            if (gp == lastGp && dur == lastDur && step == lastStep) return;
            lastGp = gp; lastDur = dur; lastStep = step;
            const int level = static_cast<int>(std::lround((26.0f - dur) / 2.0f));
            LOG("[HACK] hack started: difficulty level %d of 0-8, %.0f s, %d lock%s%s", level, dur, locks, locks == 1 ? "" : "s",
                Clusterfuck::Active() ? " (CLUSTERFUCK: unfair hacking, guess the address)" :
                g_settings.fairHacking ? " (fair hacking: they never run out)" : "");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void __cdecl OnHackKeyEvent(Mem::Regs* r)
    {
        NoteHack(reinterpret_cast<void*>(r->ecx));
        if (r->Stack<int>(0x10) != 1) return;   // iKey: IK_LeftMouse
        int& action = r->Stack<int>(0x8);
        if ((action == 1 || action == 3) && g_hackLines < 200)
        {
            ++g_hackLines;
            wchar_t bind[32] = L"";
            __try
            {
                const auto* s = r->Stack<const wchar_t* const*>(0x4);   // the key's binding (FString: Data, Num)
                if (s && s[0] && reinterpret_cast<const int*>(s)[1] > 0) wcsncpy_s(bind, s[0], _TRUNCATE);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            LOG("[HACK] left button %s (bound to '%ls')", action == 1 ? "pressed: handled now" : "released", bind);
        }
        if (action == 1) action = 3;            // press: handle it now, the way the game handles the release
        else if (action == 3) action = 2;       // the release: already handled
    }

    // The click's first check: a digit box of the IP under the cursor (FindBoxIPAtCursorPos, +2BCh). The box click
    // (0x10BF4DE0) uses the hovered box (gameplay +4C8h, set by mouse events like the hovered number), needs that
    // box not solved yet (+4F0h[box]) and a try left (+4B8h). Same as numbers: the box under the cursor becomes the
    // hovered one.
    void __cdecl OnHackClickBox(Mem::Regs* r)
    {
        void* self = reinterpret_cast<void*>(r->ecx);
        using Fn = int(__thiscall*)(void*);
        const int box = reinterpret_cast<Fn>((*reinterpret_cast<void***>(self))[0x2BC / 4])(self);
        r->eax = static_cast<uint32_t>(box);
        if (box == -1) return;
        int was = -2, locks = -1, solved = -1;
        bool given = false;
        __try
        {
            uint8_t* gp = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(self) + 0x2F0);
            if (gp)
            {
                int& hovered = *reinterpret_cast<int*>(gp + 0x4C8);
                was = hovered;
                hovered = box;
                int& left = *reinterpret_cast<int*>(gp + 0x4B8);   // CurrentHackInfo.iNbLocks
                const int* done = *reinterpret_cast<int**>(gp + 0x4F0);
                solved = done ? done[box] : -1;
                // FairHacking: locks never run out (the lock icon counts down to 0 and stays there).
                if (g_settings.fairHacking && !Clusterfuck::Active() && left <= 0 && solved == 0) { left = 1; given = true; }
                locks = left;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (g_hackLines < 200)
        {
            ++g_hackLines;
            LOG("[HACK] click: fragment box %d (hovered box was %d; locks %d%s; %s)", box, was, locks, given ? ", one given" : "",
                solved > 0 ? "already locked: the game ignores it" : locks <= 0 ? "no locks left: the game ignores it" : "a lock is used");
        }
    }

    // the mouse-move check (hover): remember the last number that was under the cursor
    void __cdecl OnHackHoverScan(Mem::Regs* r)
    {
        void* self = reinterpret_cast<void*>(r->ecx);
        const int idx = FindScanned(self);
        r->eax = static_cast<uint32_t>(idx);
        if (idx != -1) { g_scanSelf = self; g_scanIdx = idx; g_scanTick = GetTickCount(); }
    }

    // The click confirms the gameplay object's hovered number (+4C4h; 0x10BF3AD0 mode 0 compares it with the
    // target, +4C0h: equal is a success, anything else a wrong pick), not the one the click's own check found.
    // The hovered number only changes on mouse events, and only when the hover check hits an unlocked number, so
    // a number that scrolled under a resting cursor (or one the cursor reached between events) was clicked as
    // the old one: a wrong pick. Here the number under the cursor at the click becomes the hovered one.
    int SyncHovered(void* self, int idx, bool& locked)
    {
        locked = false;
        __try
        {
            uint8_t* gp = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(self) + 0x2F0);
            if (!gp) return -2;
            const uint8_t* list = *reinterpret_cast<uint8_t**>(gp + 0x498);
            locked = list && (list[idx * 0x1C + 0x18] & 1);
            int& hovered = *reinterpret_cast<int*>(gp + 0x4C4);
            const int was = hovered;
            if (!locked) hovered = idx;
            return was;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
    }

    // the click's check
    void __cdecl OnHackClickScan(Mem::Regs* r)
    {
        void* self = reinterpret_cast<void*>(r->ecx);
        int idx = FindScanned(self);
        const DWORD ago = GetTickCount() - g_scanTick;
        const bool grace = idx == -1 && self == g_scanSelf && g_scanIdx != -1 && ago <= kHackGraceMs;
        if (grace) idx = g_scanIdx;
        r->eax = static_cast<uint32_t>(idx);
        bool locked = false;
        const int was = idx != -1 ? SyncHovered(self, idx, locked) : -2;
        if (g_hackLines < 200)
        {
            ++g_hackLines;
            char extra[96] = "";
            if (locked) _snprintf_s(extra, _TRUNCATE, " (locked: the game ignores it)");
            else if (was >= -1 && was != idx) _snprintf_s(extra, _TRUNCATE, " (the game still had %d as hovered: now %d)", was, idx);
            if (grace) LOG("[HACK] click: number %d (just off it: it was under the cursor %lu ms ago)%s", idx, ago, extra);
            else LOG("[HACK] click: %s %d%s", idx == -1 ? "no number under the cursor" : "number", idx, extra);
        }
    }
}

bool RawMouse::InGameCursor(int& x, int& y)
{
    if (!g_cursorScreen) return false;
    x = g_cursorPos[0];
    y = g_cursorPos[1];
    return true;
}

bool RawMouse::MenuCursor(int& x, int& y)
{
    if (!g_anyCursorKnown) return false;
    x = g_anyCursor[0];
    y = g_anyCursor[1];
    return true;
}

void RawMouse::Init()
{
    if (g_settings.hackingClickFix)
    {
        int ok = 0;
        if (auto a = Mem::Find("hacking KeyEvent", "51 8B 44 24 0C 53 55 8B 6C 24 10 56 57 33 DB 33 FF 83 F8 04"))
            ok += Mem::MidHook("hacking KeyEvent", a, 5, &OnHackKeyEvent, Mem::MidMode::OriginalAfter);
        if (auto a = Mem::Find("hacking hover check", "8B 06 8B CE FF 90 C0 02 00 00 83 F8 FF 0F 84"))
            ok += Mem::MidHook("hacking hover check", a + 4, 6, &OnHackHoverScan, Mem::MidMode::Replace);
        if (auto a = Mem::Find("hacking click check", "8B 06 8B CE FF 90 C0 02 00 00 83 F8 FF 74 26"))
            ok += Mem::MidHook("hacking click check", a + 4, 6, &OnHackClickScan, Mem::MidMode::Replace);
        LOG("RawMouse: hacking clicks %s", ok == 3 ? "on press, with a 150 ms grace, confirming the number under the cursor"
                                                   : "fix FAILED (patterns not found)");
    }
    // Fragment-box clicks: the box under the cursor, logged; with FairHacking a lock is always there.
    if (g_settings.hackingClickFix || g_settings.fairHacking)
        if (auto a = Mem::Find("hacking click box check", "8B 16 8B CE FF 92 BC 02 00 00 83 F8 FF 74 12"))
            Mem::MidHook("hacking click box check", a + 4, 6, &OnHackClickBox, Mem::MidMode::Replace);

    // UnfairHacking (CLUSTERFUCK): every hack is the hardest level (8: 0 locks, 10 s). Hooked where Setup takes
    // the level byte from the level's table ('movzx edx,cl', 0x10BF54D5).
    if (auto a = Mem::Find("hack level", "0F B6 D1 8B 8E 8C 04 00 00 C1 E2 04"))
        Mem::MidHook("hack level", a, 9, &OnHackLevel, Mem::MidMode::OriginalAfter);

    // FairHacking, part 2: a hack's difficulty level (EHackingGameplay Setup, 0x10BF5440) is the device's hacking
    // level + its 'increment' (+1 for every earlier hack of that device: EchelonLevelInfo.HackedDevices) + 1 from
    // the EEV, looked up in the level's per-difficulty table (on Expert: level 8 from tier 3 up, 0 locks, 10 s).
    // The increment is dropped ('add al,bl' NOPed): hacking a device again is no harder than the first time.
    if (g_settings.fairHacking)
    {
        if (auto a = Mem::Find("hack repeat penalty", "8A 80 D6 02 00 00 8B 4C 24 50 8B 11 02 C3"))
        {
            Mem::Nop(a + 12, 2);
            LOG("RawMouse: fair hacking: hack locks never run out, and hacking a device again is no harder");
        }
    }

    // Menu cursor speed (applies with and without raw input).
    if (g_settings.menuCursorSpeed > 0.0f)
    {
        if (auto a = Mem::Find("menu cursor X", "0F BF 44 24 40 89 44 24 20 DB 44 24 20 D8 44 24 44"))
            Mem::MidHook("menu cursor X", a, 5, &OnMenuAxisX, Mem::MidMode::OriginalAfter);
        if (auto a = Mem::Find("menu cursor Y", "0F BF 44 24 42 89 44 24 20 DB 44 24 20 D8 44 24 44"))
            Mem::MidHook("menu cursor Y", a, 5, &OnMenuAxisY, Mem::MidMode::OriginalAfter);
    }

    // UGameEngine::Tick: raw mouse injection and the profiler's engine timing.
    auto tick = Mem::Find("UGameEngine::Tick", "83 EC 1C 53 55 56 8B F1 8A 86 CC 01 00 00");
    if (!tick || !Mem::Detour("UGameEngine::Tick", reinterpret_cast<void*>(tick), reinterpret_cast<void*>(&hkEngineTick), reinterpret_cast<void**>(&oEngineTick)))
        return;

    if (!g_settings.rawInput)
    {
        LOG("RawMouse: disabled in config");
        return;
    }
    g_pid = GetCurrentProcessId();

    auto cie = Mem::Find("UWindowsViewport::CauseInputEvent", "56 8B F1 8B 46 18 8B 48 28 8B 81 A8 00 00 00 85 C0 74");
    if (!cie || !Mem::Detour("CauseInputEvent", reinterpret_cast<void*>(cie), reinterpret_cast<void*>(&hkCauseInputEvent), reinterpret_cast<void**>(&oCauseInputEvent)))
        return;

    if (HANDLE t = CreateThread(nullptr, 0, RawThread, nullptr, 0, nullptr))
        CloseHandle(t);
}
