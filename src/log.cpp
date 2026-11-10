#include "pch.h"
#include <cstdarg>

namespace
{
    FILE* g_file = nullptr;
    CRITICAL_SECTION g_lock;
    bool g_lockInit = false;
    LARGE_INTEGER g_start{}, g_freq{};
}

void Log::Init(const std::wstring& path)
{
    if (!g_lockInit)
    {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void Log::Write(const char* fmt, ...)
{
    if (!g_file || !g_lockInit)
        return;

    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);

    EnterCriticalSection(&g_lock);
    fprintf(g_file, "[%9.3f] %s\n", t, buf);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

void Log::Flush()
{
    if (g_file)
        fflush(g_file);
}
