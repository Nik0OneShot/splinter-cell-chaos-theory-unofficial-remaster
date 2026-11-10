// Mods folder (mod support, phase 1).
//
// Mods live in <game>\Mods\<ModName>\. A mod's System\ and Data\ folders mirror the game's: a file there
// replaces the game's file with the same path whenever the game (or Bink, for videos) opens it, and files the
// game doesn't have are added to the game's folder listings, so a new package in Data\Maps or
// Data\StaticMeshes is found like a shipped one (the engine's [Core.System] Paths already cover
// Data\Textures, Data\StaticMeshes, Data\Animations, Data\Sounds and Data\Music, which the shipped game leaves
// empty). Everything else in a mod's folder is reserved for later mod features (scripts, level overlays).
//
// mod.ini (optional; defaults in brackets):
//   [Mod]
//   Name = (folder name)   Author =   Version =   Description =
//   Enabled = 1            Priority = 0   (when two mods replace the same file, the higher priority wins;
//                                          equal priorities: the folder name later in the alphabet wins)
// Folders whose name starts with '_' or '.' are ignored (an easy way to switch a mod off).
//
// How: kernel32 CreateFileW/A, GetFileAttributesW/A/ExW and FindFirstFileW/A + FindNextFileW/A + FindClose are
// detoured, but only when at least one mod has files. Only read-only opens are redirected. .ini, .log, .exe,
// .dll and .asi files are never replaced (the ini files hold the player's settings; DLLs are loaded through
// LoadLibrary, which this doesn't touch). A listing is only rebuilt for folders a mod has files in, so the
// rest of the game's file access passes straight through.
#include "pch.h"
#include "features.h"

namespace
{
    struct ModInfo
    {
        std::wstring folder, name, author, version, description, dir;
        int priority = 0;
        size_t replaced = 0, added = 0;
    };

    struct ModFile
    {
        std::wstring path;      // full path of the mod's file
        std::wstring rel;       // path relative to the game folder, original case (for the log)
        std::wstring fileName;  // file name, original case (for folder listings)
        size_t mod = 0;         // index into g_mods
    };

    std::wstring g_root;        // game folder, lower case, with a trailing backslash
    std::wstring g_modsDir;     // <game>\Mods\ (original case)
    std::vector<ModInfo> g_mods;
    std::unordered_map<std::wstring, ModFile> g_files;                // relative path, lower case -> mod file
    std::unordered_map<std::wstring, std::vector<const ModFile*>> g_dirs; // relative folder, lower case -> its mod files
    bool g_logFiles = true;

    std::mutex g_noteLock;
    std::unordered_set<std::wstring> g_noted;

    thread_local int t_inHook = 0;
    struct InHook
    {
        InHook() { ++t_inHook; }
        ~InHook() { --t_inHook; }
    };

