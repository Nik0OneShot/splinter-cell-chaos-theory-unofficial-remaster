// D3D9 frame capture (diagnostics): press the capture key (F10 by default) in game and the next
// two frames are written to SCCTRemaster_capture_N.txt - every draw call with its shaders,
// textures, render target and the relevant render/texture-stage states, plus the bytecode of
// every shader used. Used to find and replay the thermal-vision pass on AMD hardware.
#include "pch.h"
#include "features.h"
#include "d3d9_capture.h"

extern HMODULE g_self;

namespace
{
    constexpr int kFramesToCapture = 2;

    struct ShaderInfo { uint32_t hash; DWORD version; std::vector<DWORD> code; };
    std::unordered_map<void*, ShaderInfo> g_shaders;          // PS and VS by object pointer
    std::unordered_set<void*> g_usedShaders;

    FILE* g_out = nullptr;
    int g_framesLeft = 0;
    unsigned g_draw = 0;
    bool g_keyWasDown = false;

    DWORD g_rs[256] = {};                    // render states (subset indices < 256)
    DWORD g_tss[4][33] = {};                 // texture stage states, stages 0-3
    IDirect3DBaseTexture9* g_tex[8] = {};
    IDirect3DSurface9* g_rt = nullptr;
    void* g_ps = nullptr;
    void* g_vs = nullptr;
    DWORD g_fvf = 0;
    float g_psConst[8][4] = {};

