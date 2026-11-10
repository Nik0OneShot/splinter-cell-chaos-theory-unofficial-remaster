#include "pch.h"
#include "../third_party/minhook/include/MinHook.h"

namespace
{
    Mem::Range g_text;
    uintptr_t g_base = 0;

    uint8_t* g_pool = nullptr;
    size_t g_poolUsed = 0;
    constexpr size_t kPoolSize = 64 * 1024;
    bool g_mhInit = false;

    uint8_t* Alloc(size_t n)
    {
        if (!g_pool)
            g_pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPoolSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_pool || g_poolUsed + n > kPoolSize)
            return nullptr;
        auto p = g_pool + g_poolUsed;
        g_poolUsed += (n + 15) & ~size_t(15);
        return p;
    }

    bool ParsePattern(const char* pat, std::vector<uint8_t>& bytes, std::vector<bool>& mask)
    {
        while (*pat)
        {
            if (*pat == ' ') { ++pat; continue; }
            if (*pat == '?')
            {
                bytes.push_back(0); mask.push_back(false);
                ++pat;
                if (*pat == '?') ++pat;
                continue;
            }
            char hex[3] = { pat[0], pat[1], 0 };
            if (!pat[1]) return false;
            bytes.push_back(static_cast<uint8_t>(strtoul(hex, nullptr, 16)));
            mask.push_back(true);
            pat += 2;
        }
        return !bytes.empty();
    }
}

void Mem::InitExe()
{
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (std::memcmp(sec->Name, ".text", 5) == 0)
        {
            g_text.start = g_base + sec->VirtualAddress;
            g_text.size = sec->Misc.VirtualSize;
        }
    }
}

Mem::Range Mem::ExeText() { return g_text; }
uintptr_t Mem::ExeBase() { return g_base; }

std::vector<uintptr_t> Mem::FindPattern(const char* pattern)
{
    std::vector<uintptr_t> out;
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    if (!ParsePattern(pattern, bytes, mask) || !g_text.start)
        return out;

    // Anchor on the first fixed byte for a fast memchr scan.
    size_t anchor = 0;
    while (anchor < mask.size() && !mask[anchor]) ++anchor;
    if (anchor == mask.size())
        return out;

    const uint8_t* begin = reinterpret_cast<const uint8_t*>(g_text.start);
    const uint8_t* end = begin + g_text.size - bytes.size();
    const uint8_t* p = begin + anchor;
    while (p < end + anchor)
    {
        p = static_cast<const uint8_t*>(std::memchr(p, bytes[anchor], (end + anchor) - p));
        if (!p) break;
        const uint8_t* cand = p - anchor;
        bool ok = true;
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            if (mask[i] && cand[i] != bytes[i]) { ok = false; break; }
        }
        if (ok) out.push_back(reinterpret_cast<uintptr_t>(cand));
        ++p;
    }
    return out;
}

uintptr_t Mem::Find(const char* name, const char* pattern, size_t index, size_t expected)
{
    auto r = FindPattern(pattern);
    if (r.size() != expected || index >= r.size())
    {
        LOG("  [!] pattern '%s': %u matches (expected %u) - feature part skipped", name, unsigned(r.size()), unsigned(expected));
        return 0;
    }
    if (g_settings.logLevel >= 2)
        LOG("  pattern '%s' -> %08X", name, unsigned(r[index]));
    return r[index];
}

bool Mem::Unprotect(uintptr_t addr, size_t size, DWORD* oldProtect)
{
    return VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, oldProtect) != FALSE;
}

void Mem::WriteBytes(uintptr_t addr, const void* data, size_t size)
{
    DWORD old;
    if (!Unprotect(addr, size, &old))
        return;
    std::memcpy(reinterpret_cast<void*>(addr), data, size);
    VirtualProtect(reinterpret_cast<void*>(addr), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
}

void Mem::Nop(uintptr_t addr, size_t size)
{
    std::vector<uint8_t> n(size, 0x90);
    WriteBytes(addr, n.data(), size);
}

static void MakeBranch(uintptr_t at, const void* target, size_t len, uint8_t op)
{
    std::vector<uint8_t> b(std::max<size_t>(len, 5), 0x90);
    b[0] = op;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (at + 5));
    std::memcpy(&b[1], &rel, 4);
    Mem::WriteBytes(at, b.data(), b.size());
}

void Mem::MakeJmp(uintptr_t at, const void* target, size_t len) { MakeBranch(at, target, len, 0xE9); }
void Mem::MakeCall(uintptr_t at, const void* target, size_t len) { MakeBranch(at, target, len, 0xE8); }

