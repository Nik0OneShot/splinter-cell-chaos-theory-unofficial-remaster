// The Niko plush: CLUSTERFUCK's unlock. Once CLUSTERFUCK is beaten, Sam starts every mission with a Niko plush in
// his inventory, with no end to them: take it out, aim and throw it like a grenade. It meows when it leaves his hand
// and when it hits something, and guards near where it lands hear it and come to look.
//
// The item is the game's leftover chemical flare (EchelonIngredient.EChemicalFlare, from the first game): an
// EInventoryItem with bIsProjectile, so the player controller's own throw (s_Throw: aim, then HandItem.Throw) handles
// it like any grenade, and the quick inventory lists it like any item that isn't a weapon. Chaos Theory never shipped
// its mesh (Ingredients_obj.SCX_ChemicalFlare is in no map), so the engine moves an invisible actor and this file
// draws the plush there. The gameplay side (giving it, topping it up, spotting throws and bounces, the guards' noise)
// is Lua in scripting.cpp's prelude; it hands over the plushes to draw every tick (SetItems) and asks for meows.
//
// Drawing: the plush (System\SCCTRemaster_Award.pak, made by tools\pack_plush.py) is drawn with the game's own
// camera matrix, found among the vertex shader constants: world geometry is drawn with the view-projection matrix,
// and that is the one 4x4 block whose w row is the camera's forward axis (unit length) through the camera's position.
// The renderer (as captured on the shader model 1/2 path the game runs here) draws the scene straight into the back
// buffer: a first pass of the world with depth (its colour feeds the glow), the lights' shadow masks into the alpha
// channel, full-screen passes with the depth test off (the glow, and the colour reset to black), then the colour
// passes: every object again, base colour, then per light an add weighted by the alpha mask (DESTALPHA) and a
// multiply (DESTCOLOR), depth-tested against the first pass. Drawn in the first pass the plush was a black silhouette:
// its depth kept the world off its pixels and the reset blacked its colour (in electromagnetic vision only its glow
// showed). So it's drawn into the back buffer once this frame has had the scene's camera matrix AND a depth-test-off
// draw there after it (the reset): at the next vertex shader constants set with colour writes and a depth test on,
// writing depth (the world drawn after it behind it fails the depth test) but not alpha (the light masks), and only at
// an opaque draw (the blended effects right after the reset feed a second glow). Frames without that pattern get it
// at Present, if the frame had the scene's camera matrix (menus don't: no plush over them). The vision modes (night
// vision, thermal, electromagnetic; EGoggle.CurrentMode, from Lua) make their final image from copies of the back
// buffer (StretchRect) that their filters read (from the frame captures and the game's shaders):
// - thermal (CurrentMode 10) draws its scene encoded (FBC1CDB9: heat, texture x vertex colour x 4, in green, heat
//   sources adding to green, nearness, 1 - view depth / range (c23, c24.y of its vertex shaders), in blue), clears
//   the depth buffer, copies it, and its filter (02934192: texreg2gb) looks the picture up in its heat palette (its
//   stage 2 texture: purple, blue, cyan, green, yellow, orange, red along green) by green and blue. The plush is drawn
//   just before that depth clear (depth-tested): blue its nearness (the game's range, read back at its scene's draws),
//   green hot, yellow-green to orange-yellow, hottest facing you, breathing.
// - night vision (CurrentMode 11) draws its scene like normal vision (a first pass, copied for its glow, full-screen
//   passes, the colour passes), copies it again, makes each pixel's brightest channel its brightness (4DFE8ECE) and
//   amplifies that into its grey-green (07B23843). The plush is drawn just before that second copy (the first copy
//   after the full-screen passes) in its own colours, dimmer, as the scene is dark there and gets amplified.
// - electromagnetic (42) draws everything that isn't electronic one flat dark blue (621F1866) and shows a blurred
//   copy: the plush goes in before that copy (the first one), flat dark blue too.
// Until dev.142 night vision and thermal were the wrong way round here (from the first captures): night vision got
// thermal's grey heat (a white blob, with a halo) and thermal got the palette (which looked right, so it stayed).
// Which goggles a frame has is told by what it draws (CurrentMode can lag: it said 11 through a night vision session).
// Goggle frames without those get it at Present, in its own colours. It's drawn with its own small vertex and pixel
// shader (plush_shaders.h: texture x (ambient + key light), tint in the constants): drawn with the fixed-function
// pipeline it came out black, and DXVK built a new pipeline for every fixed-function state (the tint per vision mode),
// over a second each time. The fixed-function path stays as a fallback if the shaders can't be made. The game's projection is kept from frames
// where the camera matrix is found (P = camera frame x view-projection), so frames where it isn't still get one
// (PlayerCalcView's camera x P). A depth clear of the full-size target lets it be drawn again. The one in Sam's hand is
// drawn at his weapon bone (from Lua): a held item isn't attached to the hand until it's thrown. Its brightness
// follows the engine's light level where it is.
#include "pch.h"
#include "features.h"
#include <mmsystem.h>
#include "plush_shaders.h"
#include "d3d9_capture.h"

extern HMODULE g_self;

namespace
{
    struct Vtx { float p[3], n[3], uv[2]; };
    constexpr DWORD kFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

    std::vector<Vtx> g_verts;
    std::vector<uint16_t> g_idx;
    int g_texW = 0, g_texH = 0;
    std::vector<uint8_t> g_texBGRA;
    int g_rate = 22050;
    std::vector<int16_t> g_meow;
    bool g_loaded = false;

    // Device resources.
    IDirect3DDevice9* g_dev = nullptr;
    IDirect3DVertexBuffer9* g_vb = nullptr;
    IDirect3DIndexBuffer9* g_ib = nullptr;
    IDirect3DTexture9* g_tex = nullptr;
    IDirect3DStateBlock9* g_state = nullptr;
    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9* g_ps = nullptr;
    bool g_inDraw = false;       // our own SetVertexShaderConstantF calls come back through the hook
    bool g_failed = false;
    bool g_warmed = false;       // the plush's draw was made once on this device (see Draw)
    bool g_soundWarm = false;    // PlaySound was used once (its first use loads the sound system)

    // What to draw (from Lua, every tick).
    struct Item { void* actor; float bright; float target; float lum; float samLum; bool hand; float loc[3]; int yaw; };
    std::vector<Item> g_items;
    void* g_sam = nullptr;
    DWORD g_lightTick = 0, g_fadeTick = 0;

    // Thermal's scene constants (see the top of the file), read back at its draws: c0 and c22 (the vertex colour,
    // c0.xzxz x c22), c23 (view depth plane) and c24 (.y: 1 / range). Its vertex shaders are told by their last lines.
    // Thermal's palette (the stage 2 texture of its filter, 02934192: tex t0; tex t1; texreg2gb t2, t0; mul_x2).
    const DWORD kHeatFilter[] = { 0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000042, 0xB00F0001, 0x00000046, 0xB00F0002, 0xB0E40000,
                                0x00000005, 0x810F0000, 0xB0E40002, 0xB0E40001, 0x0000FFFF };
    void* g_filterShaders[16] = {};
    int g_filterShaderCount = 0;
    bool g_curIsFilter = false, g_palTried = false, g_knotsLogged = false;
    void* g_palTex = nullptr;          // the texture last read (compared only)
    uint32_t g_palHash = 0;            // and what was in it
    unsigned g_palAge = 0, g_palLogs = 0;
    std::vector<float> g_pal;   // brightness 0..1, g_palW x g_palH
    std::vector<float> g_palRGB;   // its colours, 3 a texel
    float g_center[3] = { 0, 0, 13 };   // the model's middle (its bounds)
    int g_palW = 0, g_palH = 0;
    // F10 frame captures: the pixels at the plushes and at Sam, at each copy of the back buffer and at Present.
    int g_probes = 0, g_copyIndex = 0;
    void* g_heatShaders[64] = {};
    int g_heatShaderCount = 0;
    void* g_curVS = nullptr;
    bool g_curIsHeat = false, g_heatFrame = false, g_heatHave = false, g_heatLogged = false;
    float g_heatC0[4], g_heatC22[4], g_heatC23[4], g_heatC24[4];