    using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using GetFileAttributesW_t = DWORD(WINAPI*)(LPCWSTR);
    using GetFileAttributesA_t = DWORD(WINAPI*)(LPCSTR);
    using GetFileAttributesExW_t = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    using FindFirstFileW_t = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
    using FindFirstFileA_t = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
    using FindNextFileW_t = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAW);
    using FindNextFileA_t = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAA);
    using FindClose_t = BOOL(WINAPI*)(HANDLE);

    // The untouched functions: the system's own until a detour succeeds, then its trampoline. Safe to call
    // before the hooks exist, from inside them, and when one of the detours failed.
    CreateFileW_t oCreateFileW = &::CreateFileW;
    CreateFileA_t oCreateFileA = &::CreateFileA;
    GetFileAttributesW_t oGetFileAttributesW = &::GetFileAttributesW;
    GetFileAttributesA_t oGetFileAttributesA = &::GetFileAttributesA;
    GetFileAttributesExW_t oGetFileAttributesExW = &::GetFileAttributesExW;
    FindFirstFileW_t oFindFirstFileW = &::FindFirstFileW;
    FindFirstFileA_t oFindFirstFileA = &::FindFirstFileA;
    FindNextFileW_t oFindNextFileW = &::FindNextFileW;
    FindNextFileA_t oFindNextFileA = &::FindNextFileA;
    FindClose_t oFindClose = &::FindClose;

    HANDLE RealFindFirst(LPCWSTR p, LPWIN32_FIND_DATAW d) { return oFindFirstFileW(p, d); }
    BOOL RealFindNext(HANDLE h, LPWIN32_FIND_DATAW d) { return oFindNextFileW(h, d); }
    BOOL RealFindClose(HANDLE h) { return oFindClose(h); }

    std::wstring Lower(std::wstring s)
    {
        if (!s.empty())
            CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
        return s;
    }

    std::wstring Widen(const char* s)
    {
        if (!s)
            return {};
        const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        if (n <= 0)
            return {};
        std::wstring w(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
        return w;
    }

    // Lower-case path relative to the game folder; false when the path is outside it.
    bool Relative(const wchar_t* path, std::wstring& rel)
    {
        if (!path || !path[0] || (path[0] == L'\\' && path[1] == L'\\'))   // UNC, \\?\ and \\.\ paths
            return false;
        wchar_t full[1024];
        const DWORD n = GetFullPathNameW(path, 1024, full, nullptr);
        if (n == 0 || n >= 1024 || n <= g_root.size())
            return false;
        CharLowerBuffW(full, n);
        if (wmemcmp(full, g_root.c_str(), g_root.size()) != 0)
            return false;
        rel.assign(full + g_root.size(), n - g_root.size());
        return true;
    }

    const ModFile* Lookup(const wchar_t* path)
    {
        std::wstring rel;
        if (!Relative(path, rel))
            return nullptr;
        const auto it = g_files.find(rel);
        return it == g_files.end() ? nullptr : &it->second;
    }

    void Note(const ModFile* f)
    {
        if (!g_logFiles)
            return;
        {
            std::lock_guard<std::mutex> lock(g_noteLock);
            if (!g_noted.insert(f->rel).second)
                return;
        }
        LOG("[MODS] %ls <- %ls", f->rel.c_str(), g_mods[f->mod].name.c_str());
    }

    bool ReadOnly(DWORD access, DWORD disposition)
    {
        const DWORD write = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
                            FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
        return !(access & write) && (disposition == OPEN_EXISTING || disposition == OPEN_ALWAYS);
    }

    // --- Opening files ---------------------------------------------------------------------------

    HANDLE WINAPI hkCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
    {
        if (t_inHook == 0 && ReadOnly(access, disp))
        {
            InHook guard;
            if (const ModFile* f = Lookup(name))
            {
                Note(f);
                return oCreateFileW(f->path.c_str(), access, share, sa, disp, flags, tmpl);
            }
        }
        return oCreateFileW(name, access, share, sa, disp, flags, tmpl);
    }

    HANDLE WINAPI hkCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
    {
        if (t_inHook == 0 && ReadOnly(access, disp))
        {
            InHook guard;
            if (const ModFile* f = Lookup(Widen(name).c_str()))
            {
                Note(f);
                return oCreateFileW(f->path.c_str(), access, share, sa, disp, flags, tmpl);
            }
        }
        return oCreateFileA(name, access, share, sa, disp, flags, tmpl);
    }

    DWORD WINAPI hkGetFileAttributesW(LPCWSTR name)
    {
        if (t_inHook == 0)
        {
            InHook guard;
            if (const ModFile* f = Lookup(name))
                return oGetFileAttributesW(f->path.c_str());
        }
        return oGetFileAttributesW(name);
    }

    DWORD WINAPI hkGetFileAttributesA(LPCSTR name)
    {
        if (t_inHook == 0)
        {
            InHook guard;
            if (const ModFile* f = Lookup(Widen(name).c_str()))
                return oGetFileAttributesW(f->path.c_str());
        }
        return oGetFileAttributesA(name);
    }

    BOOL WINAPI hkGetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info)
    {
        if (t_inHook == 0)
        {
            InHook guard;
            if (const ModFile* f = Lookup(name))
                return oGetFileAttributesExW(f->path.c_str(), level, info);
        }
        return oGetFileAttributesExW(name, level, info);
    }

    // --- Folder listings ---------------------------------------------------------------------------
    // A listing of a folder a mod has files in is built in full when it starts (the game's own entries with the
    // replaced ones swapped for the mod's, plus the mod's new files) and handed out from a handle of our own.

    struct Listing
    {
        std::vector<WIN32_FIND_DATAW> items;
        size_t next = 1;
    };
    std::mutex g_listLock;
    std::unordered_set<Listing*> g_listings;
    std::atomic<int> g_listingCount{ 0 };

    Listing* OurListing(HANDLE h)
    {
        if (g_listingCount.load(std::memory_order_acquire) == 0 || h == INVALID_HANDLE_VALUE || !h)
            return nullptr;
        std::lock_guard<std::mutex> lock(g_listLock);
        auto* l = static_cast<Listing*>(h);
        return g_listings.count(l) ? l : nullptr;
    }

    // Case-insensitive wildcard match ('*' and '?'), with "*.*" matching everything like Windows does.
    bool Matches(const wchar_t* spec, const wchar_t* name)
    {
        if (wcscmp(spec, L"*.*") == 0 || wcscmp(spec, L"*") == 0)
            return true;
        const wchar_t* star = nullptr;
        const wchar_t* resume = nullptr;
        while (*name)
        {
            if (*spec == L'*')
            {
                star = spec++;
                resume = name;
            }
            else if (*spec == L'?' || towlower(*spec) == towlower(*name))
            {
                ++spec;
                ++name;
            }
            else if (star)
            {
                spec = star + 1;
                name = ++resume;
            }
            else
                return false;
        }
        while (*spec == L'*')
            ++spec;
        return *spec == 0;
    }

    // Builds the merged listing for 'pattern'; false when no mod has files in that folder (pass through).
    bool BuildListing(const wchar_t* pattern, std::vector<WIN32_FIND_DATAW>& items)
    {
        if (!pattern)
            return false;
        const std::wstring pat = pattern;
        const size_t slash = pat.find_last_of(L"\\/");
        const std::wstring dir = slash == std::wstring::npos ? L"." : (slash == 0 ? pat.substr(0, 1) : pat.substr(0, slash));
        const std::wstring spec = slash == std::wstring::npos ? pat : pat.substr(slash + 1);
        if (spec.empty())
            return false;

        std::wstring relDir;
        if (!Relative(dir.c_str(), relDir))
            return false;
        while (!relDir.empty() && relDir.back() == L'\\')
            relDir.pop_back();
        const auto it = g_dirs.find(relDir);
        if (it == g_dirs.end())
            return false;

        WIN32_FIND_DATAW fd;
        HANDLE h = RealFindFirst(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
                items.push_back(fd);
            while (RealFindNext(h, &fd));
            RealFindClose(h);
        }

        int added = 0, replaced = 0;
        for (const ModFile* f : it->second)
        {
            if (!Matches(spec.c_str(), f->fileName.c_str()))
                continue;
            WIN32_FIND_DATAW mine;
            HANDLE m = RealFindFirst(f->path.c_str(), &mine);
            if (m == INVALID_HANDLE_VALUE)
                continue;
            RealFindClose(m);
            wcsncpy_s(mine.cFileName, f->fileName.c_str(), _TRUNCATE);
            mine.cAlternateFileName[0] = 0;
            bool found = false;
            for (auto& item : items)
            {
                if (_wcsicmp(item.cFileName, f->fileName.c_str()) == 0)
                {
                    item = mine;
                    found = true;
                    break;
                }
            }
            if (found)
                ++replaced;
            else
            {
                items.push_back(mine);
                ++added;
            }
        }
        if (g_logFiles && (added || replaced))
        {
            std::lock_guard<std::mutex> lock(g_noteLock);
            if (g_noted.insert(L"?list:" + Lower(pat)).second)
                LOG("[MODS] listing %ls: %d file(s) added and %d replaced by mods", pattern, added, replaced);
        }
        return true;
    }

    HANDLE StartListing(std::vector<WIN32_FIND_DATAW>&& items)
    {
        if (items.empty())
        {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        auto* l = new Listing{ std::move(items) };
        std::lock_guard<std::mutex> lock(g_listLock);
        g_listings.insert(l);
        g_listingCount.fetch_add(1, std::memory_order_release);
        return static_cast<HANDLE>(l);
    }

    void ToAnsi(const WIN32_FIND_DATAW& w, WIN32_FIND_DATAA& a)
    {
        a.dwFileAttributes = w.dwFileAttributes;
        a.ftCreationTime = w.ftCreationTime;
        a.ftLastAccessTime = w.ftLastAccessTime;
        a.ftLastWriteTime = w.ftLastWriteTime;
        a.nFileSizeHigh = w.nFileSizeHigh;
        a.nFileSizeLow = w.nFileSizeLow;
        a.dwReserved0 = w.dwReserved0;
        a.dwReserved1 = w.dwReserved1;
        WideCharToMultiByte(CP_ACP, 0, w.cFileName, -1, a.cFileName, MAX_PATH, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, w.cAlternateFileName, -1, a.cAlternateFileName, 14, nullptr, nullptr);
    }

    HANDLE WINAPI hkFindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW out)
    {
        if (t_inHook == 0)
        {
            InHook guard;
            std::vector<WIN32_FIND_DATAW> items;
            if (BuildListing(pattern, items))
            {
                if (!items.empty())
                    *out = items[0];
                return StartListing(std::move(items));
            }
        }
        return oFindFirstFileW(pattern, out);
    }

    HANDLE WINAPI hkFindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA out)
    {
        if (t_inHook == 0)
        {
            InHook guard;
            std::vector<WIN32_FIND_DATAW> items;
            if (BuildListing(Widen(pattern).c_str(), items))
            {
                if (!items.empty())
                    ToAnsi(items[0], *out);
                return StartListing(std::move(items));
            }
        }
        return oFindFirstFileA(pattern, out);
    }

    BOOL WINAPI hkFindNextFileW(HANDLE h, LPWIN32_FIND_DATAW out)
    {
        if (Listing* l = OurListing(h))
        {
            if (l->next >= l->items.size())
            {
                SetLastError(ERROR_NO_MORE_FILES);
                return FALSE;
            }
            *out = l->items[l->next++];
            return TRUE;
        }
        return oFindNextFileW(h, out);
    }

    BOOL WINAPI hkFindNextFileA(HANDLE h, LPWIN32_FIND_DATAA out)
    {
        if (Listing* l = OurListing(h))
        {
            if (l->next >= l->items.size())
            {
                SetLastError(ERROR_NO_MORE_FILES);
                return FALSE;
            }
            ToAnsi(l->items[l->next++], *out);
            return TRUE;
        }
        return oFindNextFileA(h, out);
    }

    BOOL WINAPI hkFindClose(HANDLE h)
    {
        if (Listing* l = OurListing(h))
        {
            {
                std::lock_guard<std::mutex> lock(g_listLock);
                g_listings.erase(l);
                g_listingCount.fetch_sub(1, std::memory_order_release);
            }
            delete l;
            return TRUE;
        }
        return oFindClose(h);
    }

    // --- Finding the mods ------------------------------------------------------------------------------

    std::wstring IniString(const std::wstring& ini, const wchar_t* key, const std::wstring& def)
    {
        wchar_t buf[512] = {};
        GetPrivateProfileStringW(L"Mod", key, def.c_str(), buf, 512, ini.c_str());
        std::wstring s = buf;
        const size_t a = s.find_first_not_of(L" \t\"");
        const size_t b = s.find_last_not_of(L" \t\"");
        return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
    }

    bool NeverReplaced(const std::wstring& lowerName)
    {
        static const wchar_t* const kExt[] = { L".ini", L".log", L".exe", L".dll", L".asi" };
        for (const wchar_t* e : kExt)
        {
            const size_t n = wcslen(e);
            if (lowerName.size() >= n && lowerName.compare(lowerName.size() - n, n, e) == 0)
                return true;
        }
        return false;
    }

    // Adds every file under <mod>\<rel> (rel = "System" or "Data", then subfolders) to g_files.
    void Walk(size_t modIndex, const std::wstring& base, const std::wstring& rel)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = RealFindFirst((base + rel + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do
        {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
                continue;
            const std::wstring childRel = rel + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                Walk(modIndex, base, childRel);
                continue;
            }
            const std::wstring key = Lower(childRel);
            if (NeverReplaced(key))
            {
                LOG("[MODS] %ls: %ls skipped (.ini, .log, .exe, .dll and .asi files can't be replaced)",
                    g_mods[modIndex].name.c_str(), childRel.c_str());
                continue;
            }
            const auto prev = g_files.find(key);
            if (prev != g_files.end() && prev->second.mod != modIndex)
                LOG("[MODS] %ls: %ls wins over %ls", childRel.c_str(), g_mods[modIndex].name.c_str(),
                    g_mods[prev->second.mod].name.c_str());
            g_files[key] = ModFile{ base + childRel, childRel, name, modIndex };
        } while (RealFindNext(h, &fd));
        RealFindClose(h);
    }

    void FindMods()
    {
        struct Candidate { ModInfo info; std::wstring dir; };
        std::vector<Candidate> found;

        WIN32_FIND_DATAW fd;
        HANDLE h = RealFindFirst((g_modsDir + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do
        {
            const std::wstring folder = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || folder.empty() || folder[0] == L'.' || folder[0] == L'_')
                continue;
            const std::wstring dir = g_modsDir + folder + L"\\";
            const std::wstring ini = dir + L"mod.ini";
            ModInfo m;
            m.folder = folder;
            m.name = IniString(ini, L"Name", folder);
            if (m.name.empty())
                m.name = folder;
            m.author = IniString(ini, L"Author", L"");
            m.version = IniString(ini, L"Version", L"");
            m.description = IniString(ini, L"Description", L"");
            m.priority = GetPrivateProfileIntW(L"Mod", L"Priority", 0, ini.c_str());
            if (GetPrivateProfileIntW(L"Mod", L"Enabled", 1, ini.c_str()) == 0)
            {
                LOG("[MODS] %ls (Mods\\%ls) is switched off (Enabled = 0)", m.name.c_str(), folder.c_str());
                continue;
            }
            found.push_back({ std::move(m), dir });
        } while (RealFindNext(h, &fd));
        RealFindClose(h);

        // Lowest priority first, so later mods overwrite earlier ones' files.
        std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
            if (a.info.priority != b.info.priority)
                return a.info.priority < b.info.priority;
            return _wcsicmp(a.info.folder.c_str(), b.info.folder.c_str()) < 0;
        });
        for (auto& c : found)
        {
            c.info.dir = c.dir;
            g_mods.push_back(c.info);
            const size_t index = g_mods.size() - 1;
            Walk(index, c.dir, L"System");
            Walk(index, c.dir, L"Data");
        }

        for (auto& [key, f] : g_files)
        {
            const std::wstring gamePath = g_root + key;
            if (oGetFileAttributesW(gamePath.c_str()) == INVALID_FILE_ATTRIBUTES)
                ++g_mods[f.mod].added;
            else
                ++g_mods[f.mod].replaced;
            const size_t slash = key.find_last_of(L'\\');
            g_dirs[slash == std::wstring::npos ? std::wstring() : key.substr(0, slash)].push_back(&f);
        }
    }
}

