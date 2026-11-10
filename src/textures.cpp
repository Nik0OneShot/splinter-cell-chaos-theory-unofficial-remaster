// Texture packs (mod support, phase 2): dumping the game's textures and replacing them from mods.
//
// A texture is identified by a 64-bit fingerprint of its top mip level as the game uploads it (pixels, size and
// format), the way emulator texture packs work, so it works for every texture in the game without unpacking
// dynamic-pc.umd. Dumps go to Mods\_TextureDump\ as <EngineName>__<fingerprint>.png (or .dds); the engine name is
// found by looking for the UTexture being uploaded on the stack (Scripting::TextureName) and is only there to help
// people find things. A replacement is any image in a mod's Textures\ folder (subfolders too) whose file name ends
// in the 16-digit fingerprint: .png, .tga, .jpg, .bmp or .dds, any resolution. Images are compressed to DXT when the
// original was DXT (stb_dxt) and get a full mip chain; DDS files are used as they are.
//
// How: the renderer fills textures in two ways: mostly IDirect3DTexture9::LockRect/UnlockRect on each mip level (vtable
// +4Ch/+50h, e.g. the mip loop at 0x10C8CDF4, which passes the full rectangle), and for a few by taking a level as a
// surface (GetSurfaceLevel, 0x10C86239) and locking that (IDirect3DSurface9 +34h/+38h). Both are hooked (vtables are
// shared by every texture / surface); while a texture-level lock runs, the surface hooks stand aside (DXVK may lock
// the level's surface inside it). A full lock of a texture's top level is recorded, and at its unlock the pixels are
// fingerprinted (and copied for a dump) before the real unlock. A texture with a replacement is
// swapped for it in SetTexture; UpdateTexture passes the fingerprint on from a staging texture to its target; Release
// drops the swap when the game frees the original (the replacement is freed with its last user). Textures whose top
// level is locked more than 3 times are treated as dynamic and left alone. Ctrl + DumpTexturesKey toggles dumping.
#include "pch.h"
#include "features.h"

#include <intrin.h>
#include <map>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

namespace
{
    constexpr D3DFORMAT kDXT1 = static_cast<D3DFORMAT>(MAKEFOURCC('D', 'X', 'T', '1'));
    constexpr D3DFORMAT kDXT3 = static_cast<D3DFORMAT>(MAKEFOURCC('D', 'X', 'T', '3'));
    constexpr D3DFORMAT kDXT5 = static_cast<D3DFORMAT>(MAKEFOURCC('D', 'X', 'T', '5'));