    // The camera matrix of this frame, and the projection kept from the last exact match.
    float g_proj[16];
    bool g_haveProj = false;
    unsigned g_rebuilt = 0, g_rejected = 0;
    float g_vp[16];
    bool g_haveVP = false, g_camFetched = false, g_haveCam = false, g_drawn = false;
    float g_camPos[3], g_camFwd[3];
    int g_vpLayout = 0;   // bit per layout seen (for the log)
    unsigned g_vpHits = 0, g_frames = 0, g_drawsHdr = 0, g_drawsPresent = 0, g_noVP = 0;

    // The render target being drawn to, and the game's depth test when the camera matrix was set.
    bool g_rtFull = true, g_rtHdr = false, g_rtBack = false;
    bool g_vpThisFrame = false;   // the scene's camera matrix was matched this frame
    bool g_sceneSeen = false;     // a depth-tested draw into the back buffer this frame (the scene has started)
    bool g_zOffAfterScene = false;   // ...and after it a draw into the back buffer with the depth test off
    bool g_beforeCopy = false;    // drawing into a goggle frame before the game copies it (see OnStretchRect)
    bool g_drawnCopy = false;     // ...and that was done this frame
    unsigned g_drawsCopy = 0;
    bool g_resetSeen = false;     // ...and after it, a draw into the back buffer with the depth test off
    int g_vision = 5, g_visionLogged = -1;   // EGoggle.CurrentMode, from Lua
    // Which goggles a frame is drawn with, from what it draws: thermal's scene shaders, or electromagnetic's; goggles on
    // with neither is night vision. (CurrentMode said 11 all through one night vision session.)
    bool g_frameHeat = false, g_frameEmf = false, g_curIsEmf = false;
    int g_mode = 5;          // the look of the draw in progress (5 normal, 10 thermal, 11 night vision, 42 EMF)
    int g_modeLogs = 0, g_modeLogged = -1;
    void* g_emfShaders[16] = {};
    int g_emfShaderCount = 0;
    // Electromagnetic vision's scene pixel shader (621F1866).
    const DWORD kEmfScene[] = { 0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000042, 0xB00F0001, 0x00000002, 0x80170000, 0xB0E40000,
                                0xB0E40001, 0x00000004, 0x80080000, 0x80AA0000, 0xA0AA0000, 0xA0FF0000, 0x00000005, 0x80070000,
                                0x80FF0000, 0x90E40000, 0x40000001, 0x80080000, 0xA0FF0001, 0x0000FFFF };
    int FrameMode()
    {
        if (g_frameHeat) return 10;
        if (g_frameEmf) return 42;
        return g_vision == 5 ? 5 : 11;
    }
    UINT g_bbW = 0, g_bbH = 0;
    DWORD g_zEnable = D3DZB_TRUE, g_zFunc = D3DCMP_LESSEQUAL;
    bool g_haveZ = false;
    int g_snapshots = 0;
    unsigned g_relaxed = 0;

    void Release()
    {
        g_warmed = false;
        if (g_state) { g_state->Release(); g_state = nullptr; }
        if (g_vs) { g_vs->Release(); g_vs = nullptr; }
        if (g_ps) { g_ps->Release(); g_ps = nullptr; }
        if (g_tex) { g_tex->Release(); g_tex = nullptr; }
        if (g_ib) { g_ib->Release(); g_ib = nullptr; }
        if (g_vb) { g_vb->Release(); g_vb = nullptr; }
        g_dev = nullptr;
        g_failed = false;
    }

