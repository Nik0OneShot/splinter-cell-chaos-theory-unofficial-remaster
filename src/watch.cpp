// [Debug] WatchVisCrouchMul: hardware data breakpoint on GameInfo.VisCrouchMul.
//
// GameInfo.VisCrouchMul (+2D8h, 0.9 in SplinterCell3Settings.ini) looks like a "crouched = less visible"
// multiplier, but no script or native code found by static analysis reads it (natives reach GameInfo via
// Actor.Level +78h -> LevelInfo.Game +5D0h, e.g. VisibilityTableLookup at 0x10B9A7D0 for the thresholds
// right before it). To settle it at runtime, debug register DR0 is set on the game thread to trap every read
// or write of that float, and the address of each distinct instruction that touches it is logged.
#include "pch.h"
#include "features.h"

namespace
{
    constexpr uintptr_t kActorLevel = 0x78, kLevelGame = 0x5D0, kVisCrouchMul = 0x2D8;

    std::atomic<uintptr_t> g_target{ 0 };
    DWORD g_gameThread = 0;
    HANDLE g_wake = nullptr;

    // Filled by the exception handler (no logging there), reported from the game thread.
    constexpr int kMaxHits = 32;
    std::atomic<uint32_t> g_hitAddr[kMaxHits];
    std::atomic<uint32_t> g_hitCount[kMaxHits];
    int g_reported = 0;

    LONG CALLBACK OnException(EXCEPTION_POINTERS* e)
    {
        if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
            return EXCEPTION_CONTINUE_SEARCH;
        const uint32_t at = static_cast<uint32_t>(e->ContextRecord->Eip);   // instruction after the access
        for (int i = 0; i < kMaxHits; ++i)
        {
            uint32_t cur = g_hitAddr[i].load();
            if (cur == 0 && g_hitAddr[i].compare_exchange_strong(cur, at))
                cur = at;
            if (cur == at)
            {
                ++g_hitCount[i];
                break;
            }
        }
        e->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Debug registers of another thread can only be changed while it is suspended.
    DWORD WINAPI ArmThread(void*)
    {
        uintptr_t armed = 0;
        for (;;)
        {
            WaitForSingleObject(g_wake, INFINITE);
            const uintptr_t target = g_target.load();
            if (target == armed)
                continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, g_gameThread);
            if (!t)
                continue;
            if (SuspendThread(t) != DWORD(-1))
            {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(t, &c))
                {
                    c.Dr0 = target;
                    c.Dr6 = 0;
                    c.Dr7 = (c.Dr7 & ~0x000F0003u) | 0x1u | (3u << 16) | (3u << 18);   // L0, read/write, 4 bytes
                    if (SetThreadContext(t, &c))
                        armed = target;
                }
                ResumeThread(t);
            }
            CloseHandle(t);
            LOG("Watch: VisCrouchMul at %08X %s", unsigned(target), armed == target ? "watched" : "could not be watched");
        }
    }
}

void Watch::OnPlayerTick(void* pawn)
{
    if (!g_settings.watchVisCrouchMul || !pawn)
        return;
    if (!g_wake)
    {
        g_gameThread = GetCurrentThreadId();
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        AddVectoredExceptionHandler(1, &OnException);
        CloseHandle(CreateThread(nullptr, 0, &ArmThread, nullptr, 0, nullptr));
        LOG("Watch: VisCrouchMul watch enabled (game thread %lu)", g_gameThread);
    }
    const uintptr_t level = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(pawn) + kActorLevel);
    const uintptr_t game = level ? *reinterpret_cast<uintptr_t*>(level + kLevelGame) : 0;
    if (game && game + kVisCrouchMul != g_target.load())
    {
        g_target = game + kVisCrouchMul;
        LOG("Watch: GameInfo %08X, VisCrouchMul = %.3f", unsigned(game), *reinterpret_cast<float*>(game + kVisCrouchMul));
        SetEvent(g_wake);
    }
    // Report new hits (and a running count every ~10 s).
    static DWORD lastSummary = GetTickCount();
    for (; g_reported < kMaxHits && g_hitAddr[g_reported].load(); ++g_reported)
        LOG("Watch: VisCrouchMul accessed by the instruction before %08X", g_hitAddr[g_reported].load());
    if (GetTickCount() - lastSummary > 10000)
    {
        lastSummary = GetTickCount();
        if (g_reported == 0)
            LOG("Watch: no code has read or written VisCrouchMul so far");
        for (int i = 0; i < g_reported; ++i)
            LOG("Watch:   %08X x%u", g_hitAddr[i].load(), g_hitCount[i].load());
    }
}
