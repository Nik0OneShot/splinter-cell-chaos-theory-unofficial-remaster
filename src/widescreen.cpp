// Widescreen / ultrawide support.
// Ported from ThirteenAG's SplinterCellChaosTheory.WidescreenFix (MIT licence,
// github.com/ThirteenAG/WidescreenFixesPack), 2024 single-file revision, plus the
// resolution unlock, high resolution Bink and FMV aspect logic from the later FusionFix.
#include "pch.h"
#include "features.h"

namespace
{
    struct
    {
        int width = 0, height = 0;
        float fWidth = 0, fHeight = 0, aspect = 4.0f / 3.0f;
        float hudScaleX = 0.003125f;      // 2 / 640 at 4:3
        float hudOffset = 1.0f;
        const float hudScaleXOriginal = 0.003125f;
        const float hudOffsetOriginal = 1.0f;
        float hudScaleXDyn = 0.003125f;   // what the HUD renderer reads (switches per image)
        float hudOffsetDyn = 1.0f;
        float textScaleX = 2.0f;
        int hudOffsetReal = 0;            // pixels (640x480 space) between 4:3 area and screen edge
        int scopeScale = 0;
        int wsHudOffsetInt = 0;           // how far corner panels move outwards
        float wsHudOffsetFloat = 0.0f;
    } S;

    float* g_gameHudScale = nullptr;      // game global (dword_1120B6BC)
    uint32_t* g_textGlobal = nullptr;     // game global (dword_11223A7C)
    uint8_t* g_inMenu = nullptr;          // render device +0x4D39

    union FColor { uint32_t rgba; struct { uint8_t b, g, r, a; }; };

    struct TextOffset
    {
        struct { int v1v1 = 435, v1v2 = 436, v1v3 = 437, v2v1 = 3, v2v2 = 21, v3v1 = 313, v3v2 = 329, v3v3 = 345, v3v4 = 361, v3v5 = 377, v3v6 = 393, v3v7 = 416; } topCorner;
        struct { int v1v1 = 489, v1v2 = 598, v2v1 = 1, v2v2 = 20, v3v1 = 23, v3v2 = 39, v3v3 = 93; } bottomCorner;
        struct { int v1 = 598, v2 = 3, v3 = 93; } icons;
        struct { int v1v1 = 465, v1v2 = 615, v2v1 = 3, v2v2 = 75, v3v1 = 128, v3v2 = 215; } objPopup;
    } g_text;