    bool Load()
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(g_self, self, MAX_PATH);
        std::wstring path = self;
        path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"SCCTRemaster_Award.pak";
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        std::vector<uint8_t> d;
        fseek(f, 0, SEEK_END);
        d.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        const bool read = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        size_t o = 0;
        auto take = [&](void* out, size_t n) { if (o + n > d.size()) return false; memcpy(out, d.data() + o, n); o += n; return true; };
        char magic[4];
        uint32_t ver = 0, nv = 0, ni = 0, w = 0, h = 0, rate = 0, ns = 0;
        if (!read || !take(magic, 4) || memcmp(magic, "NIKO", 4) || !take(&ver, 4) || ver != 1 || !take(&nv, 4) || !take(&ni, 4) ||
            nv == 0 || nv > 65535 || ni == 0 || ni % 3 || ni > 3000000)
            return false;
        g_verts.resize(nv);
        g_idx.resize(ni);
        if (!take(g_verts.data(), nv * sizeof(Vtx)) || !take(g_idx.data(), ni * 2) || !take(&w, 4) || !take(&h, 4) || w == 0 || h == 0 ||
            w > 4096 || h > 4096)
            return false;
        g_texW = int(w); g_texH = int(h);
        g_texBGRA.resize(size_t(w) * h * 4);
        if (!take(g_texBGRA.data(), g_texBGRA.size()) || !take(&rate, 4) || !take(&ns, 4) || ns > 22050u * 10) return false;
        g_rate = int(rate);
        g_meow.resize(ns);
        if (!take(g_meow.data(), ns * 2)) return false;
        for (uint16_t i : g_idx)
            if (i >= nv) return false;
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (const Vtx& v : g_verts)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v.p[k]); hi[k] = std::max(hi[k], v.p[k]); }
        for (int k = 0; k < 3; ++k) g_center[k] = 0.5f * (lo[k] + hi[k]);
        return true;
    }

    // ----- drawing -----

    bool CreateResources(IDirect3DDevice9* dev)
    {
        if (g_dev == dev && g_vb && g_ib && g_tex && g_state) return true;
        if (g_failed && g_dev == dev) return false;
        Release();
        g_dev = dev;
        g_failed = true;
        void* p = nullptr;
        const UINT vbytes = UINT(g_verts.size() * sizeof(Vtx)), ibytes = UINT(g_idx.size() * 2);
        if (FAILED(dev->CreateVertexBuffer(vbytes, D3DUSAGE_WRITEONLY, kFVF, D3DPOOL_MANAGED, &g_vb, nullptr)) ||
            FAILED(g_vb->Lock(0, vbytes, &p, 0)))
        { LOG("Plush: [!] couldn't create the vertex buffer"); return false; }
        memcpy(p, g_verts.data(), vbytes);
        g_vb->Unlock();
        if (FAILED(dev->CreateIndexBuffer(ibytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ib, nullptr)) ||
            FAILED(g_ib->Lock(0, ibytes, &p, 0)))
        { LOG("Plush: [!] couldn't create the index buffer"); return false; }
        memcpy(p, g_idx.data(), ibytes);
        g_ib->Unlock();
        // The texture with a box-filtered mip chain.
        int levels = 1;
        for (int s = std::max(g_texW, g_texH); s > 1; s >>= 1) ++levels;
        if (FAILED(dev->CreateTexture(UINT(g_texW), UINT(g_texH), UINT(levels), 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr)))
        { LOG("Plush: [!] couldn't create the texture"); return false; }
        std::vector<uint8_t> cur = g_texBGRA, next;
        int w = g_texW, h = g_texH;
        for (int l = 0; l < levels; ++l)
        {
            D3DLOCKED_RECT r;
            if (SUCCEEDED(g_tex->LockRect(UINT(l), &r, nullptr, 0)))
            {
                for (int y = 0; y < h; ++y) memcpy(static_cast<uint8_t*>(r.pBits) + size_t(y) * r.Pitch, cur.data() + size_t(y) * w * 4, size_t(w) * 4);
                g_tex->UnlockRect(UINT(l));
            }
            const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            next.assign(size_t(nw) * nh * 4, 0);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        int sum = 0;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                sum += cur[(size_t(std::min(h - 1, y * 2 + dy)) * w + std::min(w - 1, x * 2 + dx)) * 4 + c];
                        next[(size_t(y) * nw + x) * 4 + c] = uint8_t(sum / 4);
                    }
            cur.swap(next);
            w = nw; h = nh;
        }
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state))) { LOG("Plush: [!] couldn't create a state block"); return false; }
        if (FAILED(dev->CreateVertexShader(kPlushVS, &g_vs)) || FAILED(dev->CreatePixelShader(kPlushPS, &g_ps)))
        {
            if (g_vs) { g_vs->Release(); g_vs = nullptr; }
            if (g_ps) { g_ps->Release(); g_ps = nullptr; }
            LOG("Plush: [!] couldn't create its shaders: fixed-function drawing instead");
        }
        g_failed = false;
        LOG("Plush: device resources ready (%d vertices, %d triangles, %dx%d texture)", int(g_verts.size()), int(g_idx.size() / 3), g_texW, g_texH);
        return true;
    }

    // Unreal's rotation matrix (row vectors): rows are the X, Y, Z axes of the rotated frame.
    void WorldMatrix(const float loc[3], const int rot[3], float m[16])
    {
        const float k = 3.14159265f / 32768.0f;
        const float sp = std::sin(rot[0] * k), cp = std::cos(rot[0] * k);
        const float sy = std::sin(rot[1] * k), cy = std::cos(rot[1] * k);
        const float sr = std::sin(rot[2] * k), cr = std::cos(rot[2] * k);
        const float r[16] = { cp * cy, cp * sy, sp, 0,
                              sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp, 0,
                              -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp, 0,
                              loc[0], loc[1], loc[2], 1 };
        memcpy(m, r, sizeof(r));
    }

    struct Pose { float loc[3]; int rot[3]; bool ok; };

    Pose ReadPose(void* a)
    {
        Pose p{};
        __try
        {
            const uint8_t* b = static_cast<const uint8_t*>(a);
            if (*reinterpret_cast<const uint32_t*>(b + 0x28) & 4) return p;   // bDeleteMe (bHidden doesn't matter: see SetItems)
            memcpy(p.loc, b + 0xE8, 12);
            memcpy(p.rot, b + 0xF4, 12);
            p.ok = std::isfinite(p.loc[0]) && std::isfinite(p.loc[1]) && std::isfinite(p.loc[2]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { p.ok = false; }
        return p;
    }

    // Where the first plush lands on screen, the render target, depth buffer, viewport and depth test: for the log.
    void Snapshot(IDirect3DDevice9* dev, const char* where)
    {
        if (g_snapshots >= 3 || g_items.empty()) return;
        size_t shown = 0;   // the first one on screen (else the first)
        for (size_t i = 0; i < g_items.size(); ++i)
        {
            Pose q = ReadPose(g_items[i].actor);
            if (g_items[i].hand) memcpy(q.loc, g_items[i].loc, sizeof(q.loc));
            float w = 0, x = 0, y = 0;
            for (int j = 0; j < 4; ++j)
            {
                const float v = q.loc[0] * g_vp[j] + q.loc[1] * g_vp[4 + j] + q.loc[2] * g_vp[8 + j] + g_vp[12 + j];
                if (j == 0) x = v; else if (j == 1) y = v; else if (j == 3) w = v;
            }
            if (w > 1 && std::fabs(x / w) < 1 && std::fabs(y / w) < 1) { shown = i; break; }
        }
        const Item& item = g_items[shown];
        ++g_snapshots;
        char rt[64] = "?", ds[64] = "none";
        IDirect3DSurface9* s = nullptr;
        D3DSURFACE_DESC d;
        if (SUCCEEDED(dev->GetRenderTarget(0, &s)) && s)
        {
            IDirect3DSurface9* bb = nullptr;
            const bool back = SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb == s;
            if (bb) bb->Release();
            if (SUCCEEDED(s->GetDesc(&d))) sprintf_s(rt, "%ux%u fmt %d%s", d.Width, d.Height, int(d.Format), back ? " (back buffer)" : " (not the back buffer)");
            s->Release();
        }
        s = nullptr;
        if (SUCCEEDED(dev->GetDepthStencilSurface(&s)) && s) { if (SUCCEEDED(s->GetDesc(&d))) sprintf_s(ds, "%ux%u fmt %d", d.Width, d.Height, int(d.Format)); s->Release(); }
        D3DVIEWPORT9 vp = {};
        dev->GetViewport(&vp);
        Pose p = ReadPose(item.actor);
        if (item.hand) memcpy(p.loc, item.loc, sizeof(p.loc));
        LOG("Plush: brightness %.2f (light level here %.1f, Sam's %.1f)%s", item.bright, item.lum, item.samLum, item.hand ? " (in his hand)" : "");
        float c[4] = {};
        for (int j = 0; j < 4; ++j) c[j] = p.loc[0] * g_vp[j] + p.loc[1] * g_vp[4 + j] + p.loc[2] * g_vp[8 + j] + g_vp[12 + j];
        LOG("Plush: drawing %s: target %s, depth %s, viewport %lu,%lu %lux%lu z %.2f-%.2f, depth test %lu/%lu%s; plush at (%.0f,%.0f,%.0f) -> "
            "clip w %.1f, x %.2f y %.2f z %.4f; camera at (%.0f,%.0f,%.0f)", where, rt, ds, vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ,
            g_zEnable, g_zFunc, g_haveZ ? "" : " (assumed)", p.loc[0], p.loc[1], p.loc[2], c[3], c[3] ? c[0] / c[3] : 0.0f,
            c[3] ? c[1] / c[3] : 0.0f, c[3] ? c[2] / c[3] : 0.0f, g_camPos[0], g_camPos[1], g_camPos[2]);
    }

    // Sam's light level (LuminosityFactor +264h) when the point is within 3 m of him, else 0.
    float SamLightNear(void* sam, const float loc[3])
    {
        if (!sam) return 0.0f;
        __try
        {
            const float* sl = reinterpret_cast<const float*>(static_cast<const uint8_t*>(sam) + 0xE8);
            const float dx = loc[0] - sl[0], dy = loc[1] - sl[1], dz = loc[2] - sl[2];
            if (dx * dx + dy * dy + dz * dz >= 300.0f * 300.0f) return 0.0f;
            return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(sam) + 0x264);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
    }

    // The first draw of this kind makes the driver (DXVK) build its pipeline: over a second the first time Niko was
    // taken out. So on each device it's done once early, with the plush behind the camera (nothing drawn), as soon as
    // the scene's camera matrix turns up.
    void DrawInner(IDirect3DDevice9* dev, bool intoHdr);
    void Mul(const float* a, const float* b, float* out);
    unsigned g_slowDraws = 0;

    void Draw(IDirect3DDevice9* dev, bool intoHdr)
    {
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        g_inDraw = true;
        DrawInner(dev, intoHdr);
        g_inDraw = false;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
        if (ms > 8.0 && g_slowDraws < 10) { ++g_slowDraws; LOG("Plush: a slow draw: %.1f ms (%d plushes)", ms, int(g_items.size())); }
    }

    void DrawInner(IDirect3DDevice9* dev, bool intoHdr)
    {
        g_drawn = true;
        const bool warmOnly = g_items.empty();
        if (warmOnly && g_warmed) return;
        if (!g_haveVP) { if (!warmOnly) ++g_noVP; return; }
        if (!CreateResources(dev)) return;
        if (!warmOnly) Snapshot(dev, g_beforeCopy ? "before the goggles' copy" : intoHdr ? "in the scene" : "at Present");
        g_state->Capture();
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(kFVF);
        dev->SetStreamSource(0, g_vb, 0, sizeof(Vtx));
        dev->SetIndices(g_ib);
        dev->SetTexture(0, g_tex);
        for (DWORD s = 1; s < 8; ++s) dev->SetTexture(s, nullptr);
        const std::pair<D3DRENDERSTATETYPE, DWORD> rs[] = {
            { D3DRS_ZENABLE, g_settings.nikoPlushDebug >= 2 ? DWORD(D3DZB_FALSE) : g_zEnable }, { D3DRS_ZWRITEENABLE, TRUE },
            { D3DRS_ZFUNC, g_zFunc }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_LIGHTING, TRUE }, { D3DRS_NORMALIZENORMALS, TRUE }, { D3DRS_SPECULARENABLE, FALSE }, { D3DRS_COLORVERTEX, FALSE },
            { D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL }, { D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL },
            { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE },
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0x7 }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 },
            { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_SHADEMODE, D3DSHADE_GOURAUD }, { D3DRS_DEPTHBIAS, 0 }, { D3DRS_SLOPESCALEDEPTHBIAS, 0 },
            { D3DRS_VERTEXBLEND, D3DVBF_DISABLE }, { D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE }, { D3DRS_TWOSIDEDSTENCILMODE, FALSE },
            { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_MULTISAMPLEANTIALIAS, TRUE }, { D3DRS_TEXTUREFACTOR, 0xFFFFFFFF } };
        for (const auto& [k, v] : rs) dev->SetRenderState(k, v);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        // (Fixed-function fallback) the vision modes: 11 night vision (green, brighter), 10 thermal (a cold blue), 42 EMF.
        DWORD tint = 0;
        if (g_mode == 11) tint = D3DCOLOR_XRGB(150, 255, 150);
        else if (g_mode == 10) tint = D3DCOLOR_XRGB(20, 30, 90);
        else if (g_mode == 42) tint = D3DCOLOR_XRGB(110, 110, 120);
        if (tint)
        {
            dev->SetRenderState(D3DRS_TEXTUREFACTOR, tint);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, g_mode == 11 ? D3DTOP_MODULATE2X : D3DTOP_MODULATE);
            dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
            dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
            dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
            dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
        else
        {
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
        D3DMATRIX ident = {};
        ident._11 = ident._22 = ident._33 = ident._44 = 1.0f;
        D3DMATRIX vp;
        memcpy(&vp, g_vp, sizeof(vp));
        dev->SetTransform(D3DTS_VIEW, &ident);
        dev->SetTransform(D3DTS_PROJECTION, &vp);
        D3DMATERIAL9 mat = {};
        mat.Diffuse = mat.Ambient = { 1, 1, 1, 1 };
        dev->SetMaterial(&mat);
        // A key light from above and in front of the camera, and the rest as ambient.
        D3DLIGHT9 light = {};
        light.Type = D3DLIGHT_DIRECTIONAL;
        const float lx = g_camFwd[0] * 0.5f, ly = g_camFwd[1] * 0.5f, lz = -1.0f;
        const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
        light.Direction = { lx / ll, ly / ll, lz / ll };
        dev->LightEnable(0, TRUE);
        for (DWORD l = 1; l < 8; ++l) dev->LightEnable(l, FALSE);
        // Night vision amplifies its dark scene: the plush drawn into it is dimmer to match.
        const float hdr = (intoHdr ? g_settings.nikoPlushHdrScale : 1.0f) * (g_beforeCopy && g_mode == 11 ? 0.4f : 1.0f);
        const bool shaders = g_vs && g_ps;
        const float mul3[3] = { 1, 1, 1 };   // (the vision modes: the shader, see below)

        if (shaders)
        {
            dev->SetVertexShader(g_vs);
            dev->SetPixelShader(g_ps);
            const float toLight[4] = { -light.Direction.x, -light.Direction.y, -light.Direction.z, 0 };
            const float solid[4] = { 1, 0, 1, g_settings.nikoPlushDebug == 3 ? 1.0f : 0.0f };   // debug: plain magenta
            // Thermal vision: a heat palette, hottest where it faces the camera, with a slow breathing pulse (alive!).
            const float pulse = 0.06f * std::sin(float(GetTickCount64() % 100000) * 0.001f * 2.2f);
            // Drawn before the goggles' copy, the game's filter does the look (thermal: the shader encodes heat).
            const float thermal[4] = { 0, pulse, 0, 0 };
            const float cam[4] = { g_camPos[0], g_camPos[1], g_camPos[2], 1 };
            dev->SetPixelShaderConstantF(2, toLight, 1);
            dev->SetPixelShaderConstantF(3, solid, 1);
            dev->SetPixelShaderConstantF(4, thermal, 1);
            const float emf[4] = { g_mode == 42 ? 1.0f : 0.0f, 0, 0, 0 };
            dev->SetPixelShaderConstantF(7, emf, 1);

            dev->SetVertexShaderConstantF(8, cam, 1);
        }
        // One draw at a world matrix with brightness b.
        auto drawAt = [&](const float* wm, float b)
        {
            if (shaders)
            {
                float wvp[16];
                Mul(wm, g_vp, wvp);
                dev->SetVertexShaderConstantF(0, wvp, 4);
                dev->SetVertexShaderConstantF(4, wm, 4);
                // Thermal: blue is nearness, 1 - view depth / range (the game's range, its c24.y; the depth from the
                // camera); green is heat: 0.70 at its coolest to 0.88 facing you (the palette's yellow-green to
                // orange-yellow at the bottom rows; Sam's body is about 0.8).
                if (g_beforeCopy && g_mode == 10)
                {
                    const float depth = (wm[12] - g_camPos[0]) * g_camFwd[0] + (wm[13] - g_camPos[1]) * g_camFwd[1] + (wm[14] - g_camPos[2]) * g_camFwd[2];
                    const float k = g_heatHave && g_heatC24[1] > 1e-6f && g_heatC24[1] < 0.1f ? g_heatC24[1] : 1.0f / 4000.0f;
                    const float nearness = 1.0f - std::clamp(depth * k, 0.0f, 1.0f);
                    const float enc[4] = { 1.0f, nearness, 0.62f, 0.26f };
                    dev->SetPixelShaderConstantF(5, enc, 1);
                }
                else
                {
                    const float off[4] = { 0, 0, 0, 0 };
                    dev->SetPixelShaderConstantF(5, off, 1);
                }
                const float amb[4] = { 0.6f * b * mul3[0], 0.6f * b * mul3[1], 0.6f * b * mul3[2], 1 };
                const float key[4] = { 0.45f * b * mul3[0], 0.45f * b * mul3[1], 0.45f * b * mul3[2], 1 };
                dev->SetPixelShaderConstantF(0, amb, 1);
                dev->SetPixelShaderConstantF(1, key, 1);
            }
            else
            {
                D3DMATRIX world;
                memcpy(&world, wm, sizeof(world));
                dev->SetTransform(D3DTS_WORLD, &world);
                light.Diffuse = { 0.4f * b, 0.4f * b, 0.4f * b, 1 };
                dev->SetLight(0, &light);
                const DWORD amb = DWORD(std::clamp(0.7f * b, 0.0f, 1.0f) * 255.0f);
                dev->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(amb, amb, amb));
            }
            dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, UINT(g_verts.size()), 0, UINT(g_idx.size() / 3));
        };
        if (!g_warmed)
        {
            const float behind[3] = { g_camPos[0] - g_camFwd[0] * 5000.0f, g_camPos[1] - g_camFwd[1] * 5000.0f, g_camPos[2] - g_camFwd[2] * 5000.0f };
            const int norot[3] = { 0, 0, 0 };
            float wm[16];
            WorldMatrix(behind, norot, wm);
            LARGE_INTEGER t0, t1, f;
            QueryPerformanceCounter(&t0);
            drawAt(wm, 0.5f);
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&f);
            g_warmed = true;
            LOG("Plush: drawing warmed up (%.1f ms, %s, %s)", double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart),
                g_beforeCopy ? "before the goggles' copy" : intoHdr ? "in the scene" : "at Present", shaders ? "shaders" : "fixed function");
        }
        int drawn = 0;
        for (const Item& it : g_items)
        {
            Pose p = ReadPose(it.actor);
            if (it.hand) { memcpy(p.loc, it.loc, sizeof(p.loc)); p.rot[0] = 0; p.rot[1] = it.yaw; p.rot[2] = 0; p.ok = true; }
            if (!p.ok) continue;
            float wm[16];
            WorldMatrix(p.loc, p.rot, wm);
            drawAt(wm, it.bright * hdr);
            ++drawn;
        }
        g_state->Apply();
        if (drawn) { if (g_beforeCopy) ++g_drawsCopy; else if (intoHdr) ++g_drawsHdr; else ++g_drawsPresent; }
    }

    void Mul(const float* a, const float* b, float* out)   // row-vector 4x4: out = a * b
    {
        float r[16];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        memcpy(out, r, sizeof(r));
    }

    // The camera's frame: rows forward, right, up, position (no roll), and its inverse (the view matrix).
    void CameraFrame(float* m, float* inv)
    {
        const float* f = g_camFwd;
        const float yaw = std::atan2(f[1], f[0]), pitch = std::asin(std::clamp(f[2], -1.0f, 1.0f));
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        const float X[3] = { cp * cy, cp * sy, sp }, Y[3] = { -sy, cy, 0 }, Z[3] = { -sp * cy, -sp * sy, cp };
        const float* p = g_camPos;
        const float m2[16] = { X[0], X[1], X[2], 0, Y[0], Y[1], Y[2], 0, Z[0], Z[1], Z[2], 0, p[0], p[1], p[2], 1 };
        memcpy(m, m2, sizeof(m2));
        const float tx = -(p[0] * X[0] + p[1] * X[1] + p[2] * X[2]);
        const float ty = -(p[0] * Y[0] + p[1] * Y[1] + p[2] * Y[2]);
        const float tz = -(p[0] * Z[0] + p[1] * Z[1] + p[2] * Z[2]);
        const float i2[16] = { X[0], Y[0], Z[0], 0, X[1], Y[1], Z[1], 0, X[2], Y[2], Z[2], 0, tx, ty, tz, 1 };
        memcpy(inv, i2, sizeof(i2));
    }

    void FetchCamera()
    {
        g_camFetched = true;
        g_haveCam = Editor::RenderCamera(g_camPos, g_camFwd);
    }

    // A 4x4 block of shader constants that is world geometry's view-projection: its w row is the camera's forward
    // axis (scaled) through the camera's position. Layout 0: the registers are the rows dp4'd with the vertex (the
    // matrix transposed, as m4x4 uses it); layout 1: the registers are the matrix's rows (w is their 4th column).
    // A view-projection maps a point straight ahead of the camera to the middle of the screen, and a point a little to
    // its right a little right of the middle. Matrices that share the camera's w row but aren't the screen projection
    // (texture projections the vision modes use: the plush landed far off screen with them) fail this.
    bool LooksLikeScreen(const float* m, float tolerance)
    {
        const float yaw = std::atan2(g_camFwd[1], g_camFwd[0]);
        const float right[3] = { -std::sin(yaw), std::cos(yaw), 0 };
        auto ndc = [&](const float* p, float out[3])
        {
            float c[4];
            for (int j = 0; j < 4; ++j) c[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
            if (!(c[3] > 1e-3f)) return false;
            out[0] = c[0] / c[3]; out[1] = c[1] / c[3]; out[2] = c[2] / c[3];
            return true;
        };
        float ahead[3], side[3], a[3], b[3];
        for (int k = 0; k < 3; ++k)
        {
            ahead[k] = g_camPos[k] + g_camFwd[k] * 1000.0f;
            side[k] = ahead[k] + right[k] * 200.0f;
        }
        if (!ndc(ahead, a) || !ndc(side, b)) return false;
        return std::fabs(a[0]) < tolerance && std::fabs(a[1]) < tolerance && a[2] > -0.01f && a[2] < 1.01f &&
               b[0] - a[0] > 0.02f && b[0] - a[0] < 2.0f;
    }

    bool TryMatrix(const float* r)
    {
        for (int layout = 0; layout < 2; ++layout)
        {
            float w[4];
            if (layout == 0) { w[0] = r[12]; w[1] = r[13]; w[2] = r[14]; w[3] = r[15]; }
            else { w[0] = r[3]; w[1] = r[7]; w[2] = r[11]; w[3] = r[15]; }
            const float n = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (!(n > 1e-6f)) continue;
            const float along = (w[0] * g_camFwd[0] + w[1] * g_camFwd[1] + w[2] * g_camFwd[2]) / n;
            const float dist = (w[0] * g_camPos[0] + w[1] * g_camPos[1] + w[2] * g_camPos[2] + w[3]) / n;
            const bool strict = along >= 0.995f && std::fabs(dist) <= 4.0f;
            // The throw's aiming view (and camera shake) moves the camera a little after PlayerCalcView: a w row of unit
            // length (a perspective projection's) close to that camera is accepted too.
            const bool unit = std::fabs(n - 1.0f) < 0.02f;
            const bool loose = along >= 0.97f && std::fabs(dist) <= 64.0f && unit;
            const bool wide = along >= 0.9f && std::fabs(dist) <= 300.0f && unit && g_rtFull && !g_haveVP;
            if (!strict && !loose && !wide) continue;
            // As a row-vector D3D matrix (v * M).
            float m[16];
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    m[i * 4 + j] = layout == 0 ? r[j * 4 + i] : r[i * 4 + j];
            if (!LooksLikeScreen(m, strict ? 0.1f : 0.35f)) { ++g_rejected; continue; }
            if (!strict) ++g_relaxed;
            memcpy(g_vp, m, sizeof(m));
            if (!(g_vpLayout & (1 << layout)))
            {
                LOG("Plush: the game's camera matrix found in the shader constants (layout %d)", layout);
                g_vpLayout |= 1 << layout;
            }
            g_haveVP = true;
            g_vpThisFrame = true;
            ++g_vpHits;
            if (strict)
            {
                float frame[16], view[16];
                CameraFrame(frame, view);
                Mul(frame, g_vp, g_proj);   // VP = view * P, so P = frame * VP
                g_haveProj = true;
            }
            return true;
        }
        return false;
    }

    // ----- sound -----

    std::vector<uint8_t> g_wav[2];
    int g_wavNext = 0;
    uint32_t g_rng = 0x9E3779B9u;
    float Rand(float a, float b) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return a + (b - a) * float(g_rng & 0xFFFF) / 65535.0f; }
}