    uint32_t Hash(const DWORD* code, size_t n)
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < n; ++i) { h ^= code[i]; h *= 16777619u; }
        return h;
    }

    std::string TexDesc(IDirect3DBaseTexture9* t)
    {
        if (!t) return "-";
        char buf[96];
        const D3DRESOURCETYPE type = t->GetType();
        D3DSURFACE_DESC d{};
        if (type == D3DRTYPE_TEXTURE)
            static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
        else if (type == D3DRTYPE_CUBETEXTURE)
            static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &d);
        else
            return "volume";
        snprintf(buf, sizeof(buf), "%s%ux%u:%d%s", type == D3DRTYPE_CUBETEXTURE ? "cube:" : "", d.Width, d.Height, int(d.Format),
                 (d.Usage & D3DUSAGE_RENDERTARGET) ? ":RT" : ((d.Usage & D3DUSAGE_DEPTHSTENCIL) ? ":DS" : ""));
        return buf;
    }

    std::string ShaderDesc(void* s)
    {
        if (!s) return "fixed";
        auto it = g_shaders.find(s);
        if (it == g_shaders.end()) return "unknown";
        g_usedShaders.insert(s);
        char buf[32];
        snprintf(buf, sizeof(buf), "%08X(v%X)", it->second.hash, unsigned(it->second.version & 0xFFFF));
        return buf;
    }

    void LogDraw(const char* kind, unsigned prim, unsigned count)
    {
        if (!g_out) return;
        D3DSURFACE_DESC rd{};
        if (g_rt) g_rt->GetDesc(&rd);
        fprintf(g_out, "#%u %s prim=%u n=%u | RT %ux%u:%d | PS %s VS %s FVF %X | ", g_draw++, kind, prim, count,
                rd.Width, rd.Height, int(rd.Format), ShaderDesc(g_ps).c_str(), ShaderDesc(g_vs).c_str(), unsigned(g_fvf));
        for (int i = 0; i < 4; ++i)
            fprintf(g_out, "T%d %s ", i, TexDesc(g_tex[i]).c_str());
        fprintf(g_out, "| blend %u %u>%u op%u atest %u ref%u fn%u z %u/%u fog %u wmask %X srgb %u",
                g_rs[D3DRS_ALPHABLENDENABLE], g_rs[D3DRS_SRCBLEND], g_rs[D3DRS_DESTBLEND], g_rs[D3DRS_BLENDOP],
                g_rs[D3DRS_ALPHATESTENABLE], g_rs[D3DRS_ALPHAREF], g_rs[D3DRS_ALPHAFUNC], g_rs[D3DRS_ZENABLE],
                g_rs[D3DRS_ZWRITEENABLE], g_rs[D3DRS_FOGENABLE], g_rs[D3DRS_COLORWRITEENABLE], g_rs[D3DRS_SRGBWRITEENABLE]);
        if (!g_ps)
        {
            for (int s = 0; s < 4 && g_tss[s][D3DTSS_COLOROP] != D3DTOP_DISABLE; ++s)
                fprintf(g_out, " | TSS%d c%u(%X,%X) a%u(%X,%X) tci%X ttf%X", s, g_tss[s][D3DTSS_COLOROP], g_tss[s][D3DTSS_COLORARG1],
                        g_tss[s][D3DTSS_COLORARG2], g_tss[s][D3DTSS_ALPHAOP], g_tss[s][D3DTSS_ALPHAARG1], g_tss[s][D3DTSS_ALPHAARG2],
                        g_tss[s][D3DTSS_TEXCOORDINDEX], g_tss[s][D3DTSS_TEXTURETRANSFORMFLAGS]);
            fprintf(g_out, " tfactor %08X", g_rs[D3DRS_TEXTUREFACTOR]);
        }
        else
        {
            for (int s = 0; s < 4; ++s)
                if (g_tss[s][D3DTSS_TEXTURETRANSFORMFLAGS]) fprintf(g_out, " ttf%d=%X", s, g_tss[s][D3DTSS_TEXTURETRANSFORMFLAGS]);
            fprintf(g_out, " c0=(%.3g %.3g %.3g %.3g) c1=(%.3g %.3g %.3g %.3g)", g_psConst[0][0], g_psConst[0][1], g_psConst[0][2],
                    g_psConst[0][3], g_psConst[1][0], g_psConst[1][1], g_psConst[1][2], g_psConst[1][3]);
        }
        fputc('\n', g_out);
    }

    // [Debug] LogShaderInputs: the first draw of every (pixel shader, vertex shader) pair on the Shader Model 1.1
    // path logs one [SHADER] line: both hashes, the four textures (type, size, format, mip levels), blend mode and
    // pixel shader constants c0-c7. Collected during normal play, for the shader pack.
    std::unordered_set<uint64_t> g_seenPairs;
    std::string TexDescLevels(IDirect3DBaseTexture9* t)
    {
        if (!t) return "-";
        return TexDesc(t) + "/" + std::to_string(t->GetLevelCount());
    }
    void ProfileDraw()
    {
        if (!g_ps || g_seenPairs.size() >= 800) return;
        auto it = g_shaders.find(g_ps);
        if (it == g_shaders.end() || (it->second.version & 0xFFFF) != 0x0101) return;
        uint32_t vh = 0;
        auto vt = g_shaders.find(g_vs);
        if (vt != g_shaders.end()) vh = vt->second.hash;
        if (!g_seenPairs.insert((uint64_t(it->second.hash) << 32) | vh).second) return;
        char line[1400];
        int o = snprintf(line, sizeof(line), "[SHADER] PS %08X VS %08X |", it->second.hash, vh);
        for (int i = 0; i < 4 && o > 0 && o < 1200; ++i)
            o += snprintf(line + o, sizeof(line) - o, " T%d %s", i, TexDescLevels(g_tex[i]).c_str());
        o += snprintf(line + o, sizeof(line) - o, " | blend %u %u>%u atest %u |", g_rs[D3DRS_ALPHABLENDENABLE], g_rs[D3DRS_SRCBLEND],
                      g_rs[D3DRS_DESTBLEND], g_rs[D3DRS_ALPHATESTENABLE]);
        for (int c = 0; c < 8 && o > 0 && o < 1300; ++c)
            o += snprintf(line + o, sizeof(line) - o, " c%d(%.3g %.3g %.3g %.3g)", c, g_psConst[c][0], g_psConst[c][1], g_psConst[c][2],
                          g_psConst[c][3]);
        LOG("%s", line);
    }

    void BeginCapture()
    {
        static int n = 0;
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(g_self, path, MAX_PATH);
        std::wstring p(path);
        p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"SCCTRemaster_capture_" + std::to_wstring(++n) + L".txt";
        g_out = _wfopen(p.c_str(), L"w");
        g_framesLeft = kFramesToCapture;
        g_draw = 0;
        g_usedShaders.clear();
        LOG("Capture: writing %ls", p.c_str());
        if (g_out)
            fprintf(g_out, "SCCT Remaster D3D9 capture. Formats are D3DFORMAT numbers (21=A8R8G8B8, 22=X8R8G8B8, 113=A16B16G16R16F, 75=D24S8, 80=D16).\n");
    }

    void EndCapture()
    {
        if (!g_out) return;
        fprintf(g_out, "\n==== shaders used ====\n");
        for (void* s : g_usedShaders)
        {
            auto it = g_shaders.find(s);
            if (it == g_shaders.end()) continue;
            fprintf(g_out, "shader %08X:", it->second.hash);
            for (DWORD d : it->second.code) fprintf(g_out, " %08X", d);
            fputc('\n', g_out);
        }
        fclose(g_out);
        g_out = nullptr;
        LOG("Capture: done");
    }
}