    using Lock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
    using Unlock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*);
    using Release_t = ULONG(STDMETHODCALLTYPE*)(IDirect3DTexture9*);
    using TexLock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
    using TexUnlock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT);
    Lock_t oLock = nullptr;
    Unlock_t oUnlock = nullptr;
    TexLock_t oTexLock = nullptr;
    TexUnlock_t oTexUnlock = nullptr;
    Release_t oRelease = nullptr;
    thread_local int t_inner = 0;   // inside a texture-level lock/unlock: the surface hooks stand aside

    bool g_active = false;          // hooks wanted at all
    bool g_dump = false;            // dumping right now
    bool g_keyHeld = false;
    thread_local int t_own = 0;     // creating/filling our own textures: leave them alone

    struct LockRec { IDirect3DTexture9* tex = nullptr; void* bits = nullptr; INT pitch = 0; bool partial = false; int count = 0; };
    struct Repl { IDirect3DTexture9* tex = nullptr; int users = 0; };

    std::recursive_mutex g_lock;
    std::unordered_map<IDirect3DSurface9*, LockRec> g_locks;     // top-level surfaces of game textures: their locks
    std::unordered_map<IDirect3DTexture9*, IDirect3DSurface9*> g_topSurface;   // game texture -> its top-level surface
    std::unordered_map<IDirect3DTexture9*, LockRec> g_texLocks;  // game textures: texture-level locks of their top level
    std::unordered_map<IDirect3DTexture9*, uint64_t> g_hashes;   // game texture -> fingerprint of its level 0
    std::unordered_map<IDirect3DTexture9*, uint64_t> g_swaps;    // game texture -> fingerprint of its replacement
    std::unordered_map<uint64_t, Repl> g_repls;                  // fingerprint -> loaded replacement
    std::unordered_map<uint64_t, std::wstring> g_files;          // fingerprint -> replacement file (from the mods)
    std::unordered_set<uint64_t> g_failed, g_dumped;
    std::atomic<int> g_swapCount{ 0 };
    std::wstring g_dumpDir;
    int g_dumpCount = 0, g_dumpLogged = 0, g_replaced = 0, g_uploads = 0, g_uploadsLogged = 0, g_renamed = 0;
    DWORD g_lastLog = 0;

    // --- Formats ---------------------------------------------------------------------------------------

    bool IsDXT(D3DFORMAT f) { return f == kDXT1 || f == kDXT3 || f == kDXT5; }
    int Bpp(D3DFORMAT f)   // bytes per pixel of uncompressed formats, 0 = unknown
    {
        switch (f)
        {
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: return 4;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8:
        case D3DFMT_V8U8: case D3DFMT_L16: return 2;
        case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: return 1;
        default: return 0;
        }
    }
    // Bytes in one row of blocks (DXT) or pixels, and the number of such rows.
    bool RowLayout(D3DFORMAT f, UINT w, UINT h, UINT& rowBytes, UINT& rows)
    {
        if (IsDXT(f)) { rowBytes = std::max(1u, (w + 3) / 4) * (f == kDXT1 ? 8u : 16u); rows = std::max(1u, (h + 3) / 4); return true; }
        const int b = Bpp(f);
        if (!b) return false;
        rowBytes = w * UINT(b);
        rows = h;
        return true;
    }
    const char* FormatTag(D3DFORMAT f)
    {
        switch (f)
        {
        case D3DFMT_A8R8G8B8: return "ARGB8"; case D3DFMT_X8R8G8B8: return "XRGB8"; case D3DFMT_R5G6B5: return "RGB565";
        case D3DFMT_A1R5G5B5: return "ARGB1555"; case D3DFMT_X1R5G5B5: return "XRGB1555"; case D3DFMT_A4R4G4B4: return "ARGB4";
        case D3DFMT_A8: return "A8"; case D3DFMT_L8: return "L8"; case D3DFMT_A8L8: return "A8L8"; case D3DFMT_V8U8: return "V8U8";
        case D3DFMT_P8: return "P8";
        default:
            if (f == kDXT1) return "DXT1";
            if (f == kDXT3) return "DXT3";
            if (f == kDXT5) return "DXT5";
            return "?";
        }
    }

    uint64_t Fingerprint(const uint8_t* bits, INT pitch, UINT rowBytes, UINT rows, UINT w, UINT h, D3DFORMAT f)
    {
        uint64_t hash = 0x9E3779B97F4A7C15ull ^ (uint64_t(w) << 40) ^ (uint64_t(h) << 20) ^ uint64_t(uint32_t(f));
        auto mix = [&](uint64_t v) { hash ^= v + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2); hash *= 0xFF51AFD7ED558CCDull; hash ^= hash >> 33; };
        for (UINT y = 0; y < rows; ++y)
        {
            const uint8_t* row = bits + size_t(y) * size_t(pitch);
            UINT x = 0;
            for (; x + 8 <= rowBytes; x += 8) { uint64_t v; memcpy(&v, row + x, 8); mix(v); }
            if (x < rowBytes) { uint64_t v = 0; memcpy(&v, row + x, rowBytes - x); mix(v); }
        }
        return hash;
    }

    // --- Decoding for PNG dumps --------------------------------------------------------------------------

    void Rgb565(uint16_t c, uint8_t* o) { o[0] = uint8_t(((c >> 11) & 31) * 255 / 31); o[1] = uint8_t(((c >> 5) & 63) * 255 / 63); o[2] = uint8_t((c & 31) * 255 / 31); }

    void DecodeColorBlock(const uint8_t* b, uint8_t* out /*16 RGBA*/, bool dxt1)
    {
        uint16_t c0, c1;
        memcpy(&c0, b, 2);
        memcpy(&c1, b + 2, 2);
        uint8_t pal[4][4];
        Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
        pal[0][3] = pal[1][3] = 255;
        if (!dxt1 || c0 > c1)
        {
            for (int i = 0; i < 3; ++i) { pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3); pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3); }
            pal[2][3] = pal[3][3] = 255;
        }
        else
        {
            for (int i = 0; i < 3; ++i) { pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2); pal[3][i] = 0; }
            pal[2][3] = 255;
            pal[3][3] = 0;
        }
        uint32_t idx;
        memcpy(&idx, b + 4, 4);
        for (int p = 0; p < 16; ++p) memcpy(out + p * 4, pal[(idx >> (2 * p)) & 3], 4);
    }

    // Converts level-0 data to RGBA8; false for formats a PNG can't hold.
    bool ToRGBA(const std::vector<uint8_t>& data, UINT rowBytes, UINT w, UINT h, D3DFORMAT f, std::vector<uint8_t>& rgba)
    {
        rgba.assign(size_t(w) * h * 4, 255);
        auto px = [&](UINT x, UINT y) { return rgba.data() + (size_t(y) * w + x) * 4; };
        if (IsDXT(f))
        {
            const UINT bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4), bs = f == kDXT1 ? 8 : 16;
            uint8_t block[64];
            for (UINT by = 0; by < bh; ++by)
                for (UINT bx = 0; bx < bw; ++bx)
                {
                    const uint8_t* b = data.data() + size_t(by) * rowBytes + size_t(bx) * bs;
                    DecodeColorBlock(f == kDXT1 ? b : b + 8, block, f == kDXT1);
                    if (f == kDXT3)
                        for (int p = 0; p < 16; ++p) block[p * 4 + 3] = uint8_t(((b[p / 2] >> ((p & 1) * 4)) & 15) * 17);
                    else if (f == kDXT5)
                    {
                        uint8_t a[8] = { b[0], b[1] };
                        if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
                        else { for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
                        uint64_t bitsA = 0;
                        memcpy(&bitsA, b + 2, 6);
                        for (int p = 0; p < 16; ++p) block[p * 4 + 3] = a[(bitsA >> (3 * p)) & 7];
                    }
                    for (int p = 0; p < 16; ++p)
                    {
                        const UINT x = bx * 4 + p % 4, y = by * 4 + p / 4;
                        if (x < w && y < h) memcpy(px(x, y), block + p * 4, 4);
                    }
                }
            return true;
        }
        for (UINT y = 0; y < h; ++y)
        {
            const uint8_t* row = data.data() + size_t(y) * rowBytes;
            for (UINT x = 0; x < w; ++x)
            {
                uint8_t* o = px(x, y);
                switch (f)
                {
                case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
                    o[0] = row[x * 4 + 2]; o[1] = row[x * 4 + 1]; o[2] = row[x * 4]; o[3] = f == D3DFMT_A8R8G8B8 ? row[x * 4 + 3] : 255; break;
                case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
                    memcpy(o, row + x * 4, 4); if (f == D3DFMT_X8B8G8R8) o[3] = 255; break;
                case D3DFMT_R5G6B5: { uint16_t c; memcpy(&c, row + x * 2, 2); Rgb565(c, o); break; }
                case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5:
                { uint16_t c; memcpy(&c, row + x * 2, 2); o[0] = uint8_t(((c >> 10) & 31) * 255 / 31); o[1] = uint8_t(((c >> 5) & 31) * 255 / 31);
                  o[2] = uint8_t((c & 31) * 255 / 31); o[3] = (f == D3DFMT_X1R5G5B5 || (c & 0x8000)) ? 255 : 0; break; }
                case D3DFMT_A4R4G4B4:
                { uint16_t c; memcpy(&c, row + x * 2, 2); o[0] = uint8_t(((c >> 8) & 15) * 17); o[1] = uint8_t(((c >> 4) & 15) * 17);
                  o[2] = uint8_t((c & 15) * 17); o[3] = uint8_t(((c >> 12) & 15) * 17); break; }
                case D3DFMT_L8: o[0] = o[1] = o[2] = row[x]; break;
                case D3DFMT_A8: o[0] = o[1] = o[2] = 255; o[3] = row[x]; break;
                case D3DFMT_A8L8: o[0] = o[1] = o[2] = row[x * 2]; o[3] = row[x * 2 + 1]; break;
                default: return false;
                }
            }
        }
        return true;
    }

    // --- DDS -----------------------------------------------------------------------------------------------