bool Plush::Available() { return g_loaded; }

bool Plush::Active()
{
    if (!g_loaded || g_settings.nikoPlush <= 0) return false;
    return g_settings.nikoPlush >= 2 || Clusterfuck::Victories() > 0;
}

void Plush::SetItems(void* sam, void* const* actors, int count, void* handActor, const float* hand, int handYaw, int vision)
{
    g_sam = sam;
    g_vision = vision;
    if (vision != g_visionLogged) { g_visionLogged = vision; g_snapshots = 0; LOG("Plush: vision mode %d", vision); }
    if (count <= 0) { g_items.clear(); return; }
    // Brightness from the light level where each one is, five times a second, eased over a quarter of a second or so.
    const DWORD now = GetTickCount();
    const bool relight = now - g_lightTick >= 200 || g_items.size() != size_t(count);
    const float ease = std::clamp(float(now - g_fadeTick) / 250.0f, 0.0f, 1.0f);
    g_fadeTick = now;
    std::vector<Item> next;
    next.reserve(size_t(count));
    for (int i = 0; i < count; ++i)
    {
        Item it{ actors[i], 1.0f, 1.0f, 0.0f, 0.0f, false, { 0, 0, 0 }, 0 };
        bool found = false;
        for (const Item& old : g_items)
            if (old.actor == it.actor) { it = old; found = true; break; }
        it.hand = hand && actors[i] == handActor;
        if (it.hand) { memcpy(it.loc, hand, sizeof(it.loc)); it.yaw = handYaw; }
        if (relight || !found)
        {
            // The level's lamps where it is (GetLuminosity counts point and spot lights only, not the sun or the zone's
            // ambient light), and in his hand at least Sam's own light level (every light on him). Not Sam's for one
            // on the ground near him: it got brighter and darker as he walked about.
            Pose p = ReadPose(it.actor);
            if (it.hand) { memcpy(p.loc, it.loc, sizeof(p.loc)); p.ok = true; }
            float lum = 0, samLum = 0;
            if (p.ok) Perception::LightAt(sam, p.loc, lum);
            if (p.ok && it.hand) samLum = SamLightNear(sam, p.loc);
            if (!std::isfinite(lum)) lum = 0;
            if (!std::isfinite(samLum)) samLum = 0;
            it.lum = lum;
            it.samLum = samLum;
            it.target = std::clamp(g_settings.nikoPlushMinLight + std::max(lum, samLum) / g_settings.nikoPlushFullLight,
                                   g_settings.nikoPlushMinLight, 1.25f);
            if (!found) it.bright = it.target;
        }
        it.bright += (it.target - it.bright) * ease;
        next.push_back(it);
    }
    if (relight) g_lightTick = now;
    g_items.swap(next);
}

