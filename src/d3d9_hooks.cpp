// IDirect3D9 / IDirect3DDevice9 vtable hooks: GPU vendor spoofing, refresh rate / VSync
// overrides, diagnostics and the AMD thermal-vision shader replacement.
#include "pch.h"
#include "features.h"
#include "d3d9_capture.h"
#include "perf.h"

namespace
{
    // IDirect3D9 vtable indices
    constexpr size_t kD3D_GetAdapterIdentifier = 5;
    constexpr size_t kD3D_CreateDevice = 16;
    // IDirect3DDevice9 vtable indices
    constexpr size_t kDev_Reset = 16;
    constexpr size_t kDev_Present = 17;
    constexpr size_t kDev_CreateTexture = 23;
    constexpr size_t kDev_SetRenderTarget = 37;
    constexpr size_t kDev_Clear = 43;
    constexpr size_t kDev_SetRenderState = 57;
    constexpr size_t kDev_SetTexture = 65;
    constexpr size_t kDev_SetTextureStageState = 67;
    constexpr size_t kDev_DrawPrimitive = 81;
    constexpr size_t kDev_DrawIndexedPrimitive = 82;
    constexpr size_t kDev_DrawPrimitiveUP = 83;
    constexpr size_t kDev_DrawIndexedPrimitiveUP = 84;
    constexpr size_t kDev_SetFVF = 89;
    constexpr size_t kDev_CreateVertexShader = 91;
    constexpr size_t kDev_SetVertexShader = 92;
    constexpr size_t kDev_CreatePixelShader = 106;
    constexpr size_t kDev_SetPixelShader = 107;
    constexpr size_t kDev_SetPixelShaderConstantF = 109;