    std::pair<int, int> DesktopRes()
    {
        return { GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }

    float AdjustFOV(float fov)
    {
        constexpr float kPi = 3.14159265358979f;
        float base = std::clamp(fov * g_settings.fovMultiplier, 1.0f, 170.0f);
        float hor = 2.0f * std::atan(std::tan(base * 0.5f * kPi / 180.0f) * (S.aspect / (4.0f / 3.0f))) * 180.0f / kPi;
        return std::clamp(hor, 1.0f, 170.0f);
    }

    void OnResolution(int w, int h, bool fullscreen)
    {
        if (w <= 0 || h <= 0)
            return;
        S.width = w; S.height = h;
        S.fWidth = float(w); S.fHeight = float(h);
        S.aspect = S.fWidth / S.fHeight;

        S.hudScaleX = (1.0f / S.fWidth * (S.fHeight / 480.0f)) * 2.0f;
        if (g_gameHudScale)
            *g_gameHudScale = S.hudScaleX;
        S.textScaleX = ((4.0f / 3.0f) / S.aspect) * 2.0f;
        S.hudOffset = S.textScaleX / 2.0f;
        S.hudOffsetReal = static_cast<int>(((480.0f * S.aspect) - 640.0f) / 2.0f);
        S.scopeScale = static_cast<int>(S.fWidth * (S.aspect / (4.0f / 3.0f)));

        const float edge = static_cast<float>(g_settings.hudEdgeOffset);
        if (S.aspect < (16.0f / 9.0f))
            S.wsHudOffsetFloat = edge / (((16.0f / 9.0f) / S.aspect) * 1.5f);
        else
            S.wsHudOffsetFloat = edge;
        S.wsHudOffsetFloat = std::min(S.wsHudOffsetFloat, static_cast<float>(std::max(S.hudOffsetReal, 0)));
        S.wsHudOffsetInt = static_cast<int>(S.wsHudOffsetFloat);

        S.hudScaleXDyn = S.hudScaleX;
        S.hudOffsetDyn = S.hudOffset;
        LOG("Widescreen: %dx%d (%s) aspect %.3f, HUD margin %d, edge offset %d", w, h, fullscreen ? "fullscreen" : "windowed",
            S.aspect, S.hudOffsetReal, S.wsHudOffsetInt);
    }

    // Before the game reads its resolution from the ini (its active one: ProgramData\Ubisoft\...\SplinterCell3.ini;
    // the copy in System\ is only the template): write the configured resolution and shader model. ShaderModel
    // ([WinDrv.WindowsClient], 1 = SM1.1 / 2 = SM2.0 / 3 = SM3.0) picks the shader set the game draws with; all
    // three are always created. Here it had been 1.1: crude low-precision specular, and no parallax mapping, HDR,
    // tone mapping or soft shadows even when switched on.
    void ApplyShaderModel(const wchar_t* path)
    {
        const int sm = g_settings.shaderModel;
        if (sm < 1 || sm > 3 || !path)
            return;
        wchar_t cur[16] = {};
        GetPrivateProfileStringW(L"WinDrv.WindowsClient", L"ShaderModel", L"", cur, 16, path);
        const std::wstring want = std::to_wstring(sm);
        if (want == cur)
        {
            LOG("Graphics: shader model %ls", cur);
            return;
        }
        WritePrivateProfileStringW(L"WinDrv.WindowsClient", L"ShaderModel", want.c_str(), path);
        LOG("Graphics: shader model %ls -> %ls (the in-game setting is overridden; [Graphics] ShaderModel = 0 keeps it)",
            cur[0] ? cur : L"unset", want.c_str());
    }

    void __cdecl OnSetRes(Mem::Regs* r)
    {
        const auto path = reinterpret_cast<const wchar_t*>(r->edi);
        ApplyShaderModel(path);
        int w = g_settings.width, h = g_settings.height;
        if (w < 0 || h < 0)
            return; // use the in-game setting
        if (w == 0 || h == 0)
            std::tie(w, h) = DesktopRes();
        if (w < 640 || h < 480)
            return;

        auto ws = std::to_wstring(w), hs = std::to_wstring(h);
        WritePrivateProfileStringW(L"WinDrv.WindowsClient", L"WindowedViewportX", ws.c_str(), path);
        WritePrivateProfileStringW(L"WinDrv.WindowsClient", L"WindowedViewportY", hs.c_str(), path);

        // Fullscreen needs a real display mode.
        bool mode = false;
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        for (DWORD i = 0; !mode && EnumDisplaySettingsW(nullptr, i, &dm); ++i)
            mode = int(dm.dmPelsWidth) == w && int(dm.dmPelsHeight) == h;
        if (!mode)
        {
            auto [dw, dh] = DesktopRes();
            ws = std::to_wstring(dw); hs = std::to_wstring(dh);
        }
        WritePrivateProfileStringW(L"WinDrv.WindowsClient", L"FullscreenViewportX", ws.c_str(), path);
        WritePrivateProfileStringW(L"WinDrv.WindowsClient", L"FullscreenViewportY", hs.c_str(), path);
        LOG("Widescreen: resolution set to %dx%d (fullscreen %ls x %ls)", w, h, ws.c_str(), hs.c_str());
    }

    void __cdecl OnResFullscreen(Mem::Regs* r) { OnResolution(int(r->ebp), int(r->edi), true); }
    void __cdecl OnResWindowed(Mem::Regs* r) { OnResolution(int(r->ebp), int(r->edi), false); }
    void __cdecl OnMenuFlag(Mem::Regs* r) { g_inMenu = reinterpret_cast<uint8_t*>(r->ecx + 0x4D39); }

    bool InGameplay() { return g_inMenu && *g_inMenu == 0; }

    // [Debug] LogHudImages: press F9 to log every distinct HUD/menu image drawn in the next 80 ms:
    // the 640x480 rectangle, colour, and the sprite region (texture UVs and size).
    bool ReadRegion(const uint8_t* region, int16_t (&w)[6], uint32_t& head)
    {
        __try { std::memcpy(w, region + 8, sizeof(w)); head = *reinterpret_cast<const uint32_t*>(region); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    DWORD g_snapUntil = 0;

    void LogHudImage(const uint8_t* region, int left, int right, int top, int bottom, uint32_t color)
    {
        static std::unordered_set<uint64_t> seen;
        DWORD& until = g_snapUntil;
        static bool wasDown = false;
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        const DWORD now = GetTickCount();
        if (down && !wasDown)
        {
            until = now + 80;
            seen.clear();
            LOG("[HUDIMG] ---- snapshot (%s) ----", InGameplay() ? "gameplay" : "menu");
        }
        wasDown = down;
        if (int(until - now) <= 0)
            return;
        int16_t w[6] = {};
        uint32_t head = 0;
        const bool ok = region && ReadRegion(region, w, head);
        const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(region)) * 0x9E3779B97F4A7C15ull) ^
                             (uint64_t(uint16_t(left)) | uint64_t(uint16_t(right)) << 16 | uint64_t(uint16_t(top)) << 32 |
                              uint64_t(uint16_t(bottom)) << 48) ^ color;
        if (!seen.insert(key).second)
            return;
        LOG("[HUDIMG] l=%d r=%d t=%d b=%d color=%08X region=%p head=%08X uv=(%d %d %d %d) size=%dx%d%s", left, right, top, bottom,
            color, region, head, w[0], w[1], w[2], w[3], uint16_t(w[4]), uint16_t(w[5]), ok ? "" : " (unreadable)");
    }

    // Per HUD image: full-screen images keep the original 4:3 stretch, scope borders and
    // camera overlays are pushed to the screen edges, corner panels move outwards.
    void __cdecl OnHudImage(Mem::Regs* r)
    {
        S.hudScaleXDyn = S.hudScaleX;
        S.hudOffsetDyn = S.hudOffset;
        if (g_settings.logHudImages)
            LogHudImage(reinterpret_cast<const uint8_t*>(r->esi), r->Stack<int16_t>(0x40), r->Stack<int16_t>(0x42),
                        r->Stack<int16_t>(0x44), r->Stack<int16_t>(0x46), r->Stack<uint32_t>(0x3C));

        auto& left = r->Stack<int16_t>(0x40);
        auto& right = r->Stack<int16_t>(0x42);
        const int16_t top = r->Stack<int16_t>(0x44);
        const int16_t bottom = r->Stack<int16_t>(0x46);
        const FColor c{ r->Stack<uint32_t>(0x3C) };

        // Loading screens (mission start, and quitting to the menu): the background is a 640x512 image
        // drawn in two pieces (GenLoadings\Menu_loading_1, 512 wide, and _2, 128 wide), while the mission
        // picture and text are placed in the 4:3 area. Stretching only the background to the screen width
        // moved its picture frame away from the picture (half the frame black at 21:9) and distorted it.
        // Keep it in the 4:3 area with everything drawn on top of it.
        if (top == 0 && bottom == 512 && ((left == 0 && right == 512) || (left == 512 && right == 640)) &&
            !g_settings.stretchLoadingScreens)
            return;

        if ((left == 0 && right == 640) || (left == -2 && right == 639 && top == -2 && bottom == 479) ||
            (left == -1 && right == 640 && top == -2 && bottom == 479) || (top == 0 && bottom == 512))
        {
            S.hudScaleXDyn = S.hudScaleXOriginal;
            S.hudOffsetDyn = S.hudOffsetOriginal;
            return;
        }
        if (!InGameplay())
            return;

        const int off = S.hudOffsetReal;
        if ((left == -1 && right == 256) || (left == -2 && right == 255) || (left == -61 && right == 319) || (left == -60 && right == 320))
        {
            left -= off; right -= off; // scope image left half
            return;
        }
        if (((left == 382 || left == 383 || left == 384) && (right == 639 || right == 640 || right == 641)) ||
            ((left == 319 && right == 699) || (left == 320 && right == 700)))
        {
            left += off; right += off; // scope image right half
            return;
        }
        if (top == 0 && bottom == 480 && c.a == 0x1E) // camera feed white overlay
        {
            if (left == 0 && right == 65) left -= off;
            else if (left == 575 && right == 640) right += off;
        }
        if ((left == -1 || left == 579) && (right == 62 || right == 649) && top == -3 && bottom == 483 && c.rgba == 0x1bffffff)
        {
            if (left == -1) left -= off; else right += off; // weapon scope
        }

        if (!g_settings.hudWidescreenMode)
            return;

        const bool topRightColor = c.rgba == 0x99ffffff || c.rgba == 0xc8ffffff || c.rgba == 0x59ffffff || c.rgba == 0x80ffffff ||
                                   c.rgba == 0x32ffffff || c.rgba == 0x96ffffff;
        const bool topRight = topRightColor &&
            (((left >= 421 && left <= 435) && (right >= 421 && right <= 435) && top >= 0 && bottom <= 200) ||
             ((left >= 421 && left <= 438) && (right <= 630) && top >= 0 && bottom <= 200) ||
             ((left >= 600 && left <= 635) && (right >= 600 && right <= 635) && top >= 0 && bottom <= 200));
        const bool panelColor = c.rgba == 0x4bb8fac8 || c.rgba == 0x32ffffff || c.rgba == 0x40ffffff || c.rgba == 0x59ffffff ||
                                c.rgba == 0x80ffffff || c.rgba == 0x96ffffff || c.rgba == 0x99ffffff || c.rgba == 0xc8ffffff ||
                                c.rgba == 0xffffffff || (c.r == 0xb8 && c.g == 0xf7 && c.b == 0xc8);
        const bool bottomRight = panelColor && (left >= 465 && left <= 622) && (right >= 468 && right <= 625) && top >= 360 && bottom <= 465;
        const bool objective = (c.r == 0xff && c.g == 0xff && c.b == 0xff) && (left >= 465 && left <= 622) && (right >= 468 && right <= 625) &&
                               top >= 250 && bottom <= 351;
        if (!(topRight || bottomRight || objective))
            return;

        const int w = right - left, h = bottom - top;
        const bool excluded =
            (left == 566 && right == 569 && top == 409 && bottom == 425) ||
            (left == 562 && right == 566 && top == 409 && bottom == 410) ||
            (left == 562 && right == 566 && top == 424 && bottom == 425) ||
            (left == 594 && right == 604 && top == 270 && bottom == 271) ||
            ((w >= 1 && w <= 4) && (h == 1 || h == 16 || h == 21 || h == 22) && (top >= 195 && bottom <= 395));
        // The cursor is a small white image too: pushed out like a panel whenever it entered these zones, so it
        // jumped sideways on the right side of the screen. On a hacking/keypad screen it's the small image under the
        // cursor point. On any other page with a cursor (briefing, the pages that take the cursor during play) it's a
        // small image with its corner (or centre) on the cursor point: a page can keep an unseen cursor that moves
        // with aiming, and a HUD icon it merely passes over must still move out.
        int cx = 0, cy = 0;
        bool cursor = false, device = false;
        if (w <= 64 && h <= 64)
        {
            if (RawMouse::InGameCursor(cx, cy))
                device = cursor = cx >= left - 2 && cx <= right + 2 && cy >= top - 2 && cy <= bottom + 2;
            // Only the cursor sprite itself (white, about 32x32): during play the hidden cursor sweeps across the
            // HUD as Sam turns, and the panel icons it touched (the green objectives and notes icons, small bars)
            // were each left unpushed for a frame, so they flicked back and forth across the screen.
            const bool sprite = c.r == 0xff && c.g == 0xff && c.b == 0xff && w >= 28 && w <= 36 && h >= 28 && h <= 36;
            if (!cursor && sprite && RawMouse::MenuCursor(cx, cy))
            {
                const int mx = (left + right) / 2, my = (top + bottom) / 2;
                cursor = (std::abs(left - cx) <= 3 && std::abs(top - cy) <= 3) || (std::abs(mx - cx) <= 3 && std::abs(my - cy) <= 3);
                static int nearLines = 0;
                if (!cursor && g_settings.logCursor && nearLines < 6 && cx >= left - 2 && cx <= right + 2 && cy >= top - 2 &&
                    cy <= bottom + 2)
                {
                    ++nearLines;
                    LOG("[CURSOR] a small image under the cursor (%d %d) was pushed %d to the right as a HUD panel: %d-%d x %d-%d, "
                        "colour %08X", cx, cy, S.wsHudOffsetInt, left, right, top, bottom, c.rgba);
                }
            }
        }
        if (cursor)
        {
            static bool logged[2] = {};
            if (!logged[device])
            {
                logged[device] = true;
                LOG("[CURSOR] the %s cursor image (%d-%d x %d-%d, colour %08X) entered a HUD corner zone at (%d %d): kept in "
                    "place instead of pushed %d to the right", device ? "hacking/keypad" : "menu", left, right, top, bottom, c.rgba,
                    cx, cy, S.wsHudOffsetInt);
            }
        }
        if (!excluded && !cursor)
        {
            left += S.wsHudOffsetInt;
            right += S.wsHudOffsetInt;
        }
    }

    // Loading progress bar (0x10A3D8D0, also on the quit-to-menu "LOADING..." screen): drawn at X 56, width
    // 259 (640x480 units) scaled by the full viewport width, so it no longer matched the 4:3 loading screen.
    // Hooked right after it computed its rectangle: locals Y +0Ch, X +10h, H +14h, W +18h; esi = viewport
    // (SizeX +8Ch, SizeY +90h). X and W are remapped into the centred 4:3 area.
    void __cdecl OnLoadingBar(Mem::Regs* r)
    {
        if (g_settings.stretchLoadingScreens)
            return;
        const int sx = *reinterpret_cast<const int*>(r->esi + 0x8C);
        const int sy = *reinterpret_cast<const int*>(r->esi + 0x90);
        const float w43 = static_cast<float>(sy) * 4.0f / 3.0f;
        if (sx <= 0 || sy <= 0 || static_cast<float>(sx) <= w43 + 0.5f)
            return;
        const float k = w43 / static_cast<float>(sx);
        r->Stack<float>(0x10) = (static_cast<float>(sx) - w43) * 0.5f + r->Stack<float>(0x10) * k;
        r->Stack<float>(0x18) *= k;
    }

    void __cdecl OnScope(Mem::Regs* r) { r->Stack<int32_t>(0x68) = S.scopeScale; }

    void __cdecl OnHudText(Mem::Regs* r)
    {
        if (!InGameplay())
            return;
        const int o1 = r->Stack<int32_t>(0x4);
        const int o2 = r->Stack<int32_t>(0xC);
        const int o3 = static_cast<int>(r->Stack<float>(0x1C));
        const FColor c{ r->Stack<uint32_t>(0x160) };
        const auto& tc = g_text.topCorner; const auto& bc = g_text.bottomCorner;
        const auto& ico = g_text.icons; const auto& obj = g_text.objPopup;

        const bool color1 = c.r == 0xff && c.g == 0xff && c.b == 0xff;
        const bool color2 = c.r == 0xb8 && c.g == 0xfa && c.b == 0xc8;
        const bool color3 = c.r == 0x66 && c.g == 0x66 && c.b == 0x66;
        const bool color4 = c.r == 0xb8 && c.g == 0xf7 && c.b == 0xc8;
        const bool tc1 = o1 == tc.v1v1 || o1 == tc.v1v2 || o1 == tc.v1v3;
        const bool tc2 = o2 >= tc.v2v1 && o2 <= tc.v2v2;
        const bool tc3 = o3 == tc.v3v1 || o3 == tc.v3v2 || o3 == tc.v3v3 || o3 == tc.v3v4 || o3 == tc.v3v5 || o3 == tc.v3v6 || o3 == tc.v3v7;
        const bool bc1 = o1 >= bc.v1v1 && o1 <= bc.v1v2;
        const bool bc2 = o2 >= bc.v2v1 && o2 <= bc.v2v2;
        const bool bc3 = o3 == bc.v3v1 || o3 == bc.v3v2 || o3 == bc.v3v3;
        const bool icoO = o1 == ico.v1 && o2 == ico.v2 && o3 == ico.v3;
        const bool ob1 = o1 >= obj.v1v1 && o1 <= obj.v1v2;
        const bool ob2 = o2 >= obj.v2v1 && o2 <= obj.v2v2;
        const bool ob3 = o3 >= obj.v3v1 && o3 <= obj.v3v2;
        if ((tc1 && tc2 && tc3 && (color1 || color2 || color3)) || (bc1 && bc2 && bc3 && (color1 || color2)) ||
            (icoO && color4) || (ob1 && ob2 && ob3 && color4))
        {
            r->Stack<float>(0x14) += S.wsHudOffsetFloat;
        }
    }

    void __cdecl OnFov1(Mem::Regs* r)
    {
        float f = AdjustFOV(*reinterpret_cast<float*>(r->ecx + 0x2BC));
        std::memcpy(&r->edx, &f, 4);
    }

    void __cdecl OnFov2(Mem::Regs* r)
    {
        float f = AdjustFOV(*reinterpret_cast<float*>(r->eax + 0x2BC));
        std::memcpy(&r->ecx, &f, 4);
    }

    // Hacking/keypad screens pick the device part under the cursor with a ray from the camera (0x10A0D050, called
    // only from the Magma cursor code, 0x10A0EE22): the cursor (640x480 units) becomes a pixel across the full
    // viewport width, and the ray's scene view gets the camera's raw FOV. The screen is rendered with the Hor+ FOV
    // and the cursor is drawn in the centred 4:3 area, so on a wide screen the pick missed the drawn cursor
    // (vertically: at 21:9 the ray landed at ~0.56x the cursor's distance from the centre). Hooked on
    // 'mov edx,[esi+2BCh]' (the FOV handed to the scene view): it becomes the rendered FOV, and the screen position
    // (argument 1, an FVector in pixels, at [esp+350h]; viewport edi: SizeX +8Ch, SizeY +90h) moves to where the
    // cursor is drawn. The caller reuses that FVector's memory for something else right after.
    void __cdecl OnPickFov(Mem::Regs* r)
    {
        float raw;
        std::memcpy(&raw, &r->edx, 4);
        const float fov = AdjustFOV(raw);
        std::memcpy(&r->edx, &fov, 4);
        static int logged = 0;
        __try
        {
            float* pos = r->Stack<float*>(0x350);
            const int sx = *reinterpret_cast<const int*>(r->edi + 0x8C);
            const int sy = *reinterpret_cast<const int*>(r->edi + 0x90);
            const float w43 = static_cast<float>(sy) * 4.0f / 3.0f;
            if (pos && sx >= 320 && sy >= 240 && sx <= 16384 && sy <= 16384 && static_cast<float>(sx) > w43 + 0.5f)
            {
                const float before = pos[0];
                pos[0] = (static_cast<float>(sx) - w43) * 0.5f + pos[0] * w43 / static_cast<float>(sx);
                if (logged < 1)
                {
                    ++logged;
                    LOG("Widescreen: hacking/keypad pick lined up with the drawn cursor (FOV %.1f -> %.1f, x %.0f -> %.0f, y %.0f, "
                        "viewport %dx%d)", raw, fov, before, pos[0], pos[1], sx, sy);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // FMV quad: keep the movie's own aspect ratio instead of stretching it.
    void __cdecl OnMovieQuad(Mem::Regs* r)
    {
        if (g_settings.fmvAspect < 0)
            return;
        auto movie = reinterpret_cast<const uint32_t*>(r->esi);
        auto renderer = reinterpret_cast<const uint8_t*>(r->edi);
        auto args = reinterpret_cast<float*>(r->esp + 4);
        const float width = float(*reinterpret_cast<const int*>(renderer + 0x4898));
        const float height = float(*reinterpret_cast<const int*>(renderer + 0x489C));
        const float mw = float(movie[0]), mh = float(movie[1]);
        if (!(width > 0 && height > 0 && mw > 0 && mh > 0))
            return;
        const float aspect = mw / mh;
        const bool inside43 = g_settings.fmvAspect == 0;
        const float areaW = inside43 ? std::min(width, height * 4.0f / 3.0f) : width;
        const float areaH = inside43 ? std::min(height, width * 3.0f / 4.0f) : height;
        const float drawW = std::min(areaW, areaH * aspect);
        const float drawH = std::min(areaH, areaW / aspect);
        const float l = (width - drawW) * 0.5f, t = (height - drawH) * 0.5f;
        if (l > 0.0f || t > 0.0f)
        {
            if (auto dev = *reinterpret_cast<IDirect3DDevice9* const*>(renderer + 0x4D3C))
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        }
        args[0] = l; args[1] = t; args[2] = l + drawW; args[3] = t + drawH;
    }

    void __cdecl OnWindowPos(Mem::Regs* r)
    {
        if (r->edi == WM_WINDOWPOSCHANGED && r->eax)
        {
            auto wp = reinterpret_cast<const WINDOWPOS*>(r->eax);
            if (wp->x < 0 && wp->y < 0)
                ShowWindow(reinterpret_cast<HWND>(r->edx), SW_RESTORE);
        }
    }

    void RedirectOperand(const char* name, const char* pattern, size_t operandOffset, void* target)
    {
        if (auto a = Mem::Find(name, pattern))
            Mem::Write<void*>(a + operandOffset, target);
    }
}

bool Widescreen::SnapshotActive()
{
    return g_settings.logHudImages && int(g_snapUntil - GetTickCount()) > 0;
}

void Widescreen::Init()
{
    const auto& s = g_settings;

    if (auto a = Mem::Find("set resolution", "8D 84 24 34 04 00 00 68 ? ? ? ? 50 E8 ? ? ? ? 83 C4 14"))
        Mem::MidHook("set resolution", a, 7, &OnSetRes, Mem::MidMode::OriginalAfter);

    if (s.unlockResolutions)
    {
        auto from = Mem::Find("resolution list filter", "75 ? B8 ? ? ? ? 68 ? ? ? ? 50 E8 ? ? ? ? 8B 5C 24 3C");
        auto to = Mem::Find("resolution list continue", "8B 5C 24 18 8B 43 78 3B C5");
        if (from && to)
            Mem::MakeJmp(from, reinterpret_cast<void*>(to));
    }

    if (s.highResVideos)
    {
        const char* binkPatterns[] = {
            "68 E0 01 00 00 68 80 02 00 00 50 FF 52 ? EB 18",
            "68 E0 01 00 00 68 80 02 00 00 50 FF 51 ? EB 18 8B 10 6A 01 6A 15 6A 00 6A 01 68 E0 01 00 00 68 80 02 00 00 50 FF 52 ? 85 DB",
            "68 E0 01 00 00 68 80 02 00 00 50 FF 51 ? EB 18 8B 10 6A 01 6A 15 6A 00 6A 01 68 E0 01 00 00 68 80 02 00 00 50 FF 52 ? 8B AC 24 88 00 00 00",
        };
        for (auto p : binkPatterns)
        {
            if (auto a = Mem::Find("bink surface size", p))
            {
                Mem::Write<int32_t>(a + 1, 2160);
                Mem::Write<int32_t>(a + 6, 3840);
            }
        }
        static float invH = 1.0f / 2160.0f, invW = 1.0f / 3840.0f;
        RedirectOperand("bink v scale", "D8 0D ? ? ? ? 8B 0E", 2, &invH);
        RedirectOperand("bink u scale", "D8 0D ? ? ? ? 8B 54 24 18 8B 44 24 24", 2, &invW);
    }

    if (auto a = Mem::Find("movie quad", "52 50 57 E8 ? ? ? ? 68 00 00 80 3F 68 00 00 80 3F 68 00 00 80 3F 68 00 00 80 3F 6A 2E 8B C8"))
        Mem::CallHook("movie quad", a + 3, &OnMovieQuad);

    if (s.windowedAltTabFix)
    {
        if (auto a = Mem::Find("viewport wndproc tail", "8B 84 24 ? ? ? ? 50 53 57 52 74 50 FF 15 ? ? ? ? 5F 5E 5D 5B 81 C4 ? ? ? ? C2 0C 00"))
            Mem::MidHook("alt-tab fix", a, 7, &OnWindowPos, Mem::MidMode::OriginalBefore);
    }

    if (s.shadowMapResolution > 0 && s.shadowMapResolution != 2048)
    {
        if (auto a = Mem::Find("shadow map size", "68 ? ? ? ? 68 ? ? ? ? 8B CE FF 92 ? ? ? ? 5F 5E C7 43 ? ? ? ? ? 5B C2 04 00 C7 87"))
        {
            Mem::Write<int32_t>(a + 1, s.shadowMapResolution);
            Mem::Write<int32_t>(a + 6, s.shadowMapResolution);
        }
        if (auto a = Mem::Find("shadow map size 2", "B9 00 01 00 00 89 94 24 DC 00 00 00"))
            Mem::Write<int32_t>(a + 1, s.shadowMapResolution);
    }
    if (!s.shadowFiltering)
    {
        if (auto a = Mem::Find("shadow filter", "B9 ? ? ? ? 89 94 24 DC 00 00 00"))
            Mem::Write<int32_t>(a + 1, 0x10000);
        if (auto a = Mem::Find("soft shadow scale", "BB 9A 99 99 3F"))
            Mem::Write<float>(a + 1, 0.25f);
        if (auto a = Mem::Find("soft shadow bias", "C7 86 C8 07 00 00 9A 99 99 BE"))
            Mem::Write<float>(a + 6, -0.15f);
    }

    if (!s.widescreen)
    {
        LOG("Widescreen: disabled in config");
        return;
    }

    switch (s.gameLanguage)
    {
    case 4: g_text.bottomCorner.v1v1 -= 7; g_text.objPopup.v2v2 += 5; g_text.topCorner.v2v2 += 4; break; // Italian
    case 5: g_text.bottomCorner.v1v1 -= 10; break;                                                        // Polish
    case 6: g_text.bottomCorner.v1v1 -= 9; g_text.objPopup.v2v2 += 4; g_text.topCorner.v2v2 += 2; break;  // Russian
    default: break;
    }

    if (auto a = Mem::Find("HUD scale global", "D9 1D ? ? ? ? 0F BF"))
        g_gameHudScale = Mem::Read<float*>(a + 2);
    if (auto a = Mem::Find("HUD text global", "A1 ? ? ? ? 83 C4 04 85 C0 D8 3D"))
        g_textGlobal = Mem::Read<uint32_t*>(a + 1);

    if (auto a = Mem::Find("set resolution (fullscreen)", "89 6B 58 89 7B 5C EB 0E"))
        Mem::MidHook("resolution fullscreen", a, 6, &OnResFullscreen, Mem::MidMode::OriginalBefore);
    if (auto a = Mem::Find("set resolution (windowed)", "89 6B 50 89 7B 54 6A 00 68 00 10 00 00"))
        Mem::MidHook("resolution windowed", a, 6, &OnResWindowed, Mem::MidMode::OriginalBefore);

    if (auto a = Mem::Find("menu flag", "88 81 39 4D 00 00 C2 08 00"))
        Mem::MidHook("menu flag", a, 6, &OnMenuFlag, Mem::MidMode::OriginalBefore);

    // HUD scale reads go through our dynamic value.
    if (g_gameHudScale)
    {
        char pat[64];
        const auto v = reinterpret_cast<uintptr_t>(g_gameHudScale);
        auto bytes = [&](const char* prefix)
        {
            snprintf(pat, sizeof(pat), "%s %02X %02X %02X %02X", prefix, unsigned(v & 0xFF), unsigned((v >> 8) & 0xFF),
                     unsigned((v >> 16) & 0xFF), unsigned((v >> 24) & 0xFF));
            return pat;
        };
        for (auto a : Mem::FindPattern(bytes("D9 05"))) Mem::Write<void*>(a + 2, &S.hudScaleXDyn); // fld
        for (auto a : Mem::FindPattern(bytes("A1"))) Mem::Write<void*>(a + 1, &S.hudScaleXDyn);    // mov eax
        for (auto a : Mem::FindPattern(bytes("D8 0D"))) Mem::Write<void*>(a + 2, &S.hudScaleXDyn); // fmul
    }
    RedirectOperand("HUD offset", "D8 25 ? ? ? ? D9 05 ? ? ? ? D8 88", 2, &S.hudOffsetDyn);

    // Loading bar rectangle: call [edx+0E8h]; mov ebx,eax; test ebx,ebx; jz ...; | push 0 x3 | push 0; push ecx
    if (auto a = Mem::Find("loading bar", "FF 92 E8 00 00 00 8B D8 85 DB 0F 84 ? ? ? ? 6A 00 6A 00 6A 00 6A 00 51"))
        Mem::MidHook("loading bar", a + 0x10, 6, &OnLoadingBar, Mem::MidMode::OriginalAfter);

    if (auto a = Mem::Find("HUD image", "33 CA 89 48 68 D9 44 24 48 DA E9 DF E0"))
        Mem::MidHook("HUD image", a, 5, &OnHudImage, Mem::MidMode::OriginalBefore);

    if (auto a = Mem::Find("scope", "8B 8E A4 01 00 00 E8 ? ? ? ? 8B 86 A4 01 00 00 8B 88 30 61 00 00 89 88 E0 56 00 00 8B 90 1C 76 00 00 89 90 E8 56 00 00 8B 06 6A 03"))
        Mem::MidHook("scope", a, 6, &OnScope, Mem::MidMode::OriginalBefore);

    RedirectOperand("text scale", "D8 3D ? ? ? ? D9 5C 24 68 DB", 2, &S.textScaleX);
    RedirectOperand("text offset", "D8 25 ? ? ? ? D9 44 24 24 D8 4C 24", 2, &S.hudOffset);

    if (s.hudWidescreenMode)
    {
        if (auto a = Mem::Find("HUD text", "A1 ? ? ? ? 83 C4 04 85 C0 D8 3D"))
            Mem::MidHook("HUD text", a, 5, &OnHudText, Mem::MidMode::OriginalBefore);
    }

    if (auto a = Mem::Find("FOV 1", "8B 91 BC 02 00 00 52 8B 54 24 24"))
        Mem::MidHook("FOV 1", a, 6, &OnFov1, Mem::MidMode::OriginalBefore);
    if (auto a = Mem::Find("FOV 2", "8B 88 BC 02 00 00 8B 44 24 20 51"))
        Mem::MidHook("FOV 2", a, 6, &OnFov2, Mem::MidMode::OriginalBefore);
    if (auto a = Mem::Find("hacking pick FOV", "8B 96 BC 02 00 00 8B 4C 24 24 8B 74 24 14 8B 5C 24 18"))
        Mem::MidHook("hacking pick FOV", a, 6, &OnPickFov, Mem::MidMode::OriginalBefore);

    // Provisional values until the game reports its real resolution.
    int w = s.width > 0 ? s.width : DesktopRes().first;
    int h = s.height > 0 ? s.height : DesktopRes().second;
    OnResolution(w, h, true);
    LOG("Widescreen: installed");
}