void Plush::Meow(const float pos[3], int kind)
{
    if (!g_loaded || g_meow.empty() || g_settings.nikoPlushVolume <= 0) return;
    // Volume and left/right from where the camera is.
    float vol = kind == 0 ? 1.0f : 0.55f, pan = 0.0f;
    float cam[3], fwd[3];
    if (Editor::RenderCamera(cam, fwd))
    {
        const float dx = pos[0] - cam[0], dy = pos[1] - cam[1], dz = pos[2] - cam[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        vol *= std::pow(std::clamp(1.0f - (dist - 150.0f) / 2850.0f, 0.0f, 1.0f), 1.5f);
        const float rl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
        if (rl > 1e-3f && dist > 1.0f) pan = std::clamp((dx * -fwd[1] / rl + dy * fwd[0] / rl) / dist, -1.0f, 1.0f) * 0.8f;
    }
    vol *= std::min(g_settings.nikoPlushVolume, 200) / 100.0f;
    if (vol < 0.01f) return;
    const float pitch = kind == 0 ? Rand(0.94f, 1.08f) : Rand(1.08f, 1.22f);
    const float len = kind == 0 ? 1.0f : 0.7f;   // the bounce "mrrp" is shorter
    const int srcN = int(g_meow.size() * len);
    const int n = std::max(1, int(srcN / pitch));
    const float gl = vol * std::min(1.0f, 1.0f - pan), gr = vol * std::min(1.0f, 1.0f + pan);
    std::vector<uint8_t>& wav = g_wav[g_wavNext];
    g_wavNext ^= 1;
    wav.assign(44 + size_t(n) * 4, 0);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&wav[at], &v, 2); };
    memcpy(&wav[0], "RIFF", 4); put32(4, uint32_t(36 + n * 4)); memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, 2); put32(24, uint32_t(g_rate)); put32(28, uint32_t(g_rate * 4)); put16(32, 4); put16(34, 16);
    memcpy(&wav[36], "data", 4); put32(40, uint32_t(n * 4));
    for (int i = 0; i < n; ++i)
    {
        const float s = i * pitch;
        const int a = std::min(int(s), srcN - 1), b = std::min(a + 1, srcN - 1);
        float v = (g_meow[size_t(a)] + (g_meow[size_t(b)] - g_meow[size_t(a)]) * (s - a)) / 32768.0f;
        if (i > n - 400) v *= float(n - i) / 400.0f;   // no click at a shortened end
        put16(44 + size_t(i) * 4, uint16_t(int16_t(std::clamp(v * gl, -1.0f, 1.0f) * 32767.0f)));
        put16(46 + size_t(i) * 4, uint16_t(int16_t(std::clamp(v * gr, -1.0f, 1.0f) * 32767.0f)));
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void Plush::OnSetVSConstants(IDirect3DDevice9* dev, UINT start, const float* data, UINT count)
{
    (void)start;
    if (g_inDraw || !data || count < 4) return;
    if (g_items.empty() && (g_warmed || !Plush::Active())) return;   // nothing to draw (or warm up)
    if (!g_camFetched) FetchCamera();
    if (!g_haveCam) return;
    for (UINT i = 0; i + 4 <= count; ++i)
        if (TryMatrix(data + i * 4)) break;
    if (g_drawn || !g_rtBack || !g_vpThisFrame || !g_resetSeen) return;   // the colour passes (see the top of the file)
    if (g_vision != 5 || g_frameHeat || g_frameEmf) return;   // the vision modes: before their copy (see the top of the file)
    // The colour pass with a depth test (not the depth pass, which has colour writes off). The plush is drawn before
    // this call reaches the device: the state block restores the old constants and the game's call sets the new ones.
    DWORD cw = 0, ze = 0, zf = D3DCMP_LESSEQUAL, blend = TRUE;
    if (FAILED(dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw)) || !(cw & 7)) return;
    // Not at a blended draw: the additive see-through effects drawn right after the reset feed a second glow, and the
    // plush drawn there glowed. The colour passes start with opaque draws.
    if (FAILED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) || blend) return;
    if (FAILED(dev->GetRenderState(D3DRS_ZENABLE, &ze))) return;
    if (ze == D3DZB_FALSE && g_settings.nikoPlushDebug < 2) return;
    dev->GetRenderState(D3DRS_ZFUNC, &zf);
    g_haveZ = true;
    g_zEnable = ze == D3DZB_FALSE ? DWORD(D3DZB_TRUE) : ze;
    // The colour pass may test EQUAL against the depth pass, which has no plush in it: less-or-equal (or the reverse).
    g_zFunc = (zf == D3DCMP_GREATER || zf == D3DCMP_GREATEREQUAL) ? DWORD(D3DCMP_GREATEREQUAL) : DWORD(D3DCMP_LESSEQUAL);
    if (!g_haveVP)
    {
        if (!g_haveProj) return;
        float frame[16], view[16];
        CameraFrame(frame, view);
        Mul(view, g_proj, g_vp);
        g_haveVP = true;
        ++g_rebuilt;
    }
    Draw(dev, true);
}

