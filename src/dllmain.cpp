// Entry point. The mod ships as d3d9.dll in the game's System folder: the game imports
// Direct3DCreate9 statically, so Windows loads us before the executable starts.
// Patching is deferred until the executable's CRT first calls GetSystemTimeAsFileTime,
// i.e. after the Steam DRM stub has handed control to the real entry point.
#include "pch.h"
#include "features.h"
#include "version.h"
#include "perf.h"

HMODULE g_self = nullptr;

namespace Game
{
    HWND hWnd = nullptr;
    GetFullNameFn GetFullName = nullptr;
}

namespace
{
    using GetSystemTimeAsFileTime_t = void(WINAPI*)(LPFILETIME);
    GetSystemTimeAsFileTime_t oGetSystemTimeAsFileTime = nullptr;
    volatile LONG g_initDone = 0;

    std::wstring SelfDir()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(g_self, path, MAX_PATH);
        std::wstring p(path);
        return p.substr(0, p.find_last_of(L"\\/") + 1);
    }

    // SEH wrapper so a crash inside one feature's setup disables that feature instead of the game.
    using InitFn = void(*)();
    void SafeInit(const char* name, InitFn fn)
    {
        __try
        {
            fn();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG("[!] %s: exception 0x%08X during setup, feature partially disabled", name, GetExceptionCode());
        }
    }

    void CheckExecutable()
    {
        const auto base = Mem::ExeBase();
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const DWORD stamp = nt->FileHeader.TimeDateStamp;
        LOG("Executable: base %08X, timestamp %08X%s", unsigned(base), unsigned(stamp),
            stamp == 0x431DDA4A ? " (Steam 1.05, supported)" : " (unknown build: some fixes may not apply)");
    }

    void InitAll()
    {
        Mem::InitExe();
        CheckExecutable();
        SafeInit("Window (DPI)", &Window::InitEarly);
        SafeInit("Watchdog", &Watchdog::Init);
        SafeInit("Perf", &Perf::Init);

        if (auto a = Mem::Find("UObject::GetFullName", "56 8B 74 24 08 85 F6 57 8B F9 75"))
            Game::GetFullName = reinterpret_cast<Game::GetFullNameFn>(a);

        SafeInit("Mods", &Mods::Init);
        SafeInit("Textures", &Textures::Init);
        SafeInit("Framerate", &Framerate::Init);
        SafeInit("Widescreen", &Widescreen::Init);
        SafeInit("Window", &Window::Init);
        SafeInit("RawMouse", &RawMouse::Init);
        SafeInit("Videos", &Videos::Init);
        SafeInit("Misc", &Misc::Init);
        SafeInit("Stuck", &Stuck::Init);
        SafeInit("Script", &Script::Init);
        SafeInit("Movement", &Movement::Init);
        SafeInit("Guards", &Guards::Init);
        SafeInit("Reaction", &Reaction::Init);       // before Perception: it hooks the event dispatcher for focus
        SafeInit("Perception", &Perception::Init);
        SafeInit("Noise", &Noise::Init);
        SafeInit("Alert", &Alert::Init);
        SafeInit("Aim", &Aim::Init);
        SafeInit("Scripted", &Scripted::Init);
        SafeInit("Clusterfuck", &Clusterfuck::Init);
        SafeInit("Plush", &Plush::Init);             // after Clusterfuck (its unlock), before Scripting (its Lua)
        SafeInit("Scripting", &Scripting::Init);
        SafeInit("Editor", &Editor::Init);
        LOG("Init complete");
    }

    void WINAPI hkGetSystemTimeAsFileTime(LPFILETIME ft)
    {
        if (InterlockedExchange(&g_initDone, 1) == 0)
        {
            Mem::PatchIAT(GetModuleHandleW(nullptr), "KERNEL32.dll", "GetSystemTimeAsFileTime", reinterpret_cast<void*>(oGetSystemTimeAsFileTime));
            InitAll();
        }
        oGetSystemTimeAsFileTime(ft);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    g_self = module;
    DisableThreadLibraryCalls(module);

    const std::wstring dir = SelfDir();
    Log::Init(dir + L"SCCTRemaster.log");
    LOG("%s %s", SCCTR_NAME, SCCTR_VERSION);
    Config::Load(dir + L"SCCTRemaster.ini");
    Config::Dump();

    // Only patch the game itself (testapp.exe is a byte-identical copy and also supported).
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    if (_wcsicmp(name, L"splintercell3.exe") != 0 && _wcsicmp(name, L"testapp.exe") != 0)
    {
        LOG("Host '%ls' is not the game: acting as a plain d3d9 proxy", name);
        return TRUE;
    }

    oGetSystemTimeAsFileTime = reinterpret_cast<GetSystemTimeAsFileTime_t>(
        Mem::PatchIAT(GetModuleHandleW(nullptr), "KERNEL32.dll", "GetSystemTimeAsFileTime", reinterpret_cast<void*>(&hkGetSystemTimeAsFileTime)));
    if (!oGetSystemTimeAsFileTime)
    {
        LOG("Could not defer initialisation (IAT entry missing); initialising now");
        g_initDone = 1;
        InitAll();
    }
    return TRUE;
}