const std::vector<Mods::Info>& Mods::Loaded()
{
    static std::vector<Info> list;
    if (list.size() != g_mods.size())
    {
        list.clear();
        for (const auto& m : g_mods)
            list.push_back({ m.name, m.folder, m.dir });
    }
    return list;
}

const std::wstring& Mods::Dir() { return g_modsDir; }

// A mod folder the level editor just created (with its mod.ini): its level files count from now on, without a restart.
void Mods::AddNew(const std::wstring& folder)
{
    if (g_modsDir.empty() || folder.empty()) return;
    for (const auto& m : g_mods)
        if (_wcsicmp(m.folder.c_str(), folder.c_str()) == 0) return;
    ModInfo m;
    m.folder = folder;
    m.name = folder;
    m.dir = g_modsDir + folder + L"\\";
    g_mods.push_back(m);
    LOG("Mods: %ls (Mods\\%ls) added by the level editor", folder.c_str(), folder.c_str());
}

void Mods::Init()
{
    if (!g_settings.mods)
    {
        LOG("Mods: off ([Mods] Mods = 0)");
        return;
    }
    g_logFiles = g_settings.logModFiles;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\'));            // ...\System
    dir.resize(dir.find_last_of(L'\\') + 1);        // game folder with a trailing backslash
    g_modsDir = dir + L"Mods\\";
    g_root = Lower(dir);

    FindMods();
    if (g_mods.empty())
    {
        LOG("Mods: none (put mods in %lsMods\\<name>\\, see Mods\\README.txt)", dir.c_str());
        return;
    }
    for (const auto& m : g_mods)
        LOG("Mods: %ls%ls%ls%ls%ls (Mods\\%ls, priority %d): %u file(s) replaced, %u added", m.name.c_str(),
            m.version.empty() ? L"" : L" ", m.version.c_str(), m.author.empty() ? L"" : L" by ", m.author.c_str(),
            m.folder.c_str(), m.priority, unsigned(m.replaced), unsigned(m.added));
    if (g_files.empty())
    {
        LOG("Mods: no game files to replace (a mod's files go in its System\\ and Data\\ folders)");
        return;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto hook = [&](const char* fn, void* detour, void** original) {
        return Mem::Detour(fn, reinterpret_cast<void*>(GetProcAddress(k32, fn)), detour, original);
    };
    bool ok = true;
    ok &= hook("CreateFileW", reinterpret_cast<void*>(&hkCreateFileW), reinterpret_cast<void**>(&oCreateFileW));
    ok &= hook("CreateFileA", reinterpret_cast<void*>(&hkCreateFileA), reinterpret_cast<void**>(&oCreateFileA));
    ok &= hook("GetFileAttributesW", reinterpret_cast<void*>(&hkGetFileAttributesW), reinterpret_cast<void**>(&oGetFileAttributesW));
    ok &= hook("GetFileAttributesA", reinterpret_cast<void*>(&hkGetFileAttributesA), reinterpret_cast<void**>(&oGetFileAttributesA));
    ok &= hook("GetFileAttributesExW", reinterpret_cast<void*>(&hkGetFileAttributesExW), reinterpret_cast<void**>(&oGetFileAttributesExW));
    // Listings: the three W functions share handles, so they go in together, and the A ones on top of them.
    ok &= hook("FindClose", reinterpret_cast<void*>(&hkFindClose), reinterpret_cast<void**>(&oFindClose));
    ok &= hook("FindNextFileW", reinterpret_cast<void*>(&hkFindNextFileW), reinterpret_cast<void**>(&oFindNextFileW));
    ok &= hook("FindNextFileA", reinterpret_cast<void*>(&hkFindNextFileA), reinterpret_cast<void**>(&oFindNextFileA));
    ok &= hook("FindFirstFileW", reinterpret_cast<void*>(&hkFindFirstFileW), reinterpret_cast<void**>(&oFindFirstFileW));
    ok &= hook("FindFirstFileA", reinterpret_cast<void*>(&hkFindFirstFileA), reinterpret_cast<void**>(&oFindFirstFileA));
    LOG("Mods: %u game file(s) come from mods%s", unsigned(g_files.size()), ok ? "" : " [!] some file hooks failed");
}