void Plush::OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code)
{
    // Thermal's scene shaders end: dp4 r6.x, <position>, c23; mul oT2.z, r6.x, c24.y; mov oT2.xyw, c0.x.
    if (!vs || !code || !g_loaded) return;
    static const DWORD tail[] = { 0xA0E40017, 0x00000005, 0xE0040002, 0x80000006, 0xA0550018, 0x00000001, 0xE00B0002, 0xA0000000, 0x0000FFFF };
    size_t n = 1;
    while (n < 4096 && code[n - 1] != 0x0000FFFF) ++n;
    if (n < std::size(tail) || memcmp(code + n - std::size(tail), tail, sizeof(tail)) != 0) return;
    g_heatShaders[g_heatShaderCount % 64] = vs;
    ++g_heatShaderCount;
}

void Plush::OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code)
{
    if (!ps || !code || !g_loaded) return;
    if (memcmp(code, kHeatFilter, sizeof(kHeatFilter)) == 0) { g_filterShaders[g_filterShaderCount % 16] = ps; ++g_filterShaderCount; }
    if (memcmp(code, kEmfScene, sizeof(kEmfScene)) == 0) { g_emfShaders[g_emfShaderCount % 16] = ps; ++g_emfShaderCount; }
}

void Plush::OnSetPixelShader(IDirect3DPixelShader9* ps)
{
    if (g_inDraw) return;
    g_curIsFilter = g_curIsEmf = false;
    for (int i = 0; ps && i < std::min(g_filterShaderCount, 16) && !g_curIsFilter; ++i) g_curIsFilter = g_filterShaders[i] == ps;
    for (int i = 0; ps && i < std::min(g_emfShaderCount, 16) && !g_curIsEmf; ++i) g_curIsEmf = g_emfShaders[i] == ps;
}

namespace
{
    // Thermal's palette: its filter's stage 2 texture, noted in the log (and read again if the texture or its contents
    // change, checked twice a second).
    void ReadPalette(IDirect3DDevice9* dev)
    {
        g_palTried = true;
        IDirect3DBaseTexture9* base = nullptr;
        if (FAILED(dev->GetTexture(2, &base)) || !base) { if (g_palLogs++ < 8) LOG("Plush: thermal's palette: no texture at stage 2"); return; }
        if (base == g_palTex && ++g_palAge < 60) { base->Release(); return; }
        g_palTex = base;
        g_palAge = 0;
        if (base->GetType() == D3DRTYPE_TEXTURE)
        {
            IDirect3DTexture9* t = static_cast<IDirect3DTexture9*>(base);
            D3DSURFACE_DESC d;
            D3DLOCKED_RECT r;
            if (SUCCEEDED(t->GetLevelDesc(0, &d)) && (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) && d.Width <= 1024 && d.Height <= 1024)
            {
                IDirect3DSurface9* level = nullptr;
                IDirect3DSurface9* sys = nullptr;
                bool locked = SUCCEEDED(t->LockRect(0, &r, nullptr, D3DLOCK_READONLY));
                if (!locked && SUCCEEDED(t->GetSurfaceLevel(0, &level)) &&
                    SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
                    SUCCEEDED(dev->GetRenderTargetData(level, sys)))
                    locked = SUCCEEDED(sys->LockRect(&r, nullptr, D3DLOCK_READONLY));
                uint32_t hash = 2166136261u;
                if (locked)
                    for (UINT y = 0; y < d.Height; ++y)
                        for (UINT x = 0; x < d.Width * 4; ++x) { hash ^= static_cast<const uint8_t*>(r.pBits)[size_t(y) * r.Pitch + x]; hash *= 16777619u; }
                if (locked && hash == g_palHash && !g_pal.empty())
                {
                    if (sys) sys->UnlockRect(); else t->UnlockRect(0);
                    locked = false;
                }
                else if (locked)
                {
                    g_palHash = hash;
                    g_knotsLogged = false;
                    g_palW = int(d.Width);
                    g_palH = int(d.Height);
                    g_pal.resize(size_t(g_palW) * g_palH);
                    g_palRGB.resize(size_t(g_palW) * g_palH * 3);
                    for (int y = 0; y < g_palH; ++y)
                        for (int x = 0; x < g_palW; ++x)
                        {
                            const uint8_t* px = static_cast<const uint8_t*>(r.pBits) + size_t(y) * r.Pitch + size_t(x) * 4;
                            g_pal[size_t(y) * g_palW + x] = (0.3f * px[2] + 0.59f * px[1] + 0.11f * px[0]) / 255.0f;
                            float* rgb = &g_palRGB[(size_t(y) * g_palW + x) * 3];
                            rgb[0] = px[2] / 255.0f; rgb[1] = px[1] / 255.0f; rgb[2] = px[0] / 255.0f;
                        }
                    if (sys) sys->UnlockRect(); else t->UnlockRect(0);
                    if (g_palLogs++ < 8)
                    {
                        LOG("Plush: thermal's palette %dx%d read (colours as RRGGBB, by green across, blue down):", g_palW, g_palH);
                        for (int row = 0; row < 9; ++row)
                        {
                            const int y = std::min(g_palH - 1, row * g_palH / 8);
                            char line[200] = "";
                            for (int col = 0; col < 9; ++col)
                            {
                                const int x = std::min(g_palW - 1, col * g_palW / 8);
                                const float* rgb = &g_palRGB[(size_t(y) * g_palW + x) * 3];
                                char v[12];
                                sprintf_s(v, " %02X%02X%02X", int(rgb[0] * 255 + 0.5f), int(rgb[1] * 255 + 0.5f), int(rgb[2] * 255 + 0.5f));
                                strcat_s(line, v);
                            }
                            LOG("Plush:   blue %.2f:%s", float(y) / g_palH, line);
                        }
                    }
                }
                else if (g_palHash == 0 && g_palLogs++ < 8) LOG("Plush: thermal's palette couldn't be read (%ux%u, format %d, pool %d)", d.Width, d.Height, int(d.Format), int(d.Pool));
                if (sys) sys->Release();
                if (level) level->Release();
            }
            else if (g_palLogs++ < 8) LOG("Plush: thermal's palette isn't a plain colour texture");
        }
        base->Release();
    }
}

