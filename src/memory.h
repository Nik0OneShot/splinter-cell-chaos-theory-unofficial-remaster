#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>

namespace Mem
{
    // Register snapshot handed to mid-function hook handlers. Layout matches pushad + pushfd.
    // 'esp' is the stack pointer at the hooked instruction (writes to it are ignored).
    struct Regs
    {
        uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
        uint32_t eflags;

        template <class T> T& Stack(int offset) const { return *reinterpret_cast<T*>(esp + offset); }
    };

    using MidHandler = void(__cdecl*)(Regs*);

    enum class MidMode
    {
        OriginalBefore, // run the overwritten instructions, then the handler
        OriginalAfter,  // run the handler, then the overwritten instructions
        Replace,        // handler fully emulates the overwritten instructions
    };

    struct Range { uintptr_t start = 0; size_t size = 0; };

    void InitExe();
    Range ExeText();
    uintptr_t ExeBase();

    // IDA-style pattern ("8B 15 ? ? ? ? A1"). Scans the exe's .text section.
    std::vector<uintptr_t> FindPattern(const char* pattern);
    // Returns the single match, or 0 (and logs) when the match count is not exactly 'expected' (use index to pick).
    uintptr_t Find(const char* name, const char* pattern, size_t index = 0, size_t expected = 1);

    bool Unprotect(uintptr_t addr, size_t size, DWORD* oldProtect);
    void WriteBytes(uintptr_t addr, const void* data, size_t size);
    template <class T> inline void Write(uintptr_t addr, T value) { WriteBytes(addr, &value, sizeof(T)); }
    template <class T> inline T Read(uintptr_t addr) { T v; std::memcpy(&v, reinterpret_cast<void*>(addr), sizeof(T)); return v; }
    void Nop(uintptr_t addr, size_t size);
    void MakeJmp(uintptr_t at, const void* target, size_t len = 5);
    void MakeCall(uintptr_t at, const void* target, size_t len = 5);

    // Mid-function hook. 'len' bytes at 'at' are overwritten (min 5) and must contain whole,
    // position-independent instructions when the mode replays them.
    bool MidHook(const char* name, uintptr_t at, size_t len, MidHandler handler, MidMode mode);

    // Hooks a 'call rel32' instruction: the handler runs with the registers/stack as they are at
    // the call instruction (Regs::esp points at the pushed arguments), then the original target runs.
    bool CallHook(const char* name, uintptr_t callSite, MidHandler handler);

    // Inline function detour (MinHook).
    bool Detour(const char* name, void* target, void* detour, void** original);

    // Replaces an import address table entry of 'module'. Returns the previous pointer or nullptr.
    void* PatchIAT(HMODULE module, const char* dllName, const char* funcName, void* replacement);

    // Swaps a vtable slot. Returns the previous pointer.
    void* PatchVTable(void** vtable, size_t index, void* replacement);
}
