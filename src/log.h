#pragma once

#include <string>

namespace Log
{
    // Opens (truncates) the log file. Safe to call once from DllMain.
    void Init(const std::wstring& path);
    void Write(const char* fmt, ...);
    void Flush();
}

#define LOG(...) ::Log::Write(__VA_ARGS__)