void Plush::OnSetVertexShader(IDirect3DVertexShader9* vs)
{
    if (g_inDraw) return;
    g_curVS = vs;
    g_curIsHeat = false;
    for (int i = 0; vs && i < std::min(g_heatShaderCount, 64) && !g_curIsHeat; ++i) g_curIsHeat = g_heatShaders[i] == vs;
}

void Plush::OnDraw(IDirect3DDevice9* dev)
{
    if (!g_inDraw && g_curIsHeat) g_frameHeat = true;
    if (!g_inDraw && g_curIsEmf) g_frameEmf = true;
    if (!g_inDraw && g_curIsFilter && g_frameHeat && g_loaded) ReadPalette(dev);
    // Thermal's constants, once a frame, from a draw whose view depth plane is the camera's (world space).
    if (!g_inDraw && g_loaded && g_curIsHeat && !g_heatFrame && !g_items.empty() && g_haveCam)
    {
        float c0[4], c22[12];
        if (SUCCEEDED(dev->GetVertexShaderConstantF(0, c0, 1)) && SUCCEEDED(dev->GetVertexShaderConstantF(22, c22, 3)))
        {
            const float* plane = c22 + 4;
            const float len = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
            const bool facing = len > 1e-6f && (plane[0] * g_camFwd[0] + plane[1] * g_camFwd[1] + plane[2] * g_camFwd[2]) / len > 0.95f;
            const float atCam = (plane[0] * g_camPos[0] + plane[1] * g_camPos[1] + plane[2] * g_camPos[2] + plane[3]) / std::max(len, 1e-6f);
            if (facing && std::fabs(atCam) < 64.0f && std::isfinite(c22[9]) && c22[9] > 0)
            {
                memcpy(g_heatC0, c0, sizeof(g_heatC0));
                memcpy(g_heatC22, c22, sizeof(g_heatC22));
                memcpy(g_heatC23, c22 + 4, sizeof(g_heatC23));
                memcpy(g_heatC24, c22 + 8, sizeof(g_heatC24));
                g_heatHave = g_heatFrame = true;
                if (!g_heatLogged)
                {
                    g_heatLogged = true;
                    LOG("Plush: thermal's scene constants: c0 (%.3g %.3g %.3g %.3g), c22 (%.3g %.3g %.3g %.3g), c23 (%.3g %.3g %.3g %.3g), "
                        "c24.y %.4g (range %.0f)", c0[0], c0[1], c0[2], c0[3], g_heatC22[0], g_heatC22[1], g_heatC22[2], g_heatC22[3], g_heatC23[0],
                        g_heatC23[1], g_heatC23[2], g_heatC23[3], g_heatC24[1], 1.0f / g_heatC24[1]);
                }
            }
        }
    }
    // The full-screen passes between the first pass and the colour passes have the depth test off. And the first
    // depth-tested draw into the back buffer starts the scene (a copy of the back buffer before it isn't the scene's).
    if (g_inDraw || !g_rtBack || !g_loaded) return;
    if (g_items.empty() && g_warmed) return;
    const bool wantReset = !g_resetSeen && !g_drawn && g_vpThisFrame;
    if (g_sceneSeen && g_zOffAfterScene && !wantReset) return;
    DWORD ze = D3DZB_TRUE;
    if (FAILED(dev->GetRenderState(D3DRS_ZENABLE, &ze))) return;
    if (ze != D3DZB_FALSE) { g_sceneSeen = true; return; }
    if (g_sceneSeen) g_zOffAfterScene = true;
    if (wantReset) g_resetSeen = true;
}

namespace
{
    // With goggles on: into the back buffer, depth-tested against the scene, before the goggles' filter reads it (see
    // the top of the file).
    void DrawGoggleFrame(IDirect3DDevice9* dev, IDirect3DSurface9* back)
    {
        g_mode = FrameMode();
        if (g_mode != g_vision && g_mode != g_modeLogged && g_modeLogs < 6)
        {
            ++g_modeLogs;
            g_modeLogged = g_mode;
            LOG("Plush: the game's goggle mode says %d, this frame is drawn as %d (going by the frame)", g_vision, g_mode);
        }
        if (!g_camFetched) FetchCamera();
        if (!g_haveVP && g_haveProj && g_haveCam)
        {
            float frame[16], view[16];
            CameraFrame(frame, view);
            Mul(view, g_proj, g_vp);
            g_haveVP = true;
            ++g_rebuilt;
        }
        if (!g_haveVP) { g_mode = 5; return; }
        IDirect3DSurface9* oldRT = nullptr;
        dev->GetRenderTarget(0, &oldRT);
        D3DVIEWPORT9 oldVP = {};
        const bool haveVP = SUCCEEDED(dev->GetViewport(&oldVP));
        dev->SetRenderTarget(0, back);   // (and the viewport to all of it)
        const bool began = SUCCEEDED(dev->BeginScene());   // (fails when the game is inside one already)
        g_beforeCopy = true;
        Draw(dev, false);
        g_beforeCopy = false;
        if (began) dev->EndScene();
        if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
        if (haveVP) dev->SetViewport(&oldVP);
        g_drawnCopy = true;
        g_drawn = true;
        g_mode = 5;
    }
}

namespace
{
    // F10 frame captures: the back buffer's pixel at each plush on screen and at Sam (his middle and head), into the log.
    void Probe(IDirect3DDevice9* dev, IDirect3DSurface9* surf, const char* label)
    {
        if (!Capture::Active() || g_probes >= 10 || !surf || !g_haveVP) return;
        ++g_probes;
        D3DSURFACE_DESC d;
        if (FAILED(surf->GetDesc(&d)) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8)) return;
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys) return;
        D3DLOCKED_RECT r;
        if (SUCCEEDED(dev->GetRenderTargetData(surf, sys)) && SUCCEEDED(sys->LockRect(&r, nullptr, D3DLOCK_READONLY)))
        {
            std::string line;
            auto at = [&](const char* name, const float p[3])
            {
                float c[4];
                for (int j = 0; j < 4; ++j) c[j] = p[0] * g_vp[j] + p[1] * g_vp[4 + j] + p[2] * g_vp[8 + j] + g_vp[12 + j];
                if (!(c[3] > 1)) return;
                const int x = int((c[0] / c[3] * 0.5f + 0.5f) * d.Width), y = int((0.5f - c[1] / c[3] * 0.5f) * d.Height);
                if (x < 0 || y < 0 || x >= int(d.Width) || y >= int(d.Height)) return;
                const uint8_t* px = static_cast<const uint8_t*>(r.pBits) + size_t(y) * r.Pitch + size_t(x) * 4;
                char buf[96];
                sprintf_s(buf, "  %s (%d,%d) RGBA %d %d %d %d;", name, x, y, px[2], px[1], px[0], px[3]);
                line += buf;
            };
            for (size_t i = 0; i < g_items.size() && i < 3; ++i)
            {
                Pose q = ReadPose(g_items[i].actor);
                if (g_items[i].hand) memcpy(q.loc, g_items[i].loc, sizeof(q.loc));
                char name[24];
                sprintf_s(name, "plush %d", int(i));
                if (q.ok || g_items[i].hand)
                {
                    at(name, q.loc);
                    const float yaw = std::atan2(g_camFwd[1], g_camFwd[0]);
                    const float beside[3] = { q.loc[0] - std::sin(yaw) * 45, q.loc[1] + std::cos(yaw) * 45, q.loc[2] };
                    sprintf_s(name, "beside plush %d", int(i));
                    at(name, beside);
                }
            }
            const Pose sam = ReadPose(g_sam);
            if (sam.ok)
            {
                at("Sam", sam.loc);
                const float head[3] = { sam.loc[0], sam.loc[1], sam.loc[2] + 40 };
                at("Sam's head", head);
            }
            sys->UnlockRect();
            LOG("Plush: probe (vision %d, frame %d) %s:%s", g_vision, FrameMode(), label, line.c_str());
        }
        sys->Release();
    }
}

