// Small fixes and package integration: intro skip, safe-mode prompt, EAX routing, Xidi, Steam overlay.
#include "pch.h"
#include "features.h"
#include <tlhelp32.h>

extern HMODULE g_self;

namespace
{
    std::wstring GameDir()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(g_self, path, MAX_PATH);
        std::wstring p(path);
        return p.substr(0, p.find_last_of(L"\\/") + 1);
    }

    bool FileExists(const std::wstring& p)
    {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    // ---- EAX: eax.dll creates DirectSound through COM, which would bypass the local
    // dsound.dll (DSOAL / ALchemy). Route it to the local DLL instead (no registry edits).
    const GUID kCLSID_DirectSound = { 0x47d4d946, 0x62e8, 0x11cf, { 0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    const GUID kCLSID_DirectSound8 = { 0x3901cc3f, 0x84b5, 0x4fa4, { 0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b } };
    using CoCreateInstance_t = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    CoCreateInstance_t oCoCreateInstance = nullptr;

    HRESULT WINAPI hkCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* ppv)
    {
        if (IsEqualGUID(clsid, kCLSID_DirectSound) || IsEqualGUID(clsid, kCLSID_DirectSound8))
        {
            const std::wstring local = GameDir() + L"dsound.dll";
            if (FileExists(local))
            {
                if (HMODULE dll = LoadLibraryW(local.c_str()))
                {
                    using GetClassObject_t = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
                    if (auto gco = reinterpret_cast<GetClassObject_t>(GetProcAddress(dll, "DllGetClassObject")))
                    {
                        IClassFactory* factory = nullptr;
                        HRESULT hr = gco(clsid, IID_IClassFactory, reinterpret_cast<LPVOID*>(&factory));
                        if (SUCCEEDED(hr) && factory)
                        {
                            hr = factory->CreateInstance(outer, iid, ppv);
                            factory->Release();
                            static bool logged = false;
                            if (!logged) { logged = true; LOG("EAX: DirectSound for eax.dll created from local dsound.dll -> 0x%08X", unsigned(hr)); }
                            return hr;
                        }
                    }
                }
            }
        }
        return oCoCreateInstance(clsid, outer, ctx, iid, ppv);
    }

    // ---- Xidi: controllers via XInput, without a dinput8.dll proxy.
    using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    DirectInput8Create_t oDirectInput8Create = nullptr;
    DirectInput8Create_t g_xidiCreate = nullptr;

    HRESULT WINAPI hkDirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN outer)
    {
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            const std::wstring xidi = GameDir() + L"Xidi.32.dll";
            if (FileExists(xidi))
            {
                if (HMODULE m = LoadLibraryW(xidi.c_str()))
                    g_xidiCreate = reinterpret_cast<DirectInput8Create_t>(GetProcAddress(m, "dinput8_DirectInput8Create"));
            }
            LOG("Xidi: %s", g_xidiCreate ? "loaded, controllers are exposed as Xidi virtual controllers" : "Xidi.32.dll not found, using system DirectInput");
        }
        if (g_xidiCreate)
            return g_xidiCreate(inst, ver, iid, out, outer);
        return oDirectInput8Create(inst, ver, iid, out, outer);
    }
}

