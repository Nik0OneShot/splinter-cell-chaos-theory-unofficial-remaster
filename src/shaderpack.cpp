// Shader pack: our own replacements for the game's Shader Model 1.1 shaders (see shaders/pack.hlsl and
// tools/build_shaders.py, which generates shaderpack_data.h). The game's ps_1_1 shaders are matched by an FNV-1a
// hash of their bytecode plus length (the same hash the [SHADER] log and F10 captures use). The game keeps its
// original shader objects; next to a replaced one we create a ps_2_a build (with specular anti-aliasing) or, if
// the driver refuses that, a ps_2_0 build, and SetPixelShader binds ours instead while the pack is on. ps_2_x
// pairs with the game's vs_1_1 vertex shaders (ps_3_0 would not). Shader Model 1.1 path only.
//
// Keys: ShaderPackToggleKey (F6) switches between the originals and the pack live. ShaderHighlightKey (F11)
// cycles a flat magenta through the shader families (shaderpack_families.h, every ps_1_1 shader in the exe), and
// with Shift through the single shaders of the current family, to see which shader draws a surface; each step is
// logged.
#include "pch.h"
#include "features.h"
#include "shaderpack_data.h"
#include "shaderpack_families.h"

namespace
{
    struct Program
    {
        const char* name;
        uint32_t hash;          // FNV-1a of the original's DWORD tokens
        size_t length;          // original's token count (incl. the end token)
        const uint32_t* builds[2];
    };
    // The game ships each of these in several byte-identical copies (exe 111274B8/11126000/111271D8, 11127598/
    // 111260E0, 111272B8); one entry covers all copies.
    const Program kPrograms[] = {
        { "specular A (Phong, light pattern)", 0x930D9091u, 55, { kPackA_2a, kPackA_20 } },
        { "specular B (Phong, blended normal maps)", 0x4B5F4410u, 61, { kPackB_2a, kPackB_20 } },
        { "specular C (Blinn, wet surfaces)", 0x33ECDAA0u, 51, { kPackC_2a, kPackC_20 } },
        { "bump base lighting D (exact light vector)", 0xAB78AFECu, 37, { kPackD_2a, kPackD_20 } },
        { "bump reflection E (no glitter)", 0x3884117Eu, 17, { kPackE_2a, kPackE_20 } },
    };
    constexpr int kFamilies = int(sizeof(kFamilyNames) / sizeof(kFamilyNames[0]));
    constexpr int kGameCount = int(sizeof(kGameShaders) / sizeof(kGameShaders[0]));

    // Every ps_1_1 shader the game created on the current device: which game shader it is and our replacement.
    // Each entry holds a reference on the game's object, so its address can't be reused by another shader while
    // it's in here; the table is emptied when the game creates a new device (it re-creates all its shaders then).
    struct Entry { int game = -1; IDirect3DPixelShader9* ours = nullptr; };
    std::unordered_map<IDirect3DPixelShader9*, Entry> g_shaders;
    IDirect3DPixelShader9* g_magenta = nullptr;
    bool g_on = true, g_init = false;
    bool g_keyDown = false, g_hlKeyDown = false;
    int g_hlFamily = -1;   // highlighted family (-1 off)
    int g_hlGame = -1;     // a single highlighted game shader within it (-1: the whole family)
    int g_logged = 0;

    uint32_t Fnv(const DWORD* code, size_t n)
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < n; ++i) { h ^= code[i]; h *= 16777619u; }
        return h;
    }
    int GameIndex(uint32_t h, size_t n)
    {
        for (int i = 0; i < kGameCount; ++i)
            if (kGameShaders[i].hash == h && kGameShaders[i].length == n) return i;
        return -1;
    }
}

