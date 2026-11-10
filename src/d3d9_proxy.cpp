// d3d9.dll proxy: loads the system Direct3D 9 runtime (or a configured wrapper such as
// DXVK/wined3d), forwards every export and hooks device creation for the D3D9-side fixes.
#include "pch.h"
#include "features.h"

extern HMODULE g_self;

namespace
{
    HMODULE g_real = nullptr;
    INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;

    struct RealExports
    {
        void* Direct3DCreate9;
        void* Direct3DCreate9Ex;
        void* D3DPERF_BeginEvent;
        void* D3DPERF_EndEvent;
        void* D3DPERF_GetStatus;
        void* D3DPERF_QueryRepeatFrame;
        void* D3DPERF_SetMarker;
        void* D3DPERF_SetOptions;
        void* D3DPERF_SetRegion;
        void* DebugSetMute;
        void* Direct3DShaderValidatorCreate9;
        void* PSGPError;
        void* PSGPSampleTexture;
        void* DebugSetLevel;
        void* Direct3D9EnableMaximizedWindowedModeShim;
    } g_exp = {};

    std::wstring ModuleDir(HMODULE m)
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(m, path, MAX_PATH);
        std::wstring p(path);
        return p.substr(0, p.find_last_of(L"\\/") + 1);
    }

    bool g_dxvk = false;   // the chained runtime is DXVK (and it has not failed)

    void ResolveExports()
    {
        g_exp = {};
        if (!g_real)
            return;
#define RESOLVE(n) g_exp.n = reinterpret_cast<void*>(GetProcAddress(g_real, #n))
        RESOLVE(Direct3DCreate9);
        RESOLVE(Direct3DCreate9Ex);
        RESOLVE(D3DPERF_BeginEvent);
        RESOLVE(D3DPERF_EndEvent);
        RESOLVE(D3DPERF_GetStatus);
        RESOLVE(D3DPERF_QueryRepeatFrame);
        RESOLVE(D3DPERF_SetMarker);
        RESOLVE(D3DPERF_SetOptions);
        RESOLVE(D3DPERF_SetRegion);
        RESOLVE(DebugSetMute);
        RESOLVE(Direct3DShaderValidatorCreate9);
        RESOLVE(PSGPError);
        RESOLVE(PSGPSampleTexture);
        RESOLVE(DebugSetLevel);
        RESOLVE(Direct3D9EnableMaximizedWindowedModeShim);
#undef RESOLVE
    }

    void LoadSystem()
    {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring path = std::wstring(sys) + L"\\d3d9.dll";
        g_real = LoadLibraryW(path.c_str());
        LOG("D3D9: system runtime '%ls' -> %s", path.c_str(), g_real ? "ok" : "FAILED");
    }

    BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*)
    {
        if (!g_settings.d3d9Chain.empty())
        {
            std::wstring chain = ModuleDir(g_self) + g_settings.d3d9Chain;
            if (_wcsicmp(g_settings.d3d9Chain.c_str(), L"d3d9.dll") != 0)
                g_real = LoadLibraryW(chain.c_str());
            LOG("D3D9: chain-loading '%ls' -> %s", chain.c_str(), g_real ? "ok" : "FAILED, falling back to system d3d9");
            if (g_real)
            {
                std::wstring lower = g_settings.d3d9Chain;
                for (auto& c : lower) c = towlower(c);
                g_dxvk = lower.find(L"dxvk") != std::wstring::npos;
            }
        }
        if (!g_real)
            LoadSystem();
        ResolveExports();
        return TRUE;
    }

    // The chained runtime could not create Direct3D (DXVK needs a Vulkan 1.3 GPU and driver): switch to
    // Windows' own D3D9 for the rest of the session.
    bool FallBackToSystem()
    {
        if (!g_real || g_settings.d3d9Chain.empty())
            return false;
        wchar_t sys[MAX_PATH], mod[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        GetModuleFileNameW(g_real, mod, MAX_PATH);
        if (_wcsnicmp(mod, sys, wcslen(sys)) == 0)
            return false;   // already the system runtime
        LOG("D3D9: '%ls' could not create Direct3D (no Vulkan support?); falling back to system d3d9", mod);
        g_dxvk = false;
        LoadSystem();
        ResolveExports();
        return g_exp.Direct3DCreate9 != nullptr;
    }
}

extern "C" void __cdecl EnsureRealD3D9();

bool D3D9Proxy::UsingDxvk()
{
    EnsureRealD3D9();   // may run before the game first touches D3D9 (frame limiter set-up)
    return g_dxvk;
}

extern "C" void __cdecl EnsureRealD3D9()
{
    InitOnceExecuteOnce(&g_loadOnce, LoadReal, nullptr, nullptr);
}

// ---- Exports ------------------------------------------------------------------------------

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk)
{
    EnsureRealD3D9();
    if (!g_exp.Direct3DCreate9)
        return nullptr;
    auto d3d = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(g_exp.Direct3DCreate9)(sdk);
    if (!d3d && FallBackToSystem())
        d3d = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(g_exp.Direct3DCreate9)(sdk);
    LOG("D3D9: Direct3DCreate9(%u) -> %p%s", sdk, d3d, g_dxvk ? " (DXVK)" : "");
    if (d3d)
        D3D9Hooks::OnCreate(d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    EnsureRealD3D9();
    if (!g_exp.Direct3DCreate9Ex)
        return D3DERR_NOTAVAILABLE;
    HRESULT hr = reinterpret_cast<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>(g_exp.Direct3DCreate9Ex)(sdk, out);
    if (SUCCEEDED(hr) && out && *out)
        D3D9Hooks::OnCreate(*out);
    return hr;
}

// Signature-agnostic forwarders.
#define FORWARD(name)                                              \
    extern "C" __declspec(naked) void Proxy_##name()               \
    {                                                              \
        __asm { call EnsureRealD3D9 }                              \
        __asm { mov eax, dword ptr [g_exp.name] }                  \
        __asm { test eax, eax }                                    \
        __asm { jz missing }                                       \
        __asm { jmp eax }                                          \
        __asm { missing: xor eax, eax }                            \
        __asm { ret }                                              \
    }

FORWARD(D3DPERF_BeginEvent)
FORWARD(D3DPERF_EndEvent)
FORWARD(D3DPERF_GetStatus)
FORWARD(D3DPERF_QueryRepeatFrame)
FORWARD(D3DPERF_SetMarker)
FORWARD(D3DPERF_SetOptions)
FORWARD(D3DPERF_SetRegion)
FORWARD(DebugSetMute)
FORWARD(Direct3DShaderValidatorCreate9)
FORWARD(PSGPError)
FORWARD(PSGPSampleTexture)
FORWARD(DebugSetLevel)
FORWARD(Direct3D9EnableMaximizedWindowedModeShim)
