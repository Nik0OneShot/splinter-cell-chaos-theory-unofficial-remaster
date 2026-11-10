// Skippable videos: Space, Enter, Escape, a mouse click or a controller A/B/Start press ends the
// current (non-looping) Bink video - logos, the intro and cutscenes.
//
// Every Bink video is drawn by UD3DRenderDevice::DrawMovieFrame(movie, flags) (0x10C86280), once per
// engine frame. The "movie" is the UCanvas: +84h HBINK, +94h looping flag (menu backgrounds), +98h hold
// flag. When BINK FrameNum (+0Ch) reaches Frames (+08h) and neither flag is set, the renderer calls the
// canvas' VideoClose (vtable +88h, same as the script native execVideoClose) and the menu script moves
// on. Skipping does exactly that, without drawing or seeking (an earlier BinkGoto-based skip stalled
// while Bink decoded up to the last frame and could wrap the video back to frame 1).
#include "pch.h"
#include "features.h"

namespace
{
    using DrawMovieFrame_t = void(__fastcall*)(void* renDev, void* edx, uint8_t* movie, int flags);
    using VideoClose_t = void(__fastcall*)(void* canvas, void* edx);
    using XInputGetState_t = DWORD(WINAPI*)(DWORD, void*);

    DrawMovieFrame_t oDrawMovieFrame = nullptr;
    XInputGetState_t pXInputGetState = nullptr;

    void* g_currentBink = nullptr;
    uint32_t g_prevButtons = 0;

    struct XInputState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };

    // Bitmask of currently held "skip" inputs.
    uint32_t HeldInputs()
    {
        static const int keys[] = { VK_SPACE, VK_RETURN, VK_ESCAPE, VK_LBUTTON, VK_RBUTTON };
        uint32_t held = 0;
        for (int i = 0; i < 5; ++i)
            if (GetAsyncKeyState(keys[i]) & 0x8000) held |= 1u << i;
        if (pXInputGetState)
        {
            for (DWORD pad = 0; pad < 4; ++pad)
            {
                XInputState st{};
                if (pXInputGetState(pad, &st) == ERROR_SUCCESS)
                {
                    // A = 0x1000, B = 0x2000, Start = 0x0010
                    if (st.buttons & 0x1000) held |= 1u << 8;
                    if (st.buttons & 0x2000) held |= 1u << 9;
                    if (st.buttons & 0x0010) held |= 1u << 10;
                }
            }
        }
        return held;
    }

    bool GameHasFocus()
    {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    void __fastcall hkDrawMovieFrame(void* renDev, void* edx, uint8_t* movie, int flags)
    {
        Watchdog::Heartbeat(); // startup/cutscene videos run in their own loop, outside the main frame loop
        void* bink = movie ? *reinterpret_cast<void**>(movie + 0x84) : nullptr;
        const bool looping = movie && *reinterpret_cast<int*>(movie + 0x94) != 0;
        const bool hold = movie && *reinterpret_cast<int*>(movie + 0x98) != 0;
        if (bink && !looping && hold)
        {
            static void* reported = nullptr;
            if (reported != bink && (HeldInputs() & 0x1F) && GameHasFocus())
            {
                reported = bink;
                LOG("Videos: this video is held open by the game script and can't be skipped");
            }
        }
        if (bink && !looping && !hold)
        {
            const uint32_t held = HeldInputs();
            if (bink != g_currentBink)
            {
                g_currentBink = bink;
                g_prevButtons = held; // inputs already held when the video started don't count
            }
            const uint32_t pressed = held & ~g_prevButtons;
            g_prevButtons = held;
            const uint32_t frames = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bink) + 0x08);
            const uint32_t frameNum = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bink) + 0x0C);
            if (pressed && GameHasFocus() && frameNum < frames)
            {
                // The startup videos play in a loop that doesn't process messages: the skip key/click
                // is still queued and would hit the next screen (e.g. select the hovered menu item).
                MSG msg;
                int flushed = 0;
                while (PeekMessageW(&msg, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) ++flushed;
                while (PeekMessageW(&msg, nullptr, WM_LBUTTONDOWN, WM_XBUTTONDBLCLK, PM_REMOVE)) ++flushed;
                Window::SuppressInputFor(400); // the matching key-up / button-up arrives a little later
                LOG("Videos: skipped at frame %u of %u (%d queued input messages discarded)", frameNum, frames, flushed);
                g_currentBink = nullptr;
                // Same as the natural end: the canvas closes the video and the script continues.
                const auto close = reinterpret_cast<VideoClose_t>((*reinterpret_cast<void***>(movie))[0x88 / 4]);
                close(movie, nullptr);
                return; // nothing left to draw this frame
            }
        }
        oDrawMovieFrame(renDev, edx, movie, flags);
    }
}

void Videos::Init()
{
    if (!g_settings.skippableVideos)
        return;
    for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
    {
        if (HMODULE x = LoadLibraryW(dll))
        {
            pXInputGetState = reinterpret_cast<XInputGetState_t>(GetProcAddress(x, "XInputGetState"));
            if (pXInputGetState) break;
        }
    }
    if (auto a = Mem::Find("UD3DRenderDevice::DrawMovieFrame", "8B 44 24 04 81 EC 84 00 00 00 56"))
    {
        if (Mem::Detour("DrawMovieFrame", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkDrawMovieFrame), reinterpret_cast<void**>(&oDrawMovieFrame)))
            LOG("Videos: skippable with Space / Enter / Esc / mouse click / controller A, B, Start");
    }
}