void ShaderPack::OnCreated(IDirect3DDevice9* dev, const DWORD* fn, IDirect3DPixelShader9* original, PFN_CreatePS create)
{
    if (!g_init) { g_init = true; g_on = g_settings.shaderPack; }
    const bool wanted = g_settings.shaderPack || g_settings.shaderPackToggleKey > 0 || g_settings.shaderHighlightKey > 0;
    if (!original || !fn || fn[0] != 0xFFFF0101 || !wanted)
        return;
    size_t n = 1;
    while (n < 4096 && fn[n - 1] != 0x0000FFFF) ++n;
    const uint32_t h = Fnv(fn, n);

    Entry e;
    e.game = GameIndex(h, n);
    if (g_settings.shaderHighlightKey > 0 && !g_magenta)
        create(dev, reinterpret_cast<const DWORD*>(kPackMagenta_20), &g_magenta);
    for (const auto& p : kPrograms)
    {
        if (p.hash != h || p.length != n)
            continue;
        for (int b = 0; b < 2 && !e.ours; ++b)
        {
            const HRESULT hr = create(dev, reinterpret_cast<const DWORD*>(p.builds[b]), &e.ours);
            if (SUCCEEDED(hr) && e.ours)
            {
                if (g_logged++ < 12)
                    LOG("ShaderPack: %s -> %s", p.name, b == 0 ? "ps_2_a (with specular anti-aliasing)" : "ps_2_0 (fallback)");
            }
            else
            {
                e.ours = nullptr;
                LOG("ShaderPack: %s, %s build refused by the driver (0x%08X)", p.name, b == 0 ? "ps_2_a" : "ps_2_0", unsigned(hr));
            }
        }
        break;
    }
    if (e.game < 0 && !e.ours)
        return;
    auto it = g_shaders.find(original);
    if (it != g_shaders.end())
    {
        if (it->second.ours) it->second.ours->Release();
        it->second = e;
    }
    else
    {
        original->AddRef();
        g_shaders.emplace(original, e);
    }
}

void ShaderPack::OnNewDevice()
{
    if (!g_shaders.empty())
        LOG("ShaderPack: new device, dropping %u shader entr%s from the old one", unsigned(g_shaders.size()),
            g_shaders.size() == 1 ? "y" : "ies");
    for (auto& [original, e] : g_shaders)
    {
        if (e.ours) e.ours->Release();
        original->Release();
    }
    g_shaders.clear();
    if (g_magenta) { g_magenta->Release(); g_magenta = nullptr; }
}

IDirect3DPixelShader9* ShaderPack::Swap(IDirect3DPixelShader9* shader)
{
    if (!shader || g_shaders.empty() || (!g_on && g_hlFamily < 0))
        return shader;
    auto it = g_shaders.find(shader);
    if (it == g_shaders.end())
        return shader;
    const Entry& e = it->second;
    if (g_hlFamily >= 0 && g_magenta && e.game >= 0 && kGameShaders[e.game].family == g_hlFamily && (g_hlGame < 0 || g_hlGame == e.game))
        return g_magenta;
    return g_on && e.ours ? e.ours : shader;
}

void ShaderPack::OnPresent()
{
    const bool focused = Game::hWnd && GetForegroundWindow() == Game::hWnd;
    if (const int key = g_settings.shaderPackToggleKey; key > 0)
    {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        if (down && !g_keyDown && focused)
        {
            g_on = !g_on;
            LOG("ShaderPack: %s (toggle key)", g_on ? "ON - the remaster's shaders" : "OFF - the game's original shaders");
        }
        g_keyDown = down;
    }
    if (const int key = g_settings.shaderHighlightKey; key > 0)
    {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        if (down && !g_hlKeyDown && focused)
        {
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!shift || g_hlFamily < 0)
            {
                g_hlFamily = g_hlFamily + 1 >= kFamilies ? -1 : g_hlFamily + 1;
                g_hlGame = -1;
                if (g_hlFamily < 0)
                    LOG("[HIGHLIGHT] off");
                else
                {
                    int count = 0;
                    for (int i = 0; i < kGameCount; ++i) count += kGameShaders[i].family == g_hlFamily;
                    LOG("[HIGHLIGHT] family %d: %s (%d shaders) in magenta - Shift+F11 steps through them one by one", g_hlFamily,
                        kFamilyNames[g_hlFamily], count);
                }
            }
            else
            {
                int next = -1;
                for (int i = g_hlGame + 1; i < kGameCount; ++i)
                    if (kGameShaders[i].family == g_hlFamily) { next = i; break; }
                g_hlGame = next;
                if (next < 0)
                    LOG("[HIGHLIGHT] family %d: %s, all shaders again", g_hlFamily, kFamilyNames[g_hlFamily]);
                else
                    LOG("[HIGHLIGHT] shader %08X (%s, %d instructions) in magenta", kGameShaders[next].hash, kFamilyNames[g_hlFamily],
                        kGameShaders[next].instructions);
            }
        }
        g_hlKeyDown = down;
    }
}