    // Capture-only hooks (installed when [Debug] CaptureKey is set).
    using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using SetRT_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    using Clear_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
    using SetRS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    using SetTex_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    using DP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using DIP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    using DPUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using DIPUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
    using SetFVF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD);
    using CreateVS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
    using SetVS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    Present_t oPresent = nullptr; SetRT_t oSetRenderTarget = nullptr; Clear_t oClear = nullptr; SetRS_t oSetRenderState = nullptr;
    SetTex_t oSetTexture = nullptr; DP_t oDrawPrimitive = nullptr; DIP_t oDrawIndexedPrimitive = nullptr; DPUP_t oDrawPrimitiveUP = nullptr;
    DIPUP_t oDrawIndexedPrimitiveUP = nullptr; SetFVF_t oSetFVF = nullptr; CreateVS_t oCreateVertexShader = nullptr; SetVS_t oSetVertexShader = nullptr;

    HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
    {
        Plush::OnPresent(d);    // the Niko plush, when it wasn't drawn into the HDR scene this frame
        Editor::OnPresent(d);   // the editor overlay goes into this frame
        const int64_t t = Perf::Begin();
        HRESULT hr = oPresent(d, a, b, w, r);
        Perf::AddPresent(t);
        Capture::OnPresent();
        ShaderPack::OnPresent();
        Textures::OnPresent();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s)
    {
        Plush::OnSetRenderTarget(d, i, s);   // before the switch: the plush goes into the scene before post processing
        Capture::OnSetRenderTarget(i, s);
        return oSetRenderTarget(d, i, s);
    }
    using SetVSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
    SetVSConstF_t oSetVertexShaderConstantF = nullptr;
    HRESULT STDMETHODCALLTYPE hkSetVertexShaderConstantF(IDirect3DDevice9* d, UINT start, const float* data, UINT count)
    {
        Plush::OnSetVSConstants(d, start, data, count);   // finds the game's camera matrix (and draws the plush there)
        return oSetVertexShaderConstantF(d, start, data, count);
    }
    HRESULT STDMETHODCALLTYPE hkClear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD s)
    {
        Capture::OnClear(f, c);
        Plush::OnBeforeClear(d, f);   // thermal: the plush goes in before its depth clear
        const HRESULT hr = oClear(d, n, r, f, c, z, s);
        Plush::OnClear(f);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE s, DWORD v) { Capture::OnSetRenderState(s, v); return oSetRenderState(d, s, v); }
    HRESULT STDMETHODCALLTYPE hkSetTexture(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t)
    {
        t = Textures::Substitute(t);   // texture packs: the mod's replacement instead of the game's texture
        Capture::OnSetTexture(s, t);
        return oSetTexture(d, s, t);
    }
    using StretchRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                                      D3DTEXTUREFILTERTYPE);
    StretchRect_t oStretchRect = nullptr;
    HRESULT STDMETHODCALLTYPE hkStretchRect(IDirect3DDevice9* d, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* t, const RECT* tr,
                                            D3DTEXTUREFILTERTYPE f)
    {
        Plush::OnStretchRect(d, s);   // goggles: the plush goes into the back buffer before their filter's copy of it
        Capture::OnStretchRect(s, t);
        return oStretchRect(d, s, sr, t, tr, f);
    }
    using UpdateTex_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
    UpdateTex_t oUpdateTexture = nullptr;
    HRESULT STDMETHODCALLTYPE hkUpdateTexture(IDirect3DDevice9* d, IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst)
    {
        const HRESULT hr = oUpdateTexture(d, src, dst);
        if (SUCCEEDED(hr)) Textures::OnUpdateTexture(src, dst);
        return hr;
    }
    // [Debug] LogHudImages snapshot (F9): log each distinct draw call with the game code that issued it
    // (return addresses found on the stack) and, for UP draws, the first vertices' screen positions.
    int ScanReturnAddresses(void* esp, uint32_t (&ret)[10])
    {
        const auto text = Mem::ExeText();
        int n = 0;
        __try
        {
            const uint32_t* sp = reinterpret_cast<const uint32_t*>(esp);
            for (int i = 0; i < 768 && n < 10; ++i)
            {
                const uint32_t v = sp[i];
                if (v - text.start >= text.size || v - text.start < 6)
                    continue;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
                if (p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
                    (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10))
                    ret[n++] = v;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    int FormatVertices(const void* verts, UINT stride, char* buf, int len, int size)
    {
        __try
        {
            for (int i = 0; i < 4 && len < size - 40; ++i)
            {
                const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(verts) + i * stride);
                len += sprintf_s(buf + len, size - len, " v%d=(%.0f %.0f)", i, f[0], f[1]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return len;
    }

    // [Debug] LogHudImages snapshot (F9): log each distinct draw call with the game code that issued it
    // (return addresses found on the stack) and, for UP draws, the first vertices' screen positions.
    void LogDraw(const char* kind, UINT prims, const void* verts, UINT stride, void* esp)
    {
        static std::unordered_set<uint64_t> seen;
        uint32_t ret[10] = {};
        const int n = ScanReturnAddresses(esp, ret);
        uint64_t key = prims;
        for (int i = 0; i < n; ++i)
            key = key * 0x100000001B3ull ^ ret[i];
        if (!seen.insert(key).second)
            return;
        char buf[512];
        int len = sprintf_s(buf, "[DRAW] %s prims=%u stack:", kind, prims);
        for (int i = 0; i < n && len < 400; ++i)
            len += sprintf_s(buf + len, sizeof(buf) - len, " %08X", ret[i]);
        if (verts && stride >= 8)
            len = FormatVertices(verts, stride, buf, len, sizeof(buf));
        LOG("%s", buf);
    }

    HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT n) { if (Widescreen::SnapshotActive()) LogDraw("DP", n, nullptr, 0, _AddressOfReturnAddress()); Capture::OnDraw("DP", t, n); Plush::OnDraw(d); Perf::CountDraw(); return oDrawPrimitive(d, t, s, n); }
    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT pc) { if (Widescreen::SnapshotActive()) LogDraw("DIP", pc, nullptr, 0, _AddressOfReturnAddress()); Capture::OnDraw("DIP", t, pc); Plush::OnDraw(d); Perf::CountDraw(); return oDrawIndexedPrimitive(d, t, b, mi, nv, si, pc); }
    HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT pc, const void* v, UINT st) { if (Widescreen::SnapshotActive()) LogDraw("DPUP", pc, v, st, _AddressOfReturnAddress()); Capture::OnDraw("DPUP", t, pc); Plush::OnDraw(d); Perf::CountDraw(); return oDrawPrimitiveUP(d, t, pc, v, st); }
    HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void* i, D3DFORMAT f, const void* v, UINT st) { if (Widescreen::SnapshotActive()) LogDraw("DIPUP", pc, v, st, _AddressOfReturnAddress()); Capture::OnDraw("DIPUP", t, pc); Plush::OnDraw(d); Perf::CountDraw(); return oDrawIndexedPrimitiveUP(d, t, mi, nv, pc, i, f, v, st); }
    HRESULT STDMETHODCALLTYPE hkSetFVF(IDirect3DDevice9* d, DWORD f) { Capture::OnSetFVF(f); return oSetFVF(d, f); }
    HRESULT STDMETHODCALLTYPE hkCreateVertexShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** out)
    {
        Perf::CountShader();
        HRESULT hr = oCreateVertexShader(d, fn, out);
        if (SUCCEEDED(hr) && out) Capture::OnShaderCreated(*out, fn);
        if (SUCCEEDED(hr) && out) Plush::OnCreateVertexShader(*out, fn);   // thermal's scene shaders
        return hr;
    }
    // Profiler-only hooks.
    using CreateVB_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
    using CreateIB_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
    using GetRTData_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
    using CreateQuery_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
    using QueryGetData_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, void*, DWORD, DWORD);
    CreateVB_t oCreateVertexBuffer = nullptr; CreateIB_t oCreateIndexBuffer = nullptr; GetRTData_t oGetRenderTargetData = nullptr;
    CreateQuery_t oCreateQuery = nullptr; QueryGetData_t oQueryGetData = nullptr;
    HRESULT STDMETHODCALLTYPE hkCreateVertexBuffer(IDirect3DDevice9* d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** vb, HANDLE* h)
    { Perf::CountCreate(); return oCreateVertexBuffer(d, len, usage, fvf, pool, vb, h); }
    HRESULT STDMETHODCALLTYPE hkCreateIndexBuffer(IDirect3DDevice9* d, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9** ib, HANDLE* h)
    { Perf::CountCreate(); return oCreateIndexBuffer(d, len, usage, fmt, pool, ib, h); }
    HRESULT STDMETHODCALLTYPE hkGetRenderTargetData(IDirect3DDevice9* d, IDirect3DSurface9* rt, IDirect3DSurface9* dst)
    { const int64_t t = Perf::Begin(); HRESULT hr = oGetRenderTargetData(d, rt, dst); Perf::AddReadback(t); return hr; }
    HRESULT STDMETHODCALLTYPE hkQueryGetData(IDirect3DQuery9* q, void* data, DWORD size, DWORD flags)
    { const int64_t t = Perf::Begin(); HRESULT hr = oQueryGetData(q, data, size, flags); Perf::AddQueryWait(t, hr == S_FALSE); return hr; }
    HRESULT STDMETHODCALLTYPE hkCreateQuery(IDirect3DDevice9* d, D3DQUERYTYPE type, IDirect3DQuery9** q)
    {
        HRESULT hr = oCreateQuery(d, type, q);
        if (SUCCEEDED(hr) && q && *q)
        {
            void** vt = *reinterpret_cast<void***>(*q);
            if (vt[7] != reinterpret_cast<void*>(&hkQueryGetData))
            {
                static bool once = false;
                void* prev = Mem::PatchVTable(vt, 7, reinterpret_cast<void*>(&hkQueryGetData));
                if (!once) { once = true; oQueryGetData = reinterpret_cast<QueryGetData_t>(prev); }
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s)
    {
        Capture::OnSetVertexShader(s);
        Plush::OnSetVertexShader(s);
        return oSetVertexShader(d, s);
    }

    using GetAdapterIdentifier_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, DWORD, D3DADAPTER_IDENTIFIER9*);
    using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
    using SetTSS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
    using CreatePS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
    using SetPS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
    using SetPSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

    GetAdapterIdentifier_t oGetAdapterIdentifier = nullptr;
    CreateDevice_t oCreateDevice = nullptr;
    Reset_t oReset = nullptr;
    CreateTexture_t oCreateTexture = nullptr;
    SetTSS_t oSetTextureStageState = nullptr;
    CreatePS_t oCreatePixelShader = nullptr;
    SetPS_t oSetPixelShader = nullptr;
    SetPSConstF_t oSetPixelShaderConstantF = nullptr;

    IDirect3D9* g_d3d = nullptr;

    const char* FormatName(D3DFORMAT f)
    {
        switch (f)
        {
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_R5G6B5: return "R5G6B5";
        case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
        case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
        case D3DFMT_R16F: return "R16F";
        case D3DFMT_R32F: return "R32F";
        case D3DFMT_G16R16F: return "G16R16F";
        case D3DFMT_A8L8: return "A8L8";
        case D3DFMT_L8: return "L8";
        case D3DFMT_L16: return "L16";
        case D3DFMT_A8: return "A8";
        case D3DFMT_D24S8: return "D24S8";
        case D3DFMT_D24X8: return "D24X8";
        case D3DFMT_D16: return "D16";
        case D3DFMT_V8U8: return "V8U8";
        case D3DFMT_Q8W8V8U8: return "Q8W8V8U8";
        case D3DFMT_DXT1: return "DXT1";
        case D3DFMT_DXT3: return "DXT3";
        case D3DFMT_DXT5: return "DXT5";
        default: return nullptr;
        }
    }

    void ApplyPresentOverrides(D3DPRESENT_PARAMETERS* pp, UINT adapter)
    {
        if (!pp)
            return;
        const auto& s = g_settings;
        // VSync -1 (auto): on with DXVK (Vulkan vsync paces frames smoothly, also in a window). With
        // Windows' D3D9: off in a window (its windowed vsync stalls ~30 ms now and then; the
        // refresh-synced limiter paces frames instead), the game's own setting in exclusive fullscreen.
        if (s.vsync == 1 || (s.vsync < 0 && D3D9Proxy::UsingDxvk()))
            pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        else if (s.vsync == 0 || (s.vsync < 0 && pp->Windowed))
            pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        if (!pp->Windowed && s.refreshRate >= 0 && g_d3d)
        {
            UINT wanted = static_cast<UINT>(s.refreshRate);
            if (wanted == 0)
            {
                DEVMODEW dm{}; dm.dmSize = sizeof(dm);
                if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
                    wanted = dm.dmDisplayFrequency;
            }
            // Use the requested rate only if the adapter exposes that mode.
            const UINT count = g_d3d->GetAdapterModeCount(adapter, pp->BackBufferFormat);
            UINT best = 0;
            for (UINT i = 0; i < count; ++i)
            {
                D3DDISPLAYMODE m;
                if (FAILED(g_d3d->EnumAdapterModes(adapter, pp->BackBufferFormat, i, &m)))
                    continue;
                if (m.Width != pp->BackBufferWidth || m.Height != pp->BackBufferHeight)
                    continue;
                if (m.RefreshRate == wanted) { best = wanted; break; }
                if (m.RefreshRate <= wanted && m.RefreshRate > best) best = m.RefreshRate;
            }
            if (best)
                pp->FullScreen_RefreshRateInHz = best;
        }
    }

    void LogPresent(const char* what, const D3DPRESENT_PARAMETERS* pp)
    {
        if (!pp) return;
        LOG("D3D9: %s %ux%u fmt=%d windowed=%d refresh=%u interval=0x%X msaa=%d swap=%d",
            what, pp->BackBufferWidth, pp->BackBufferHeight, int(pp->BackBufferFormat), pp->Windowed,
            pp->FullScreen_RefreshRateInHz, pp->PresentationInterval, int(pp->MultiSampleType), int(pp->SwapEffect));
    }

    // ---- IDirect3D9 -----------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE hkGetAdapterIdentifier(IDirect3D9* self, UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id)
    {
        HRESULT hr = oGetAdapterIdentifier(self, adapter, flags, id);
        if (SUCCEEDED(hr) && id)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG("D3D9: adapter %u '%s' vendor=%04X device=%04X driver=%u.%u.%u.%u", adapter, id->Description,
                    id->VendorId, id->DeviceId, HIWORD(id->DriverVersion.HighPart), LOWORD(id->DriverVersion.HighPart),
                    HIWORD(id->DriverVersion.LowPart), LOWORD(id->DriverVersion.LowPart));
            }
            switch (g_settings.spoofVendor)
            {
            case 1: id->VendorId = 0x10DE; break;
            case 2: id->VendorId = 0x1002; break;
            case 3: id->VendorId = 0x8086; break;
            default: break;
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
    {
        ApplyPresentOverrides(pp, 0);
        LogPresent("Reset", pp);
        Editor::OnReset(true);
        Plush::OnReset(true);
        const HRESULT hr = oReset(dev, pp);
        if (SUCCEEDED(hr)) { Editor::OnReset(false); Plush::OnReset(false); }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkCreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                              D3DPOOL pool, IDirect3DTexture9** tex, HANDLE* shared)
    {
        Perf::CountCreate();
        HRESULT hr = oCreateTexture(dev, w, h, levels, usage, fmt, pool, tex, shared);
        if (SUCCEEDED(hr) && tex && *tex)
            Textures::OnCreateTexture(*tex);
        if (g_settings.logRenderTargets && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
        {
            const char* n = FormatName(fmt);
            char buf[16];
            if (!n) { snprintf(buf, sizeof(buf), "%d", int(fmt)); n = buf; }
            LOG("D3D9: CreateTexture %ux%u %s usage=0x%X -> 0x%08X", w, h, n, usage, unsigned(hr));
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkSetTextureStageState(IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
    {
        HRESULT hr = oSetTextureStageState(dev, stage, type, value);
        AmdFix::OnSetTextureStageState(dev, stage, type, value);
        Capture::OnSetTextureStageState(stage, type, value);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkCreatePixelShader(IDirect3DDevice9* dev, const DWORD* fn, IDirect3DPixelShader9** out)
    {
        Perf::CountShader();
        int kind = -1;
        if (const DWORD* repl = AmdFix::Replacement(fn, &kind))
        {
            HRESULT hr = oCreatePixelShader(dev, repl, out);
            if (SUCCEEDED(hr))
            {
                AmdFix::OnCreated(out ? *out : nullptr, kind);
                if (out) Plush::OnCreatePixelShader(*out, fn);
                if (out) Capture::OnShaderCreated(*out, repl);
                return hr;
            }
            LOG("AMDFix: replacement shader %d rejected by the driver (0x%08X), using original", kind, unsigned(hr));
        }
        HRESULT hr = oCreatePixelShader(dev, fn, out);
        if (SUCCEEDED(hr) && out)
        {
            Capture::OnShaderCreated(*out, fn);
            Plush::OnCreatePixelShader(*out, fn);   // thermal's filter (its palette)
            ShaderPack::OnCreated(dev, fn, *out, oCreatePixelShader);
        }
        if (g_settings.logShaders && fn)
        {
            size_t len = 1;
            while (len < 4096 && fn[len - 1] != 0x0000FFFF) ++len;
            uint32_t hash = 2166136261u;
            for (size_t i = 0; i < len; ++i) { hash ^= fn[i]; hash *= 16777619u; }
            LOG("D3D9: CreatePixelShader ver=%08X len=%u hash=%08X -> %p", unsigned(fn[0]), unsigned(len), hash, out ? *out : nullptr);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkSetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* shader)
    {
        HRESULT hr = oSetPixelShader(dev, ShaderPack::Swap(shader));
        AmdFix::OnSetPixelShader(dev, shader);
        Capture::OnSetPixelShader(shader);
        Plush::OnSetPixelShader(shader);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkSetPixelShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count)
    {
        AmdFix::OnSetPixelShaderConstantF(start, data, count);
        Capture::OnSetPixelShaderConstantF(start, data, count);
        return oSetPixelShaderConstantF(dev, start, data, count);
    }

    HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                             D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
    {
        ApplyPresentOverrides(pp, adapter);
        LogPresent("CreateDevice", pp);
        // The game makes a new device per map change: the editor overlay must let go of the old one first, or its
        // references keep the old device (and its swap chain on the same window) alive.
        Editor::OnNewDevice();
        Plush::OnNewDevice();
        HRESULT hr = oCreateDevice(self, adapter, type, focus, flags, pp, out);
        LOG("D3D9: CreateDevice -> 0x%08X", unsigned(hr));
        if (SUCCEEDED(hr) && out && *out)
        {
            Game::hWnd = pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus;
            D3D9Hooks::OnDevice(*out);
        }
        return hr;
    }
}

void D3D9Hooks::OnCreate(IDirect3D9* d3d)
{
    g_d3d = d3d;
    void** vt = *reinterpret_cast<void***>(d3d);
    if (vt[kD3D_CreateDevice] != reinterpret_cast<void*>(&hkCreateDevice))
    {
        oCreateDevice = reinterpret_cast<CreateDevice_t>(Mem::PatchVTable(vt, kD3D_CreateDevice, reinterpret_cast<void*>(&hkCreateDevice)));
        oGetAdapterIdentifier = reinterpret_cast<GetAdapterIdentifier_t>(Mem::PatchVTable(vt, kD3D_GetAdapterIdentifier, reinterpret_cast<void*>(&hkGetAdapterIdentifier)));
    }
}

void D3D9Hooks::OnDevice(IDirect3DDevice9* dev)
{
    ShaderPack::OnNewDevice();
    void** vt = *reinterpret_cast<void***>(dev);
    if (vt[kDev_CreatePixelShader] == reinterpret_cast<void*>(&hkCreatePixelShader))
        return; // shared vtable, already hooked
    oReset = reinterpret_cast<Reset_t>(Mem::PatchVTable(vt, kDev_Reset, reinterpret_cast<void*>(&hkReset)));
    oCreateTexture = reinterpret_cast<CreateTexture_t>(Mem::PatchVTable(vt, kDev_CreateTexture, reinterpret_cast<void*>(&hkCreateTexture)));
    oSetTextureStageState = reinterpret_cast<SetTSS_t>(Mem::PatchVTable(vt, kDev_SetTextureStageState, reinterpret_cast<void*>(&hkSetTextureStageState)));
    oCreatePixelShader = reinterpret_cast<CreatePS_t>(Mem::PatchVTable(vt, kDev_CreatePixelShader, reinterpret_cast<void*>(&hkCreatePixelShader)));
    oSetPixelShader = reinterpret_cast<SetPS_t>(Mem::PatchVTable(vt, kDev_SetPixelShader, reinterpret_cast<void*>(&hkSetPixelShader)));
    oSetPixelShaderConstantF = reinterpret_cast<SetPSConstF_t>(Mem::PatchVTable(vt, kDev_SetPixelShaderConstantF, reinterpret_cast<void*>(&hkSetPixelShaderConstantF)));

#define HOOK(idx, fn, orig) orig = reinterpret_cast<decltype(orig)>(Mem::PatchVTable(vt, idx, reinterpret_cast<void*>(&fn)))
    // Frame boundary, draw counting and shader creation: used by the profiler and the capture.
    HOOK(kDev_Present, hkPresent, oPresent);
    HOOK(kDev_DrawPrimitive, hkDrawPrimitive, oDrawPrimitive);
    HOOK(kDev_DrawIndexedPrimitive, hkDrawIndexedPrimitive, oDrawIndexedPrimitive);
    HOOK(kDev_DrawPrimitiveUP, hkDrawPrimitiveUP, oDrawPrimitiveUP);
    HOOK(kDev_DrawIndexedPrimitiveUP, hkDrawIndexedPrimitiveUP, oDrawIndexedPrimitiveUP);
    HOOK(kDev_CreateVertexShader, hkCreateVertexShader, oCreateVertexShader);
    if (g_settings.perfLog)
    {
        HOOK(26, hkCreateVertexBuffer, oCreateVertexBuffer);
        HOOK(27, hkCreateIndexBuffer, oCreateIndexBuffer);
        HOOK(32, hkGetRenderTargetData, oGetRenderTargetData);
        HOOK(118, hkCreateQuery, oCreateQuery);
    }
    const bool capture = g_settings.captureKey > 0 || g_settings.logShaderInputs;
    if (capture || Textures::Active())
        HOOK(kDev_SetTexture, hkSetTexture, oSetTexture);
    if (Textures::Active())
        HOOK(31, hkUpdateTexture, oUpdateTexture);   // IDirect3DDevice9::UpdateTexture
    if (capture || Plush::Available())
        HOOK(kDev_SetRenderTarget, hkSetRenderTarget, oSetRenderTarget);
    if (Plush::Available())
        HOOK(94, hkSetVertexShaderConstantF, oSetVertexShaderConstantF);   // IDirect3DDevice9::SetVertexShaderConstantF
    if (capture || Plush::Available())
        HOOK(kDev_Clear, hkClear, oClear);
    if (capture || Plush::Available())
        HOOK(34, hkStretchRect, oStretchRect);   // IDirect3DDevice9::StretchRect
    if (capture || Plush::Available())
        HOOK(kDev_SetVertexShader, hkSetVertexShader, oSetVertexShader);   // (the plush: thermal's scene shaders)
    if (capture)
    {
        HOOK(kDev_SetRenderState, hkSetRenderState, oSetRenderState);
        HOOK(kDev_SetFVF, hkSetFVF, oSetFVF);
        LOG("D3D9: frame capture armed (key 0x%X)", g_settings.captureKey);
    }
#undef HOOK
    LOG("D3D9: device hooks installed");
}

// ===== AMD thermal vision fix ==================================================================
//
// Chaos Theory ships 78 hand-written ps_1_1 shaders. Three of them use the legacy
// "texture addressing" instructions (texreg2gb, texbem, texm3x3vspec) that only exist in
// ps_1_x and that current AMD D3D9 drivers translate incorrectly. The texreg2gb one is a
// palette lookup (the heat gradient), which matches the "everything renders cold" symptom.
// Each is replaced with an equivalent ps_2_0 shader. Exact bytecode match only.

namespace
{
    // Original game bytecode (ps_1_1).
    const DWORD kOrigPalette[] = { // tex t0; tex t1; texreg2gb t2, t0; mul_x2 r0, t2, t1
        0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000042, 0xB00F0001, 0x00000046, 0xB00F0002, 0xB0E40000,
        0x00000005, 0x810F0000, 0xB0E40002, 0xB0E40001, 0x0000FFFF };
    const DWORD kOrigBumpEnv[] = { // tex t0; texbem t3, t0; mov r0, t3
        0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000043, 0xB00F0003, 0xB0E40000, 0x00000001, 0x800F0000,
        0xB0E40003, 0x0000FFFF };
    const DWORD kOrigEnvSpec[] = { // tex t0; texm3x3pad t1, t0_bx2; texm3x3pad t2, t0_bx2; texm3x3vspec t3, t0_bx2; mul r0, t3, v0
        0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000049, 0xB00F0001, 0xB4E40000, 0x00000049, 0xB00F0002,
        0xB4E40000, 0x0000004D, 0xB00F0003, 0xB4E40000, 0x00000005, 0x800F0000, 0xB0E40003, 0x90E40000, 0x0000FFFF };

    // ps_2_0 replacements (hand-assembled).
    const DWORD kNewPalette[] = {
        0xFFFF0200,
        0x0200001F, 0x80000000, 0xB0030000,               // dcl t0.xy
        0x0200001F, 0x80000000, 0xB0030001,               // dcl t1.xy
        0x0200001F, 0x90000000, 0xA00F0800,               // dcl_2d s0
        0x0200001F, 0x90000000, 0xA00F0801,               // dcl_2d s1
        0x0200001F, 0x90000000, 0xA00F0802,               // dcl_2d s2
        0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,   // texld r0, t0, s0
        0x03000042, 0x800F0001, 0xB0E40001, 0xA0E40801,   // texld r1, t1, s1
        0x02000001, 0x800F0002, 0x80C90000,               // mov r2, r0.yzxw        (green/blue -> u/v)
        0x03000042, 0x800F0002, 0x80E40002, 0xA0E40802,   // texld r2, r2, s2       (palette lookup)
        0x03000005, 0x800F0000, 0x80E40002, 0x80E40001,   // mul r0, r2, r1
        0x03000002, 0x800F0000, 0x80E40000, 0x80E40000,   // add r0, r0, r0         (_x2)
        0x02000001, 0x800F0800, 0x80E40000,               // mov oC0, r0
        0x0000FFFF };

    // c30 = (M00, M01, 0, 0), c31 = (M10, M11, 0, 0) of stage 3, managed by us while bound.
    // (ps_2_0 has no arbitrary swizzles, so the matrix rows go into two registers.)
    constexpr UINT kBumpConst = 30;
    const DWORD kNewBumpEnv[] = {
        0xFFFF0200,
        0x0200001F, 0x80000000, 0xB0030000,               // dcl t0.xy
        0x0200001F, 0x80000000, 0xB0030003,               // dcl t3.xy
        0x0200001F, 0x90000000, 0xA00F0800,               // dcl_2d s0
        0x0200001F, 0x90000000, 0xA00F0803,               // dcl_2d s3
        0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,   // texld r0, t0, s0       (du, dv)
        0x04000004, 0x800F0001, 0x80000000, 0xA0E4001E, 0xB0E40003, // mad r1, r0.xxxx, c30, t3
        0x04000004, 0x800F0001, 0x80550000, 0xA0E4001F, 0x80E40001, // mad r1, r0.yyyy, c31, r1
        0x03000042, 0x800F0001, 0x80E40001, 0xA0E40803,   // texld r1, r1, s3
        0x02000001, 0x800F0800, 0x80E40001,               // mov oC0, r1
        0x0000FFFF };

    const DWORD kNewEnvSpec[] = {
        0xFFFF0200,
        0x05000051, 0xA00F001F, 0x40000000, 0xBF800000, 0x00000000, 0x00000000, // def c31, 2, -1, 0, 0
        0x0200001F, 0x80000000, 0xB00F0000,               // dcl t0
        0x0200001F, 0x80000000, 0xB00F0001,               // dcl t1
        0x0200001F, 0x80000000, 0xB00F0002,               // dcl t2
        0x0200001F, 0x80000000, 0xB00F0003,               // dcl t3
        0x0200001F, 0x80000000, 0x900F0000,               // dcl v0
        0x0200001F, 0x90000000, 0xA00F0800,               // dcl_2d s0
        0x0200001F, 0x98000000, 0xA00F0803,               // dcl_cube s3
        0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,   // texld r0, t0, s0
        0x04000004, 0x800F0000, 0x80E40000, 0xA000001F, 0xA055001F, // mad r0, r0, c31.xxxx, c31.yyyy  (_bx2)
        0x02000001, 0x800F0001, 0xA0AA001F,               // mov r1, c31.zzzz
        0x02000001, 0x800F0002, 0xA0AA001F,               // mov r2, c31.zzzz
        0x03000008, 0x80010001, 0xB0E40001, 0x80E40000,   // dp3 r1.x, t1, r0
        0x03000008, 0x80020001, 0xB0E40002, 0x80E40000,   // dp3 r1.y, t2, r0
        0x03000008, 0x80040001, 0xB0E40003, 0x80E40000,   // dp3 r1.z, t3, r0       (N)
        0x02000001, 0x80010002, 0xB0FF0001,               // mov r2.x, t1.wwww
        0x02000001, 0x80020002, 0xB0FF0002,               // mov r2.y, t2.wwww
        0x02000001, 0x80040002, 0xB0FF0003,               // mov r2.z, t3.wwww      (E)
        0x03000008, 0x80010003, 0x80E40001, 0x80E40002,   // dp3 r3.x, r1, r2       (N.E)
        0x03000008, 0x80020003, 0x80E40001, 0x80E40001,   // dp3 r3.y, r1, r1       (N.N)
        0x02000006, 0x80020003, 0x80550003,               // rcp r3.y, r3.yyyy
        0x03000005, 0x80010003, 0x80000003, 0x80550003,   // mul r3.x, r3.xxxx, r3.yyyy
        0x03000002, 0x80010003, 0x80000003, 0x80000003,   // add r3.x, r3.xxxx, r3.xxxx
        0x04000004, 0x800F0004, 0x80E40001, 0x80000003, 0x81E40002, // mad r4, r1, r3.xxxx, -r2   (R)
        0x03000042, 0x800F0004, 0x80E40004, 0xA0E40803,   // texld r4, r4, s3       (cube)
        0x03000005, 0x800F0000, 0x80E40004, 0x90E40000,   // mul r0, r4, v0
        0x02000001, 0x800F0800, 0x80E40000,               // mov oC0, r0
        0x0000FFFF };

    struct KnownShader { const char* name; const DWORD* orig; size_t origLen; const DWORD* repl; };
    const KnownShader kKnown[] = {
        { "palette lookup (texreg2gb)",    kOrigPalette, std::size(kOrigPalette), kNewPalette },
        { "bump env map (texbem)",         kOrigBumpEnv, std::size(kOrigBumpEnv), kNewBumpEnv },
        { "env reflection (texm3x3vspec)", kOrigEnvSpec, std::size(kOrigEnvSpec), kNewEnvSpec },
    };

    // Replaced shader objects (render thread only; a handful at most).
    struct Replaced { IDirect3DPixelShader9* shader; int kind; };
    Replaced g_replaced[32] = {};
    int g_replacedCount = 0;
    bool g_boundLogged[3] = {};
    unsigned g_bindCount[3] = {};

    float g_bumpMat[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }; // c30 = (M00, M01, 0, 0), c31 = (M10, M11, 0, 0)
    float g_gameConst[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }; // the game's own values of c30/c31
    bool g_bumpActive = false;
    bool g_constOurs = false;
}

const DWORD* AmdFix::Replacement(const DWORD* fn, int* kind)
{
    if (!g_settings.amdThermalFix || !fn || fn[0] != 0xFFFF0101)
        return nullptr;
    for (int i = 0; i < 3; ++i)
    {
        if (!(g_settings.shaderReplaceMask & (1 << i)))
            continue;
        const auto& k = kKnown[i];
        if (std::memcmp(fn, k.orig, k.origLen * sizeof(DWORD)) == 0)
        {
            *kind = i;
            return k.repl;
        }
    }
    return nullptr;
}

void AmdFix::OnCreated(IDirect3DPixelShader9* shader, int kind)
{
    // The game creates its shader set once per device (again after a device reset), so keep
    // the most recent entries; stale pointers are only ever compared, never dereferenced.
    g_replaced[g_replacedCount % 32] = { shader, kind };
    ++g_replacedCount;
    LOG("AMDFix: replaced legacy shader '%s' with ps_2_0 version (%p)", kKnown[kind].name, shader);
}

void AmdFix::OnSetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* shader)
{
    if (!g_settings.amdThermalFix || g_replacedCount == 0)
        return;
    int kind = -1;
    const int n = std::min(g_replacedCount, 32);
    for (int j = 0; j < n && shader && kind < 0; ++j)
    {
        const int i = (g_replacedCount - 1 - j) % 32; // newest first
        if (g_replaced[i].shader != shader)
            continue;
        // A released shader's address can be reused by another one: confirm by bytecode.
        const int k = g_replaced[i].kind;
        const DWORD* repl = kKnown[k].repl;
        size_t replLen = 1;
        while (repl[replLen - 1] != 0x0000FFFF) ++replLen;
        DWORD code[128];
        UINT size = sizeof(code);
        if (SUCCEEDED(shader->GetFunction(code, &size)) && size == replLen * sizeof(DWORD) && std::memcmp(code, repl, size) == 0)
            kind = k;
    }
    if (kind >= 0)
    {
        ++g_bindCount[kind];
        if (!g_boundLogged[kind])
        {
            g_boundLogged[kind] = true;
            LOG("AMDFix: '%s' used for the first time this session", kKnown[kind].name);
        }
    }

    const bool bump = (kind == 1);
    if (bump)
    {
        oSetPixelShaderConstantF(dev, kBumpConst, g_bumpMat, 2);
        g_constOurs = true;
    }
    else if (g_constOurs)
    {
        oSetPixelShaderConstantF(dev, kBumpConst, g_gameConst, 2); // give the game its constants back
        g_constOurs = false;
    }
    g_bumpActive = bump;
}

void AmdFix::OnSetTextureStageState(IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (stage != 3 || type < D3DTSS_BUMPENVMAT00 || type > D3DTSS_BUMPENVMAT11)
        return;
    float f;
    std::memcpy(&f, &value, 4);
    static const int slot[4] = { 0, 1, 4, 5 }; // 00 -> c30.x, 01 -> c30.y, 10 -> c31.x, 11 -> c31.y
    g_bumpMat[slot[type - D3DTSS_BUMPENVMAT00]] = f;
    if (g_bumpActive && oSetPixelShaderConstantF)
        oSetPixelShaderConstantF(dev, kBumpConst, g_bumpMat, 2);
}

void AmdFix::OnSetPixelShaderConstantF(UINT start, const float* data, UINT count)
{
    if (!data)
        return;
    for (UINT reg = kBumpConst; reg < kBumpConst + 2; ++reg)
    {
        if (reg >= start && reg < start + count)
        {
            std::memcpy(&g_gameConst[(reg - kBumpConst) * 4], data + (reg - start) * 4, 4 * sizeof(float));
            if (g_bumpActive)
                g_constOurs = false; // the game wrote it while our shader is bound; it owns it now
        }
    }
}