void Capture::OnPresent()
{
    if (g_out)
    {
        fprintf(g_out, "---- end of frame ----\n");
        if (--g_framesLeft <= 0)
            EndCapture();
    }
    // [Debug] AutoCaptureAt: one capture that many seconds after the first frame (no key needed)
    static DWORD s_first = 0;
    static bool s_autoDone = false;
    if (!s_first) s_first = GetTickCount();
    if (g_settings.autoCaptureAt > 0.0f && !s_autoDone && !g_out &&
        GetTickCount() - s_first >= static_cast<DWORD>(g_settings.autoCaptureAt * 1000.0f))
    {
        s_autoDone = true;
        BeginCapture();
    }
    const int key = g_settings.captureKey;
    if (key > 0)
    {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        if (down && !g_keyWasDown && !g_out && Game::hWnd && GetForegroundWindow() == Game::hWnd)
            BeginCapture();
        g_keyWasDown = down;
    }
}

void Capture::OnShaderCreated(void* shader, const DWORD* code)
{
    if (!shader || !code) return;
    size_t n = 1;
    while (n < 8192 && code[n - 1] != 0x0000FFFF) ++n;
    g_shaders[shader] = ShaderInfo{ Hash(code, n), code[0], std::vector<DWORD>(code, code + n) };
}

void Capture::OnSetPixelShader(void* s) { g_ps = s; }
void Capture::OnSetVertexShader(void* s) { g_vs = s; }
void Capture::OnSetFVF(DWORD fvf) { g_fvf = fvf; }
void Capture::OnSetTexture(DWORD stage, IDirect3DBaseTexture9* t) { if (stage < 8) g_tex[stage] = t; }
void Capture::OnSetRenderState(D3DRENDERSTATETYPE s, DWORD v) { if (unsigned(s) < 256) g_rs[s] = v; }
void Capture::OnSetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) { if (stage < 4 && unsigned(t) < 33) g_tss[stage][t] = v; }
void Capture::OnSetPixelShaderConstantF(UINT start, const float* d, UINT count)
{
    for (UINT i = 0; i < count && d; ++i)
        if (start + i < 8) std::memcpy(g_psConst[start + i], d + i * 4, 16);
}
void Capture::OnSetRenderTarget(DWORD index, IDirect3DSurface9* rt)
{
    if (index == 0) g_rt = rt;
    if (g_out && index == 0)
    {
        D3DSURFACE_DESC d{};
        if (rt) rt->GetDesc(&d);
        fprintf(g_out, "SetRenderTarget0 %ux%u:%d\n", d.Width, d.Height, int(d.Format));
    }
}
void Capture::OnClear(DWORD flags, D3DCOLOR color)
{
    if (g_out) fprintf(g_out, "Clear flags=%X color=%08X\n", unsigned(flags), unsigned(color));
}
void Capture::OnDraw(const char* kind, unsigned prim, unsigned count)
{
    if (g_out) LogDraw(kind, prim, count);
    if (g_settings.logShaderInputs) ProfileDraw();
}
bool Capture::Active() { return g_out != nullptr; }