void Plush::OnStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src)
{
    // The copy the goggles' filter reads: night vision's is the first after the full-screen passes (its first copy,
    // before them, only feeds its glow); electromagnetic's (and thermal's, if its depth clear didn't come) the first.
    if (!g_loaded || g_inDraw || !src) return;
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    if (back == src)
    {
        const int mode = FrameMode();
        const bool draw = mode != 5 && g_sceneSeen && !g_items.empty();
        if (draw && !g_drawnCopy && (mode != 11 || g_zOffAfterScene))
            DrawGoggleFrame(dev, back);
        char label[32];
        sprintf_s(label, "copy %d", ++g_copyIndex);
        Probe(dev, back, label);
    }
    back->Release();
}

void Plush::OnBeforeClear(IDirect3DDevice9* dev, DWORD flags)
{
    // Thermal clears the depth buffer between its scene and the copy: the plush goes in before (depth-tested).
    if (!g_loaded || g_inDraw || FrameMode() != 10 || g_drawnCopy || !g_sceneSeen || !g_rtBack || g_items.empty()) return;
    if (!(flags & D3DCLEAR_ZBUFFER) || (flags & D3DCLEAR_TARGET)) return;
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    DrawGoggleFrame(dev, back);
    back->Release();
}

void Plush::OnClear(DWORD flags)
{
    // A depth clear of the scene after the plush was drawn into it: draw it again at the next camera matrix. (Not
    // after it went into a goggle frame's copy: that one is in the goggles' image already.)
    if ((flags & D3DCLEAR_ZBUFFER) && g_rtFull && g_drawn && !g_drawnCopy) g_drawn = false;
}

void Plush::OnSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface)
{
    if (index != 0 || !g_loaded) return;
    D3DSURFACE_DESC d = {};
    const bool known = surface && SUCCEEDED(surface->GetDesc(&d));
    g_rtFull = !known || !g_bbW || (d.Width + 8 >= g_bbW && d.Height + 8 >= g_bbH);
    g_rtHdr = known && g_rtFull && d.Format == D3DFMT_A16B16G16R16F;
    IDirect3DSurface9* bb = nullptr;
    g_rtBack = surface && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb == surface;
    if (bb) bb->Release();
}

void Plush::OnPresent(IDirect3DDevice9* dev)
{
    if (!g_loaded) return;
    if (Capture::Active())
    {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { Probe(dev, bb, "at Present"); bb->Release(); }
    }
    else g_probes = 0;
    g_copyIndex = 0;
    if (!g_bbW)
    {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
        {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(bb->GetDesc(&d))) { g_bbW = d.Width; g_bbH = d.Height; }
            bb->Release();
        }
    }
    // The sound system's first use takes a moment: once, with a moment of silence, before any meow.
    if (!g_soundWarm && Plush::Active())
    {
        g_soundWarm = true;
        static std::vector<uint8_t> silence;
        const uint32_t n = 2205;   // 0.1 s, 22050 Hz mono
        silence.assign(44 + n * 2, 0);
        auto put32 = [&](size_t at, uint32_t v) { memcpy(&silence[at], &v, 4); };
        auto put16 = [&](size_t at, uint16_t v) { memcpy(&silence[at], &v, 2); };
        memcpy(&silence[0], "RIFF", 4); put32(4, 36 + n * 2); memcpy(&silence[8], "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, 22050); put32(28, 44100); put16(32, 2); put16(34, 16);
        memcpy(&silence[36], "data", 4); put32(40, n * 2);
        PlaySoundW(reinterpret_cast<LPCWSTR>(silence.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
    // Not drawn in the scene this frame: into the back buffer, depth-tested against the scene. Only if this frame drew
    // the world (its camera matrix was set): menus don't, and the plush showed over them.
    if (!g_vpThisFrame) g_drawn = true;
    if (!g_drawn && !g_items.empty() && !g_haveVP && g_haveProj && g_haveCam)
    {
        float frame[16], view[16];
        CameraFrame(frame, view);
        Mul(view, g_proj, g_vp);
        g_haveVP = true;
        ++g_rebuilt;
    }
    if (!g_drawn && !g_items.empty())
    {
        IDirect3DSurface9* back = nullptr;
        IDirect3DSurface9* oldRT = nullptr;
        dev->GetRenderTarget(0, &oldRT);
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) dev->SetRenderTarget(0, back);
        if (SUCCEEDED(dev->BeginScene()))
        {
            Draw(dev, false);
            dev->EndScene();
        }
        if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
        if (back) back->Release();
    }
    ++g_frames;
    if (!g_items.empty() && (g_frames % 1800) == 0)
        LOG("Plush: %d to draw; last 1800 frames: camera matrix %u times (%u loose, %u lookalikes rejected), %u rebuilt from the camera, drawn "
            "%u in the scene + %u before a goggles' copy + %u at Present, %u frames without a camera matrix", int(g_items.size()), g_vpHits,
            g_relaxed, g_rejected, g_rebuilt, g_drawsHdr, g_drawsCopy, g_drawsPresent, g_noVP);
    if ((g_frames % 1800) == 0) g_vpHits = g_drawsHdr = g_drawsCopy = g_drawsPresent = g_noVP = g_relaxed = g_rebuilt = g_rejected = 0;
    g_drawn = false;
    g_haveVP = false;
    g_vpThisFrame = false;
    g_resetSeen = false;
    g_sceneSeen = false;
    g_zOffAfterScene = false;
    g_heatFrame = false;
    g_frameHeat = g_frameEmf = false;
    g_drawnCopy = false;
    g_camFetched = false;
}

void Plush::OnNewDevice()
{
    Release();
    g_heatShaderCount = 0;   // the game makes its shaders again
    g_filterShaderCount = 0;
    g_curIsFilter = false;
    g_emfShaderCount = 0;
    g_curIsEmf = false;
    g_palTried = false;
    g_palTex = nullptr;
    g_palHash = 0;
    g_pal.clear();
    g_palRGB.clear();
    g_curVS = nullptr;
    g_curIsHeat = false;
    g_items.clear();
    g_bbW = g_bbH = 0;
    g_rtFull = true;
    g_rtHdr = false;
    g_rtBack = false;
}

void Plush::OnReset(bool before)
{
    if (before) Release();
    g_bbW = g_bbH = 0;
    g_rtFull = true;
    g_rtHdr = false;
}

void Plush::Init()
{
    g_loaded = Load();
    if (!g_loaded)
    {
        LOG("Plush: no Niko plush (System\\SCCTRemaster_Award.pak missing or unreadable)");
        return;
    }
    LOG("Plush: Niko plush loaded (%d vertices, %d triangles, meow %.2f s); %s", int(g_verts.size()), int(g_idx.size() / 3),
        float(g_meow.size()) / float(g_rate),
        g_settings.nikoPlush <= 0 ? "switched off ([Gameplay] NikoPlush = 0)"
        : g_settings.nikoPlush >= 2 ? "given in every mission ([Gameplay] NikoPlush = 2)"
        : Clusterfuck::Victories() > 0 ? "unlocked (CLUSTERFUCK beaten)" : "locked until CLUSTERFUCK is beaten");
}