bool Mem::MidHook(const char* name, uintptr_t at, size_t len, MidHandler handler, MidMode mode)
{
    if (!at || len < 5)
        return false;

    const uint8_t* orig = reinterpret_cast<const uint8_t*>(at);
    if (mode != MidMode::Replace)
    {
        // Refuse to replay relative branches; they would need relocation.
        const uint8_t b0 = orig[0];
        if (b0 == 0xE8 || b0 == 0xE9 || b0 == 0xEB || (b0 >= 0x70 && b0 <= 0x7F) || (b0 == 0x0F && (orig[1] & 0xF0) == 0x80))
        {
            LOG("  [!] midhook '%s': first instruction is a relative branch", name);
            return false;
        }
    }

    uint8_t* t = Alloc(len * 2 + 64);
    if (!t)
        return false;
    uint8_t* p = t;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) *p++ = b; };
    auto emit32 = [&](uint32_t v) { std::memcpy(p, &v, 4); p += 4; };

    if (mode == MidMode::OriginalBefore) { std::memcpy(p, orig, len); p += len; }
    emit({ 0x9C });                         // pushfd
    emit({ 0x60 });                         // pushad
    emit({ 0x83, 0x44, 0x24, 0x0C, 0x04 }); // add dword ptr [esp+0Ch], 4  -> Regs::esp == esp at hook site
    emit({ 0x54 });                         // push esp
    emit({ 0xB8 }); emit32(reinterpret_cast<uint32_t>(handler)); // mov eax, handler
    emit({ 0xFF, 0xD0 });                   // call eax
    emit({ 0x83, 0xC4, 0x04 });             // add esp, 4
    emit({ 0x61 });                         // popad
    emit({ 0x9D });                         // popfd
    if (mode == MidMode::OriginalAfter) { std::memcpy(p, orig, len); p += len; }
    emit({ 0xE9 }); emit32(static_cast<uint32_t>((at + len) - (reinterpret_cast<uintptr_t>(p) + 4)));

    MakeJmp(at, t, len);
    if (g_settings.logLevel >= 2)
        LOG("  midhook '%s' at %08X (%u bytes)", name, unsigned(at), unsigned(len));
    return true;
}

bool Mem::CallHook(const char* name, uintptr_t site, MidHandler handler)
{
    if (!site || *reinterpret_cast<const uint8_t*>(site) != 0xE8)
    {
        LOG("  [!] callhook '%s': not a call instruction", name);
        return false;
    }
    const uintptr_t target = site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
    uint8_t* t = Alloc(64);
    if (!t)
        return false;
    uint8_t* p = t;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) *p++ = b; };
    auto emit32 = [&](uint32_t v) { std::memcpy(p, &v, 4); p += 4; };
    emit({ 0x9C });                         // pushfd
    emit({ 0x60 });                         // pushad
    emit({ 0x83, 0x44, 0x24, 0x0C, 0x08 }); // add dword ptr [esp+0Ch], 8  (skip eflags + return address)
    emit({ 0x54 });                         // push esp
    emit({ 0xB8 }); emit32(reinterpret_cast<uint32_t>(handler));
    emit({ 0xFF, 0xD0 });                   // call eax
    emit({ 0x83, 0xC4, 0x04 });             // add esp, 4
    emit({ 0x61 });                         // popad
    emit({ 0x9D });                         // popfd
    emit({ 0xE9 }); emit32(static_cast<uint32_t>(target - (reinterpret_cast<uintptr_t>(p) + 4))); // jmp original
    MakeCall(site, t, 5);
    if (g_settings.logLevel >= 2)
        LOG("  callhook '%s' at %08X -> %08X", name, unsigned(site), unsigned(target));
    return true;
}

bool Mem::Detour(const char* name, void* target, void* detour, void** original)
{
    if (!target)
        return false;
    if (!g_mhInit)
    {
        if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
            return false;
        g_mhInit = true;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK)
    {
        LOG("  [!] detour '%s' at %08X failed", name, unsigned(reinterpret_cast<uintptr_t>(target)));
        return false;
    }
    if (g_settings.logLevel >= 2)
        LOG("  detour '%s' at %08X", name, unsigned(reinterpret_cast<uintptr_t>(target)));
    return true;
}

void* Mem::PatchIAT(HMODULE module, const char* dllName, const char* funcName, void* replacement)
{
    if (!module)
        return nullptr;
    const auto base = reinterpret_cast<uintptr_t>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return nullptr;

    // Address of the real function, used when the name table is unavailable.
    void* realAddr = nullptr;
    if (HMODULE dll = GetModuleHandleA(dllName))
        realAddr = reinterpret_cast<void*>(GetProcAddress(dll, funcName));

    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dllName) != 0)
            continue;
        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        auto names = imp->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk) : nullptr;
        for (; thunk->u1.Function; ++thunk)
        {
            bool match = false;
            if (names)
            {
                if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                {
                    auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    match = std::strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) == 0;
                }
                ++names;
            }
            if (!match && realAddr && reinterpret_cast<void*>(thunk->u1.Function) == realAddr)
                match = true;
            if (match)
            {
                void* old = reinterpret_cast<void*>(thunk->u1.Function);
                Write<uintptr_t>(reinterpret_cast<uintptr_t>(&thunk->u1.Function), reinterpret_cast<uintptr_t>(replacement));
                return old;
            }
        }
    }
    return nullptr;
}

void* Mem::PatchVTable(void** vtable, size_t index, void* replacement)
{
    void* old = vtable[index];
    if (old != replacement)
        Write<void*>(reinterpret_cast<uintptr_t>(&vtable[index]), replacement);
    return old;
}
