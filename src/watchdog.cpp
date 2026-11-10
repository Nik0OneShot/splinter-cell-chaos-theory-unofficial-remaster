// Hang detector (diagnostics): if the game's main thread stops reaching the end of its frame loop
// for a few seconds, a watchdog thread captures where it is stuck (instruction pointer and a stack
// walk with module names) and writes it to the log. Logged once per hang.
#include "pch.h"
#include "features.h"

namespace
{
    std::atomic<DWORD> g_heartbeat{ 0 };
    DWORD g_mainThreadId = 0;
    HANDLE g_mainThread = nullptr;

    std::string Describe(uintptr_t addr)
    {
        HMODULE mod = nullptr;
        char buf[160];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &mod) && mod)
        {
            char path[MAX_PATH];
            GetModuleFileNameA(mod, path, MAX_PATH);
            const char* name = strrchr(path, '\\');
            snprintf(buf, sizeof(buf), "%08X %s+0x%X", unsigned(addr), name ? name + 1 : path, unsigned(addr - reinterpret_cast<uintptr_t>(mod)));
        }
        else
            snprintf(buf, sizeof(buf), "%08X ?", unsigned(addr));
        return buf;
    }

    bool IsCode(uintptr_t addr)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return false;
        return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 && mbi.Type == MEM_IMAGE;
    }

    void DumpMainThread()
    {
        if (SuspendThread(g_mainThread) == DWORD(-1))
            return;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(g_mainThread, &ctx))
        {
            LOG("[HANG] main thread has not finished a frame for 3 s. EIP %s", Describe(ctx.Eip).c_str());
            LOG("[HANG] eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X",
                unsigned(ctx.Eax), unsigned(ctx.Ebx), unsigned(ctx.Ecx), unsigned(ctx.Edx), unsigned(ctx.Esi), unsigned(ctx.Edi), unsigned(ctx.Ebp), unsigned(ctx.Esp));
            // Heuristic stack scan: every value on the stack that points into executable image code.
            int found = 0;
            for (uintptr_t sp = ctx.Esp; sp < ctx.Esp + 0x4000 && found < 40; sp += 4)
            {
                uintptr_t v = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(sp), &v, 4, nullptr))
                    break;
                if (IsCode(v))
                {
                    LOG("[HANG]   [esp+%04X] %s", unsigned(sp - ctx.Esp), Describe(v).c_str());
                    ++found;
                }
            }
        }
        ResumeThread(g_mainThread);
    }

    DWORD WINAPI WatchdogThread(LPVOID)
    {
        bool reported = false;
        for (;;)
        {
            Sleep(500);
            const DWORD last = g_heartbeat.load();
            if (!last)
                continue; // the frame loop hasn't started yet
            const DWORD age = GetTickCount() - last;
            if (age > 3000 && !reported)
            {
                reported = true;
                DumpMainThread();
            }
            else if (age < 1000 && reported)
            {
                reported = false;
                LOG("[HANG] main thread is running again");
            }
        }
    }
}

void Watchdog::Init()
{
    g_mainThreadId = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (HANDLE t = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr))
        CloseHandle(t);
}

void Watchdog::Heartbeat()
{
    g_heartbeat.store(GetTickCount());
}