namespace
{
    // ---- Steam overlay: Steam injects gameoverlayrenderer.dll into the game but never starts the overlay's UI
    // process (gameoverlayui64.exe) for Chaos Theory, so the renderer keeps waiting for it ("since last frame from
    // ui process was seen" in Steam\logs\gameoverlay_renderer.txt) and nothing appears: no FPS counter, Shift+Tab
    // or F12 screenshots. When Steam hasn't started one a few seconds after launch, start it the way Steam does
    // for other games (same arguments). It's created through CreateProcessInternalW because the renderer hooks
    // CreateProcessW to inject itself into the game's child processes, and the UI isn't on its exclusion list;
    // the Steam* launch variables are left out of its environment, like when Steam starts it.
    using NtQueryInformationProcess_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    using CreateProcessInternalW_t = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                                   LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE);

    bool HasPidArg(const std::wstring& cmd, DWORD pid)
    {
        const std::wstring key = L"-pid " + std::to_wstring(pid);
        for (size_t at = cmd.find(key); at != std::wstring::npos; at = cmd.find(key, at + 1))
        {
            const size_t end = at + key.size();
            if ((at == 0 || cmd[at - 1] == L' ') && (end == cmd.size() || !iswdigit(cmd[end])))
                return true;
        }
        return false;
    }

    // A running gameoverlayui(64).exe meant for this process: its command line says "-pid <us>", or, when the
    // command line can't be read, it was started after the game.
    bool FindOverlayUi(DWORD self, const FILETIME& selfStart, const char** how)
    {
        static const auto query = reinterpret_cast<NtQueryInformationProcess_t>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return false;
        bool found = false;
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        {
            if (_wcsicmp(pe.szExeFile, L"gameoverlayui64.exe") != 0 && _wcsicmp(pe.szExeFile, L"gameoverlayui.exe") != 0)
                continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h)
                continue;
            struct Str { USHORT length, maxLength; const wchar_t* text; };
            alignas(8) uint8_t buf[4096];
            ULONG len = 0;
            const auto* s = reinterpret_cast<const Str*>(buf);
            if (query && query(h, 60 /* ProcessCommandLineInformation */, buf, sizeof(buf), &len) >= 0 &&
                reinterpret_cast<const uint8_t*>(s->text) >= buf && reinterpret_cast<const uint8_t*>(s->text) + s->length <= buf + sizeof(buf))
            {
                found = HasPidArg(std::wstring(s->text, s->length / sizeof(wchar_t)), self);
                *how = "Steam started it";
            }
            else
            {
                FILETIME created, e, k, u;
                found = GetProcessTimes(h, &created, &e, &k, &u) && CompareFileTime(&created, &selfStart) >= 0;
                *how = "one was started after the game";
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        return found;
    }

    std::vector<wchar_t> OverlayEnvironment()
    {
        std::vector<wchar_t> out;
        if (wchar_t* env = GetEnvironmentStringsW())
        {
            for (const wchar_t* p = env; *p; p += wcslen(p) + 1)
            {
                if (_wcsnicmp(p, L"Steam", 5) == 0 || _wcsnicmp(p, L"SDL_", 4) == 0 || _wcsnicmp(p, L"EnableConfiguratorSupport", 25) == 0)
                    continue;
                out.insert(out.end(), p, p + wcslen(p) + 1);
            }
            FreeEnvironmentStringsW(env);
        }
        out.push_back(0);
        if (out.size() == 1)
            out.push_back(0);
        return out;
    }

    DWORD WINAPI OverlayThread(LPVOID)
    {
        const DWORD self = GetCurrentProcessId();
        FILETIME selfStart = {}, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &selfStart, &e, &k, &u);

        // Steam starts its UI within a second or two of launch when it does at all.
        const char* how = "";
        for (int i = 0; i < 10; ++i)
        {
            Sleep(500);
            if (FindOverlayUi(self, selfStart, &how))
            {
                LOG("Steam overlay: UI already running (%s), nothing to do", how);
                return 0;
            }
        }
        HMODULE renderer = GetModuleHandleW(L"gameoverlayrenderer.dll");
        if (!renderer)
        {
            LOG("Steam overlay: not loaded (game started outside Steam, or the overlay is turned off in Steam)");
            return 0;
        }

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(renderer, path, MAX_PATH);
        std::wstring dir(path);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        std::wstring exe = dir + L"gameoverlayui64.exe";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            exe = dir + L"gameoverlayui.exe";

        DWORD steamPid = 0, size = sizeof(steamPid);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid", RRF_RT_REG_DWORD, nullptr, &steamPid, &size);
        if (!steamPid)
        {
            LOG("Steam overlay: Steam's process id not found in the registry, not starting the UI");
            return 0;
        }
        wchar_t gameId[64] = L"13570";
        for (const wchar_t* var : { L"SteamOverlayGameId", L"SteamGameId", L"SteamAppId" })
        {
            wchar_t v[64];
            const DWORD n = GetEnvironmentVariableW(var, v, 64);
            if (n > 0 && n < 64) { wcscpy_s(gameId, v); break; }
        }

        std::wstring cmd = L"\"" + exe + L"\" -pid " + std::to_wstring(self) + L" -steampid " + std::to_wstring(steamPid) +
                           L" -manuallyclearframes 0 -gameid " + gameId;
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(0);
        std::vector<wchar_t> env = OverlayEnvironment();
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};

        auto internal = reinterpret_cast<CreateProcessInternalW_t>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateProcessInternalW"));
        if (!internal)
            internal = reinterpret_cast<CreateProcessInternalW_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateProcessInternalW"));
        const BOOL ok = internal
            ? internal(nullptr, exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(), dir.c_str(), &si, &pi, nullptr)
            : CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(), dir.c_str(), &si, &pi);
        if (!ok)
        {
            LOG("Steam overlay: Steam didn't start the overlay UI and starting %ls failed (error %lu)", exe.c_str(), GetLastError());
            return 0;
        }
        CloseHandle(pi.hThread);
        if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            LOG("Steam overlay: started the UI ourselves (Steam didn't), but it exited at once (code %d)", int(code));
        }
        else
            LOG("Steam overlay: Steam didn't start the overlay UI, started it ourselves (pid %lu, game id %ls%s)", pi.dwProcessId, gameId,
                internal ? "" : ", plain CreateProcessW");
        CloseHandle(pi.hProcess);
        return 0;
    }
}

