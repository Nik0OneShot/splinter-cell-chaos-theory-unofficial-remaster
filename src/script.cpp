// Calling UnrealScript from native code, the way the engine raises script events (e.g. the stub that raises
// NotifyEnterDoorRadius): FName constructor (0x10CD4790; thiscall on the FName, (const TCHAR*, EFindName)),
// UObject FindFunction (0x10CD92D0; (FName, INT Global), returns the UFunction or null) and ProcessEvent
// (UObject vtable +10h; (UFunction*, void* Parms, void* Result)). Only script (non-native) functions can be
// called this way: a native function reads its arguments from bytecode, which ProcessEvent doesn't supply.
// A script state is UObject.StateFrame (+0Ch) -> FStateFrame.StateNode (+1Ch), whose FName is at +20h.
#include "pch.h"
#include "features.h"

// Native functions: called like the script VM calls them, with a minimal FFrame (vtable +0, Node +4, Object +8,
// Code +0Ch, Locals +10h) whose Code is a few bytes of bytecode holding the arguments (EX_NameConst 21h + FName,
// EX_FloatConst 1Eh + float, EX_IntConst 1Dh + int, EX_False 28h, EX_Nothing 0Bh for a skipped optional
// argument, EX_EndFunctionParms 16h; the natives' P_FINISH then checks the next byte for 42h, so a 0 follows).
// The native's code is GNatives[iNative] (table found in execMakeNoise: call [edx*4+GNatives]) or, for natives
// without a number, UFunction.Func, whose offset is found once by matching EPawn.BlendAnimOverCurrent (1538).
namespace
{
    using FNameCtor_t = void*(__fastcall*)(void* self, void* edx, const wchar_t* name, int find);
    using FindFunction_t = void*(__fastcall*)(void* obj, void* edx, int name, int global);
    using ProcessEvent_t = void(__fastcall*)(void* obj, void* edx, void* func, void* parms, void* result);
    using Exec_t = void(__fastcall*)(void* obj, void* edx, void* frame, void* result);
    FNameCtor_t pFNameCtor = nullptr;
    FindFunction_t pFindFunction = nullptr;
    void** g_natives = nullptr;
    void*** g_namesData = nullptr;   // &FName::Names (TArray<FNameEntry*>: Data, Num); entry text at +0Ch
    int g_funcOffset = -1;   // -1 not looked for yet, 0 not found

    void __fastcall FrameLog(void*, void*, const wchar_t*, int) {}
    void* g_frameVtbl[16] = { &FrameLog, &FrameLog, &FrameLog, &FrameLog, &FrameLog, &FrameLog, &FrameLog, &FrameLog,
                              &FrameLog, &FrameLog, &FrameLog, &FrameLog, &FrameLog, &FrameLog, &FrameLog, &FrameLog };
    struct Frame { void** vtbl; void* node; void* object; const uint8_t* code; uint8_t* locals; uint8_t extra[0x40]; };