#pragma pack(push, 1)
    struct DDSPixelFormat { uint32_t size, flags, fourCC, bits, r, g, b, a; };
    struct DDSHeader
    {
        uint32_t size, flags, height, width, pitchOrLinear, depth, mipCount, reserved[11];
        DDSPixelFormat pf;
        uint32_t caps, caps2, caps3, caps4, reserved2;
    };
#pragma pack(pop)

    bool PixelFormatFor(D3DFORMAT f, DDSPixelFormat& pf)
    {
        pf = { 32, 0, 0, 0, 0, 0, 0, 0 };
        if (IsDXT(f)) { pf.flags = 0x4; pf.fourCC = uint32_t(f); return true; }
        switch (f)
        {
        case D3DFMT_A8R8G8B8: pf = { 32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000 }; return true;
        case D3DFMT_X8R8G8B8: pf = { 32, 0x40, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0 }; return true;
        case D3DFMT_A8B8G8R8: pf = { 32, 0x41, 0, 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000 }; return true;
        case D3DFMT_R5G6B5: pf = { 32, 0x40, 0, 16, 0xF800, 0x7E0, 0x1F, 0 }; return true;
        case D3DFMT_A1R5G5B5: pf = { 32, 0x41, 0, 16, 0x7C00, 0x3E0, 0x1F, 0x8000 }; return true;
        case D3DFMT_X1R5G5B5: pf = { 32, 0x40, 0, 16, 0x7C00, 0x3E0, 0x1F, 0 }; return true;
        case D3DFMT_A4R4G4B4: pf = { 32, 0x41, 0, 16, 0xF00, 0xF0, 0xF, 0xF000 }; return true;
        case D3DFMT_L8: pf = { 32, 0x20000, 0, 8, 0xFF, 0, 0, 0 }; return true;
        case D3DFMT_A8: pf = { 32, 0x2, 0, 8, 0, 0, 0, 0xFF }; return true;
        case D3DFMT_A8L8: pf = { 32, 0x20001, 0, 16, 0xFF, 0, 0, 0xFF00 }; return true;
        default: return false;
        }
    }

    D3DFORMAT FormatFromDDS(const DDSPixelFormat& pf)
    {
        if (pf.flags & 0x4)
        {
            const D3DFORMAT f = static_cast<D3DFORMAT>(pf.fourCC);
            return IsDXT(f) ? f : D3DFMT_UNKNOWN;
        }
        const bool alpha = (pf.flags & 0x1) != 0;
        if (pf.bits == 32 && pf.r == 0xFF0000 && pf.g == 0xFF00 && pf.b == 0xFF) return alpha && pf.a ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
        if (pf.bits == 32 && pf.r == 0xFF && pf.g == 0xFF00 && pf.b == 0xFF0000) return alpha && pf.a ? D3DFMT_A8B8G8R8 : D3DFMT_X8B8G8R8;
        if (pf.bits == 16 && pf.r == 0xF800 && pf.g == 0x7E0 && pf.b == 0x1F) return D3DFMT_R5G6B5;
        if (pf.bits == 16 && pf.r == 0x7C00 && pf.g == 0x3E0 && pf.b == 0x1F) return alpha && pf.a ? D3DFMT_A1R5G5B5 : D3DFMT_X1R5G5B5;
        if (pf.bits == 16 && pf.r == 0xF00 && pf.g == 0xF0 && pf.b == 0xF) return D3DFMT_A4R4G4B4;
        if (pf.bits == 16 && pf.r == 0xFF && pf.a == 0xFF00) return D3DFMT_A8L8;
        if (pf.bits == 8 && pf.r == 0xFF) return D3DFMT_L8;
        if (pf.bits == 8 && pf.a == 0xFF) return D3DFMT_A8;
        return D3DFMT_UNKNOWN;
    }

    // --- Files ---------------------------------------------------------------------------------------------

    bool ReadFile(const std::wstring& path, std::vector<uint8_t>& out)
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0 || n > 512L * 1024 * 1024) { fclose(f); return false; }
        out.resize(size_t(n));
        const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        return ok;
    }
    bool WriteFile(const std::wstring& path, const void* data, size_t n)
    {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        const bool ok = fwrite(data, 1, n, f) == n;
        fclose(f);
        return ok;
    }

    std::wstring Hex(uint64_t h) { wchar_t b[17]; swprintf(b, 17, L"%016llx", static_cast<unsigned long long>(h)); return b; }

    // The fingerprint at the end of a file name ("anything_0123456789abcdef.png"), or 0.
    uint64_t HashFromName(const std::wstring& file)
    {
        std::wstring stem = file;
        const size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem.resize(dot);
        if (stem.size() < 16) return 0;
        const std::wstring tail = stem.substr(stem.size() - 16);
        if (stem.size() > 16 && iswalnum(stem[stem.size() - 17])) return 0;
        uint64_t v = 0;
        for (wchar_t c : tail)
        {
            int d;
            if (c >= L'0' && c <= L'9') d = c - L'0';
            else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
            else return 0;
            v = (v << 4) | uint64_t(d);
        }
        return v;
    }

    void ScanDir(const std::wstring& dir, int& count)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ScanDir(dir + name + L"\\", count); continue; }
            std::wstring ext = name.substr(name.find_last_of(L'.') == std::wstring::npos ? name.size() : name.find_last_of(L'.'));
            for (auto& c : ext) c = towlower(c);
            if (ext != L".png" && ext != L".tga" && ext != L".jpg" && ext != L".jpeg" && ext != L".bmp" && ext != L".dds") continue;
            if (const uint64_t hash = HashFromName(name)) { g_files[hash] = dir + name; ++count; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // --- Replacements -----------------------------------------------------------------------------------

    IDirect3DTexture9* Create(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, D3DFORMAT f)
    {
        IDirect3DTexture9* t = nullptr;
        ++t_own;
        const HRESULT hr = dev->CreateTexture(w, h, levels, 0, f, D3DPOOL_MANAGED, &t, nullptr);
        --t_own;
        return SUCCEEDED(hr) ? t : nullptr;
    }

    bool Fill(IDirect3DTexture9* t, UINT level, const uint8_t* src, UINT rowBytes, UINT rows)
    {
        D3DLOCKED_RECT lr;
        ++t_own;
        bool ok = SUCCEEDED(t->LockRect(level, &lr, nullptr, 0));
        if (ok)
        {
            for (UINT y = 0; y < rows; ++y) memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, src + size_t(y) * rowBytes, rowBytes);
            t->UnlockRect(level);
        }
        --t_own;
        return ok;
    }

    IDirect3DTexture9* LoadDDS(IDirect3DDevice9* dev, const std::vector<uint8_t>& file, UINT& w, UINT& h, D3DFORMAT& f)
    {
        if (file.size() < 4 + sizeof(DDSHeader) || memcmp(file.data(), "DDS ", 4) != 0) return nullptr;
        DDSHeader hd;
        memcpy(&hd, file.data() + 4, sizeof(hd));
        if (hd.pf.flags & 0x4 && hd.pf.fourCC == MAKEFOURCC('D', 'X', '1', '0')) return nullptr;   // DX10 header: not D3D9
        f = FormatFromDDS(hd.pf);
        w = hd.width;
        h = hd.height;
        if (f == D3DFMT_UNKNOWN || !w || !h || w > 16384 || h > 16384) return nullptr;
        UINT levels = (hd.flags & 0x20000) && hd.mipCount ? hd.mipCount : 1;
        IDirect3DTexture9* t = Create(dev, w, h, levels, f);
        if (!t) return nullptr;
        size_t at = 4 + sizeof(DDSHeader);
        levels = t->GetLevelCount();
        for (UINT l = 0; l < levels; ++l)
        {
            const UINT lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
            UINT rowBytes, rows;
            RowLayout(f, lw, lh, rowBytes, rows);
            if (at + size_t(rowBytes) * rows > file.size()) break;
            Fill(t, l, file.data() + at, rowBytes, rows);
            at += size_t(rowBytes) * rows;
        }
        return t;
    }

    // Box-filters an RGBA8 image to half size (at least 1x1).
    std::vector<uint8_t> Half(const std::vector<uint8_t>& src, UINT w, UINT h, UINT& nw, UINT& nh)
    {
        nw = std::max(1u, w / 2);
        nh = std::max(1u, h / 2);
        std::vector<uint8_t> out(size_t(nw) * nh * 4);
        for (UINT y = 0; y < nh; ++y)
            for (UINT x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    const UINT x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1), y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                    const int s = src[(size_t(y0) * w + x0) * 4 + c] + src[(size_t(y0) * w + x1) * 4 + c] + src[(size_t(y1) * w + x0) * 4 + c] + src[(size_t(y1) * w + x1) * 4 + c];
                    out[(size_t(y) * nw + x) * 4 + c] = uint8_t((s + 2) / 4);
                }
        return out;
    }

    IDirect3DTexture9* LoadImage(IDirect3DDevice9* dev, const std::vector<uint8_t>& file, D3DFORMAT original, UINT& w, UINT& h, D3DFORMAT& f)
    {
        int iw = 0, ih = 0, comp = 0;
        stbi_uc* px = stbi_load_from_memory(file.data(), int(file.size()), &iw, &ih, &comp, 4);
        if (!px) return nullptr;
        std::vector<uint8_t> img(px, px + size_t(iw) * ih * 4);
        stbi_image_free(px);
        w = UINT(iw);
        h = UINT(ih);
        if (!w || !h || w > 16384 || h > 16384) return nullptr;
        bool anyAlpha = false;
        for (size_t i = 3; i < img.size() && !anyAlpha; i += 4) anyAlpha = img[i] < 255;
        const bool dxt = IsDXT(original) && w % 4 == 0 && h % 4 == 0;
        f = dxt ? (original == kDXT1 && !anyAlpha ? kDXT1 : kDXT5) : D3DFMT_A8R8G8B8;
        UINT levels = 1;
        for (UINT lw = w, lh = h; lw > 1 || lh > 1; lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2)) ++levels;
        IDirect3DTexture9* t = Create(dev, w, h, levels, f);
        if (!t) return nullptr;
        UINT lw = w, lh = h;
        for (UINT l = 0; l < levels; ++l)
        {
            UINT rowBytes, rows;
            RowLayout(f, lw, lh, rowBytes, rows);
            std::vector<uint8_t> data(size_t(rowBytes) * rows);
            if (dxt)
            {
                const bool alpha = f == kDXT5;
                uint8_t block[64];
                for (UINT by = 0; by < rows; ++by)
                    for (UINT bx = 0; bx < rowBytes / (alpha ? 16 : 8); ++bx)
                    {
                        for (int p = 0; p < 16; ++p)
                        {
                            const UINT x = std::min(bx * 4 + p % 4, lw - 1), y = std::min(by * 4 + p / 4, lh - 1);
                            memcpy(block + p * 4, img.data() + (size_t(y) * lw + x) * 4, 4);
                        }
                        stb_compress_dxt_block(data.data() + size_t(by) * rowBytes + size_t(bx) * (alpha ? 16 : 8), block, alpha ? 1 : 0, STB_DXT_HIGHQUAL);
                    }
            }
            else
                for (size_t i = 0; i < size_t(lw) * lh; ++i)
                {
                    data[i * 4] = img[i * 4 + 2]; data[i * 4 + 1] = img[i * 4 + 1]; data[i * 4 + 2] = img[i * 4]; data[i * 4 + 3] = img[i * 4 + 3];
                }
            Fill(t, l, data.data(), rowBytes, rows);
            if (l + 1 < levels) { UINT nw, nh; img = Half(img, lw, lh, nw, nh); lw = nw; lh = nh; }
        }
        return t;
    }

    // The replacement for 'hash' (loaded on first use), or null. Caller holds g_lock.
    IDirect3DTexture9* Replacement(IDirect3DTexture9* original, uint64_t hash, UINT ow, UINT oh, D3DFORMAT of)
    {
        auto it = g_repls.find(hash);
        if (it != g_repls.end()) return it->second.tex;
        auto file = g_files.find(hash);
        if (file == g_files.end() || g_failed.count(hash)) return nullptr;
        std::vector<uint8_t> data;
        IDirect3DTexture9* t = nullptr;
        UINT w = 0, h = 0;
        D3DFORMAT f = D3DFMT_UNKNOWN;
        IDirect3DDevice9* dev = nullptr;
        if (ReadFile(file->second, data) && SUCCEEDED(original->GetDevice(&dev)) && dev)
        {
            t = data.size() >= 4 && memcmp(data.data(), "DDS ", 4) == 0 ? LoadDDS(dev, data, w, h, f) : LoadImage(dev, data, of, w, h, f);
            dev->Release();
        }
        if (!t)
        {
            g_failed.insert(hash);
            LOG("[TEX] couldn't load %ls (unsupported or broken file)", file->second.c_str());
            return nullptr;
        }
        g_repls[hash] = Repl{ t, 0 };
        ++g_replaced;
        LOG("[TEX] %016llx (%ux%u %s) -> %ls (%ux%u %s)", static_cast<unsigned long long>(hash), ow, oh, FormatTag(of),
            file->second.c_str(), w, h, FormatTag(f));
        return t;
    }

    void Unswap(IDirect3DTexture9* tex)   // caller holds g_lock
    {
        auto it = g_swaps.find(tex);
        if (it == g_swaps.end()) return;
        auto r = g_repls.find(it->second);
        g_swaps.erase(it);
        g_swapCount.store(int(g_swaps.size()));
        if (r != g_repls.end() && --r->second.users <= 0)
        {
            IDirect3DTexture9* t = r->second.tex;
            g_repls.erase(r);
            ++t_own;
            t->Release();
            --t_own;
        }
    }

    void Assign(IDirect3DTexture9* tex, uint64_t hash, UINT w, UINT h, D3DFORMAT f)   // caller holds g_lock
    {
        g_hashes[tex] = hash;
        auto cur = g_swaps.find(tex);
        if (cur != g_swaps.end() && cur->second == hash) return;
        Unswap(tex);
        if (g_files.empty()) return;
        if (Replacement(tex, hash, w, h, f))
        {
            ++g_repls[hash].users;
            g_swaps[tex] = hash;
            g_swapCount.store(int(g_swaps.size()));
        }
    }

    // --- Dumping -----------------------------------------------------------------------------------------

    std::wstring SafeName(const std::string& s)
    {
        std::wstring w;
        for (char c : s) w += (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? wchar_t(c) : L'_';
        return w.substr(0, 120);
    }

    void Wrote(const std::wstring& path, bool ok)
    {
        if (ok)
        {
            if (++g_dumpCount == 1) LOG("[TEX] first texture dumped: %ls", path.c_str());
            return;
        }
        static int failures = 0;
        if (++failures <= 5) LOG("[TEX] [!] couldn't write %ls (error %lu)", path.c_str(), GetLastError());
    }

    // Textures already in the dump folder from earlier sessions: fingerprint -> file name.
    std::unordered_map<uint64_t, std::wstring> g_onDisk;
    bool g_onDiskScanned = false;

    void Dump(uint64_t hash, const std::vector<uint8_t>& data, UINT rowBytes, UINT rows, UINT w, UINT h, D3DFORMAT f, const std::string& engineName)
    {
        if (!g_dumped.insert(hash).second) return;
        if (!g_onDiskScanned)
        {
            g_onDiskScanned = true;
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileW((g_dumpDir + L"*").c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE)
            {
                do
                    if (const uint64_t hh = HashFromName(fd.cFileName)) g_onDisk[hh] = fd.cFileName;
                while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
        }
        // Already dumped before: keep it, but give a fingerprint-only file the name found now.
        if (auto old = g_onDisk.find(hash); old != g_onDisk.end())
        {
            if (!engineName.empty() && old->second.find(L"__") == std::wstring::npos)
            {
                const size_t dot = old->second.find_last_of(L'.');
                const std::wstring renamed = SafeName(engineName) + L"__" + old->second;
                if (dot != std::wstring::npos && MoveFileW((g_dumpDir + old->second).c_str(), (g_dumpDir + renamed).c_str()))
                {
                    old->second = renamed;
                    ++g_renamed;
                }
            }
            return;
        }
        if (!CreateDirectoryW(g_dumpDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            static bool noted = false;
            if (!noted) { noted = true; LOG("[TEX] [!] couldn't create %ls (error %lu)", g_dumpDir.c_str(), GetLastError()); }
        }
        const std::wstring base = g_dumpDir + (engineName.empty() ? L"" : SafeName(engineName) + L"__") + Hex(hash);
        std::vector<uint8_t> rgba;
        if (!g_settings.dumpTexturesDDS && ToRGBA(data, rowBytes, w, h, f, rgba))
        {
            std::vector<uint8_t> png;
            stbi_write_png_to_func([](void* ctx, void* d, int n) {
                auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                v->insert(v->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n);
            }, &png, int(w), int(h), 4, rgba.data(), int(w) * 4);
            Wrote(base + L".png", !png.empty() && WriteFile(base + L".png", png.data(), png.size()));
            return;
        }
        DDSPixelFormat pf;
        if (!PixelFormatFor(f, pf))
        {
            static int skipped = 0;
            if (++skipped <= 5) LOG("[TEX] texture %016llx has format %s (%d), which can't be saved", static_cast<unsigned long long>(hash), FormatTag(f), int(f));
            return;
        }
        DDSHeader hd{};
        hd.size = 124;
        hd.flags = 0x1 | 0x2 | 0x4 | 0x1000 | (IsDXT(f) ? 0x80000 : 0x8);
        hd.height = h;
        hd.width = w;
        hd.pitchOrLinear = IsDXT(f) ? rowBytes * rows : rowBytes;
        hd.mipCount = 1;
        hd.pf = pf;
        hd.caps = 0x1000;
        std::vector<uint8_t> out(4 + sizeof(hd));
        memcpy(out.data(), "DDS ", 4);
        memcpy(out.data() + 4, &hd, sizeof(hd));
        out.insert(out.end(), data.begin(), data.begin() + size_t(rowBytes) * rows);
        Wrote(base + L".dds", WriteFile(base + L".dds", out.data(), out.size()));
    }

    // --- Hooks ---------------------------------------------------------------------------------------------


    bool FullRect(const RECT* r, UINT w, UINT h)
    {
        return !r || (r->left <= 0 && r->top <= 0 && r->right >= LONG(w) && r->bottom >= LONG(h));
    }

    void Record(LockRec& r, IDirect3DTexture9* tex, const D3DLOCKED_RECT* lr, const RECT* rect)
    {
        D3DSURFACE_DESC td;
        r.tex = tex;
        r.bits = lr->pBits;
        r.pitch = lr->Pitch;
        r.partial = !(SUCCEEDED(tex->GetLevelDesc(0, &td)) && FullRect(rect, td.Width, td.Height));
        ++r.count;
    }

    // Before the real unlock: fingerprint (and maybe copy) the locked top level. Caller holds g_lock.
    struct Capture { IDirect3DTexture9* tex = nullptr; uint64_t hash = 0; UINT w = 0, h = 0, rowBytes = 0, rows = 0;
                     D3DFORMAT f = D3DFMT_UNKNOWN; std::vector<uint8_t> copy; std::string name; bool have = false, dynamic = false; };
    void Take(LockRec& r, Capture& c)
    {
        c.tex = r.tex;
        c.dynamic = r.count > 3;
        D3DSURFACE_DESC desc;
        if (r.bits && !r.partial && !c.dynamic && SUCCEEDED(r.tex->GetLevelDesc(0, &desc)) &&
            RowLayout(desc.Format, desc.Width, desc.Height, c.rowBytes, c.rows))
        {
            c.w = desc.Width; c.h = desc.Height; c.f = desc.Format;
            const uint8_t* bits = static_cast<const uint8_t*>(r.bits);
            c.hash = Fingerprint(bits, r.pitch, c.rowBytes, c.rows, c.w, c.h, c.f);
            c.have = true;
            if (g_dump && !g_dumped.count(c.hash))
            {
                c.copy.resize(size_t(c.rowBytes) * c.rows);
                for (UINT y = 0; y < c.rows; ++y) memcpy(c.copy.data() + size_t(y) * c.rowBytes, bits + size_t(y) * r.pitch, c.rowBytes);
                Scripting::TextureName(_AddressOfReturnAddress(), 0x1000, c.w, c.h, c.name);
            }
        }
        r.bits = nullptr;
    }

    // After the real unlock.
    void Finish(Capture& c)
    {
        if (!c.tex) return;
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (c.have)
        {
            ++g_uploads;
            Assign(c.tex, c.hash, c.w, c.h, c.f);
            if (!c.copy.empty()) Dump(c.hash, c.copy, c.rowBytes, c.rows, c.w, c.h, c.f, c.name);
        }
        else if (c.dynamic)
        {
            Unswap(c.tex);
            g_hashes.erase(c.tex);
        }
    }

    // --- Texture-level locks ---

    HRESULT STDMETHODCALLTYPE hkTexLock(IDirect3DTexture9* tex, UINT level, D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags)
    {
        ++t_inner;
        const HRESULT hr = oTexLock(tex, level, lr, rect, flags);
        --t_inner;
        if (t_own || level != 0 || FAILED(hr) || !lr || (flags & D3DLOCK_READONLY)) return hr;
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        Record(g_texLocks[tex], tex, lr, rect);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkTexUnlock(IDirect3DTexture9* tex, UINT level)
    {
        Capture c;
        if (!t_own && level == 0)
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            auto it = g_texLocks.find(tex);
            if (it != g_texLocks.end() && it->second.bits) Take(it->second, c);
        }
        ++t_inner;
        const HRESULT hr = oTexUnlock(tex, level);
        --t_inner;
        Finish(c);
        return hr;
    }

    // --- Surface-level locks ---

    // The texture whose top mip level 's' is, or null (the caller gets no reference).
    IDirect3DTexture9* TopLevelOf(IDirect3DSurface9* surf)
    {
        IDirect3DTexture9* tex = nullptr;
        ++t_own;
        if (FAILED(surf->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) || !tex) { --t_own; return nullptr; }
        D3DSURFACE_DESC sd, td;
        const bool top = SUCCEEDED(surf->GetDesc(&sd)) && SUCCEEDED(tex->GetLevelDesc(0, &td)) && sd.Width == td.Width && sd.Height == td.Height;
        tex->Release();
        --t_own;
        return top ? tex : nullptr;
    }

    HRESULT STDMETHODCALLTYPE hkLock(IDirect3DSurface9* surf, D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags)
    {
        const HRESULT hr = oLock(surf, lr, rect, flags);
        if (t_own || t_inner || FAILED(hr) || !lr || (flags & D3DLOCK_READONLY)) return hr;
        IDirect3DTexture9* tex = TopLevelOf(surf);
        if (!tex) return hr;
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        Record(g_locks[surf], tex, lr, rect);
        g_topSurface[tex] = surf;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkUnlock(IDirect3DSurface9* surf)
    {
        if (t_own || t_inner) return oUnlock(surf);
        Capture c;
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            auto it = g_locks.find(surf);
            if (it != g_locks.end() && it->second.bits) Take(it->second, c);
        }
        const HRESULT hr = oUnlock(surf);
        Finish(c);
        return hr;
    }

    ULONG STDMETHODCALLTYPE hkRelease(IDirect3DTexture9* t)
    {
        const ULONG n = oRelease(t);
        if (n == 0 && !t_own)
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            Unswap(t);
            g_hashes.erase(t);
            auto top = g_topSurface.find(t);
            if (top != g_topSurface.end()) { g_locks.erase(top->second); g_topSurface.erase(top); }
            g_texLocks.erase(t);
        }
        return n;
    }
}

namespace
{
    // Saves every live game texture that has a fingerprint (read back from its top level); these have no engine name.
    void DumpLoaded()
    {
        std::vector<std::pair<IDirect3DTexture9*, uint64_t>> live;
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            for (const auto& [tex, hash] : g_hashes)
                if (!g_dumped.count(hash)) live.emplace_back(tex, hash);
        }
        int n = 0;
        for (const auto& [tex, hash] : live)
        {
            IDirect3DSurface9* surf = nullptr;
            D3DSURFACE_DESC desc;
            UINT rowBytes, rows;
            ++t_own;
            const bool ok = SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf && SUCCEEDED(surf->GetDesc(&desc)) &&
                            desc.Pool != D3DPOOL_DEFAULT && RowLayout(desc.Format, desc.Width, desc.Height, rowBytes, rows);
            std::vector<uint8_t> copy;
            if (ok)
            {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(surf->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
                {
                    copy.resize(size_t(rowBytes) * rows);
                    for (UINT y = 0; y < rows; ++y) memcpy(copy.data() + size_t(y) * rowBytes, static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, rowBytes);
                    surf->UnlockRect();
                }
            }
            if (surf) surf->Release();
            --t_own;
            if (copy.empty()) continue;
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            Dump(hash, copy, rowBytes, rows, desc.Width, desc.Height, desc.Format, "");
            ++n;
        }
        LOG("[TEX] dumped %d texture(s) that were already loaded", n);
    }
}

void Textures::Init()
{
    g_dump = g_settings.mods && g_settings.dumpTextures;
    if (!g_settings.mods) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\'));
    dir.resize(dir.find_last_of(L'\\') + 1);
    g_dumpDir = dir + L"Mods\\_TextureDump\\";
    int count = 0;
    if (g_settings.texturePacks)
        for (const auto& m : Mods::Loaded())   // lowest priority first: later mods win
        {
            const int before = count;
            ScanDir(m.dir + L"Textures\\", count);
            if (count > before) LOG("[TEX] %ls: %d replacement texture(s)", m.name.c_str(), count - before);
        }
    g_active = !g_files.empty() || g_dump || g_settings.dumpTexturesKey > 0 || g_settings.editorKey > 0;
    LOG("Textures: %u replacement(s)%s%s", unsigned(g_files.size()), g_dump ? ", dumping to Mods\\_TextureDump" : "",
        g_active ? "" : " (off)");
}

bool Textures::Active() { return g_active; }

bool Textures::Dumping() { return g_dump; }

void Textures::SetDumping(bool on)
{
    if (on == g_dump) return;
    g_dump = on;
    LOG("[TEX] texture dumping %s (to %ls)", g_dump ? "on" : "off", g_dumpDir.c_str());
    if (g_dump) DumpLoaded();
}

void Textures::OnCreateTexture(IDirect3DTexture9* tex)
{
    if (!g_active || !tex || t_own || oLock) return;
    IDirect3DSurface9* surf = nullptr;
    ++t_own;
    const HRESULT hr = tex->GetSurfaceLevel(0, &surf);
    --t_own;
    if (FAILED(hr) || !surf) { LOG("Textures: [!] couldn't get a texture surface; texture packs unavailable"); return; }
    void** svt = *reinterpret_cast<void***>(surf);
    oLock = reinterpret_cast<Lock_t>(Mem::PatchVTable(svt, 13, reinterpret_cast<void*>(&hkLock)));        // IDirect3DSurface9::LockRect
    oUnlock = reinterpret_cast<Unlock_t>(Mem::PatchVTable(svt, 14, reinterpret_cast<void*>(&hkUnlock)));  // IDirect3DSurface9::UnlockRect
    ++t_own;
    surf->Release();
    --t_own;
    void** vt = *reinterpret_cast<void***>(tex);
    oRelease = reinterpret_cast<Release_t>(Mem::PatchVTable(vt, 2, reinterpret_cast<void*>(&hkRelease)));       // IDirect3DTexture9::Release
    oTexLock = reinterpret_cast<TexLock_t>(Mem::PatchVTable(vt, 19, reinterpret_cast<void*>(&hkTexLock)));      // IDirect3DTexture9::LockRect
    oTexUnlock = reinterpret_cast<TexUnlock_t>(Mem::PatchVTable(vt, 20, reinterpret_cast<void*>(&hkTexUnlock))); // IDirect3DTexture9::UnlockRect
    LOG("Textures: surface and texture hooks installed");
}

IDirect3DBaseTexture9* Textures::Substitute(IDirect3DBaseTexture9* tex)
{
    if (!tex || g_swapCount.load(std::memory_order_relaxed) == 0) return tex;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_swaps.find(static_cast<IDirect3DTexture9*>(tex));
    if (it == g_swaps.end()) return tex;
    auto r = g_repls.find(it->second);
    return r != g_repls.end() ? r->second.tex : tex;
}

void Textures::OnUpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst)
{
    if (!g_active || !src || !dst || src == dst) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_hashes.find(static_cast<IDirect3DTexture9*>(src));
    if (it == g_hashes.end()) return;
    auto* d = static_cast<IDirect3DTexture9*>(dst);
    if (d->GetType() != D3DRTYPE_TEXTURE) return;
    D3DSURFACE_DESC desc;
    if (FAILED(d->GetLevelDesc(0, &desc))) return;
    Assign(d, it->second, desc.Width, desc.Height, desc.Format);
}

void Textures::OnPresent()
{
    if (!g_active) return;
    if (g_settings.dumpTexturesKey > 0 && g_settings.dumpTexturesKey < 256 && Game::hWnd && GetForegroundWindow() == Game::hWnd)
    {
        const bool down = (GetAsyncKeyState(g_settings.dumpTexturesKey) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        if (down && !g_keyHeld) SetDumping(!g_dump);
        g_keyHeld = down;
    }
    const DWORD now = GetTickCount();
    if ((g_dumpCount != g_dumpLogged || g_uploads != g_uploadsLogged) && now - g_lastLog > 3000)
    {
        g_lastLog = now;
        g_dumpLogged = g_dumpCount;
        g_uploadsLogged = g_uploads;
        LOG("[TEX] %d texture upload(s) seen, %d replaced, %d dumped so far (%d earlier dumps renamed)", g_uploads, int(g_swaps.size()),
            g_dumpCount, g_renamed);
    }
}