void Misc::Init()
{
    const auto& s = g_settings;

    if (s.skipIntro)
    {
        if (auto a = Mem::Find("intro videos", "75 ? 68 ? ? ? ? E8 ? ? ? ? 50 E8 ? ? ? ? 83 C4 08 85 C0 75 ? 68"))
            Mem::Write<uint8_t>(a, 0xEB);
    }

    if (s.disableSafeMode)
    {
        if (auto a = Mem::Find("safe mode prompt", "E8 ? ? ? ? 68 ? ? ? ? 53 68"))
            Mem::Nop(a, 5);
    }

    if (s.eaxFix)
    {
        if (HMODULE eax = GetModuleHandleW(L"eax.dll"))
        {
            oCoCreateInstance = reinterpret_cast<CoCreateInstance_t>(Mem::PatchIAT(eax, "OLE32.DLL", "CoCreateInstance", reinterpret_cast<void*>(&hkCoCreateInstance)));
            if (!oCoCreateInstance)
                oCoCreateInstance = reinterpret_cast<CoCreateInstance_t>(Mem::PatchIAT(eax, "ole32.dll", "CoCreateInstance", reinterpret_cast<void*>(&hkCoCreateInstance)));
            LOG("EAX: %s", oCoCreateInstance ? "eax.dll DirectSound routing installed" : "eax.dll does not import CoCreateInstance");
        }
        else
            LOG("EAX: eax.dll not loaded");
    }

    if (s.steamOverlayFix)
    {
        if (HANDLE t = CreateThread(nullptr, 0, &OverlayThread, nullptr, 0, nullptr))
            CloseHandle(t);
    }

    if (s.useXidi)
    {
        oDirectInput8Create = reinterpret_cast<DirectInput8Create_t>(
            Mem::PatchIAT(GetModuleHandleW(nullptr), "DINPUT8.dll", "DirectInput8Create", reinterpret_cast<void*>(&hkDirectInput8Create)));
        if (!oDirectInput8Create)
            LOG("Xidi: could not hook DirectInput8Create");
        else
        {
            wchar_t path[MAX_PATH] = {};
            HMODULE di = GetModuleHandleW(L"dinput8.dll");
            if (di) GetModuleFileNameW(di, path, MAX_PATH);
            LOG("Xidi: DirectInput8Create hooked (dinput8 in use: %ls)", path);
        }
    }
}