    void* SafeFind(void* obj, int name)
    {
        __try { return pFindFunction(obj, nullptr, name, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    int FindFuncOffset(void* fn, void* target)
    {
        __try
        {
            for (int off = 0x28; off < 0x200; off += 4)
                if (*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(fn) + off) == target) return off;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    bool SafeExec(Exec_t exec, void* obj, Frame* frame, void* result)
    {
        __try { exec(obj, nullptr, frame, result); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

void* Script::NativeCode(void* obj, int function)
{
    if (!Ready() || !g_natives || !obj || !function) return nullptr;
    if (g_funcOffset < 0)
    {
        g_funcOffset = 0;
        const int blend = Name(L"BlendAnimOverCurrent");
        if (void* fn = blend ? SafeFind(obj, blend) : nullptr)
            g_funcOffset = FindFuncOffset(fn, g_natives[1538]);
        LOG("Script: UFunction.Func %s", g_funcOffset ? "found" : "not found (native calls unavailable)");
    }
    if (!g_funcOffset) return nullptr;
    void* fn = SafeFind(obj, function);
    if (!fn) return nullptr;
    __try { return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(fn) + g_funcOffset); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* Script::Function(void* obj, int function)
{
    if (!Ready() || !obj || !function) return nullptr;
    return SafeFind(obj, function);
}

void* Script::Native(int index)
{
    if (!g_natives || index <= 0 || index >= 4096) return nullptr;
    __try { return g_natives[index]; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void** Script::NativeSlot(int index)
{
    if (!g_natives || index <= 0 || index >= 4096) return nullptr;
    return &g_natives[index];
}

bool Script::CallNative(void* obj, void* code, const uint8_t* args)
{
    if (!obj || !code || !args) return false;
    alignas(16) uint8_t locals[256] = {};
    alignas(16) uint8_t result[32] = {};
    Frame frame{ g_frameVtbl, nullptr, obj, args, locals, {} };
    return SafeExec(reinterpret_cast<Exec_t>(code), obj, &frame, result);
}

void Script::Args::Name(int name) { b.push_back(0x21); Raw(&name, 4); }
void Script::Args::Float(float f) { b.push_back(0x1E); Raw(&f, 4); }
void Script::Args::Int(int i) { b.push_back(0x1D); Raw(&i, 4); }
void Script::Args::Bool(bool v) { b.push_back(v ? 0x27 : 0x28); }
void Script::Args::Skip() { b.push_back(0x0B); }
void Script::Args::Byte(uint8_t v) { b.push_back(0x24); b.push_back(v); }   // EX_ByteConst
const uint8_t* Script::Args::End() { b.push_back(0x16); b.push_back(0x00); b.push_back(0x00); return b.data(); }
void Script::Args::Raw(const void* p, size_t n) { const auto* c = static_cast<const uint8_t*>(p); b.insert(b.end(), c, c + n); }

void Script::Init()
{
    pFNameCtor = reinterpret_cast<FNameCtor_t>(Mem::Find("FName constructor",
        "83 EC 44 A1 ? ? ? ? 55 8B 6C 24 4C 85 ED 56 89 44 24 48 8B F1 74 07 66 83 7D 00 00 75 19 C7 06 00 00 00 00"));
    // The script-event stubs (two identical ones) call FindFunction at +20h.
    if (const uintptr_t a = Mem::Find("script event stub",
            "8B 15 ? ? ? ? 8A 44 24 04 56 57 8B F1 8B 3E 6A 00 8D 4C 24 10 51 6A 00 52 8B CE 88 44 24 1C E8", 0, 2))
        pFindFunction = reinterpret_cast<FindFunction_t>(a + 0x25 + *reinterpret_cast<const int32_t*>(a + 0x21));
    // execMakeNoise (and one twin): ... mov ecx,[esi+8]; push esi; call [edx*4 + GNatives]
    if (const uintptr_t a = Mem::Find("native table",
            "83 EC 24 56 8B 74 24 2C 8B 46 0C C7 44 24 0C 00 00 00 00 0F B6 10 40 57 89 46 0C 8D 44 24 10 50 8B F9 8B 4E 08 56 FF 14 95", 0, 2))
        g_natives = *reinterpret_cast<void***>(a + 41);
    // FName constructor: ... call Allocate; mov edx,[Names.Data]; mov [NameHash+ebx*4],eax; mov ecx,[esi]; mov [edx+ecx*4],eax
    if (const uintptr_t a = Mem::Find("FName table",
            "8B 0C 9D ? ? ? ? 8B 16 51 6A 00 52 55 E8 ? ? ? ? 8B 15 ? ? ? ? 89 04 9D ? ? ? ? 8B 0E 89 04 8A"))
        g_namesData = *reinterpret_cast<void****>(a + 21);
    LOG("Script: calling script functions %s, natives %s", Ready() ? "available" : "unavailable (pattern not found)",
        g_natives ? "available" : "unavailable");
}

bool Script::NameText(int name, wchar_t* out, size_t len)
{
    if (!out || !len) return false;
    out[0] = 0;
    if (!g_namesData || name <= 0) return false;
    __try
    {
        void** data = *g_namesData;
        const int num = *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(g_namesData) + 4);
        if (!data || name >= num || !data[name]) return false;
        // The text is TCHAR (wide); if it looks like 8-bit text instead, read it as that. Only printable ASCII
        // is copied, so the result is always safe to log.
        const uint8_t* b = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(data[name]) + 0xC);
        const bool wide = b[0] && !b[1];
        size_t i = 0;
        for (; i + 1 < len; ++i)
        {
            const unsigned c = wide ? reinterpret_cast<const wchar_t*>(b)[i] : b[i];
            if (!c) break;
            out[i] = (c >= 0x20 && c < 0x7F) ? static_cast<wchar_t>(c) : L'?';
        }
        out[i] = 0;
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

bool Script::Ready()
{
    return pFNameCtor && pFindFunction;
}

int Script::Name(const wchar_t* text)
{
    if (!pFNameCtor || !text || !*text) return 0;
    int name = 0;
    __try { pFNameCtor(&name, nullptr, text, 0); }   // FNAME_Find: 0 when the name doesn't exist
    __except (EXCEPTION_EXECUTE_HANDLER) { name = 0; }
    return name;
}

bool Script::Call(void* obj, int function, void* parms)
{
    if (!Ready() || !obj || !function) return false;
    __try
    {
        void* fn = pFindFunction(obj, nullptr, function, 0);
        if (!fn) return false;
        const auto pe = reinterpret_cast<ProcessEvent_t>((*reinterpret_cast<void***>(obj))[0x10 / 4]);
        pe(obj, nullptr, fn, parms, nullptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int Script::StateName(void* obj)
{
    if (!obj) return 0;
    __try
    {
        void* frame = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(obj) + 0xC);
        void* node = frame ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(frame) + 0x1C) : nullptr;
        return node ? *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(node) + 0x20) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
