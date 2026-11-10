// In-game level editor (mod support, phase 6).
//
// EditorKey (default ~, which the game leaves unbound) opens it. While it's open the world is frozen (LevelInfo.Pauser,
// like the pause menu), every game input event is dropped (RawMouse's CauseInputEvent hook asks IsOpen()), the raw
// mouse moves the editor's own cursor (or turns the free camera while the right button is held) instead of Sam's
// camera (RawMouse hands its per-frame delta to AddMouseDelta), and keyboard messages go to ImGui (window.cpp's
// ViewportWndProc hook asks WndProc()). The game keeps warping the Windows cursor to the window centre, so the editor
// draws its own cursor and never reads the Windows one.
//
// Free camera: the engine asks the player controller for the view every frame with the script event PlayerCalcView
// (EPlayerController.PlayerCalcView -> EPlayerCam.CalcView), through UObject::ProcessEvent (0x10CF4A90). That's
// detoured: after the original, the out parameters CameraLocation/CameraRotation are replaced with the editor's
// camera while it's flying, and otherwise remembered (so the free camera starts where the game's camera was).
//
// Picking: a ray from the camera through the cursor (Hor+ FOV as the widescreen code renders it: FOVAngle x
// FOVMultiplier at 4:3, widened for the aspect). Actor.Trace (from Sam) finds what it hits with collision; actors
// without collision are picked by their collision radius around their location. Dragging the selected actor moves it in
// the plane facing the camera (Shift: up/down only), through SetLocation.
//
// Drawing: Dear ImGui (DX9 backend) just before each Present, into the back buffer, inside its own BeginScene/EndScene;
// the backend saves and restores every device state it touches. The backend holds a reference to the device (and owns
// textures and buffers on it), so it's shut down as soon as the game starts making a new device (OnNewDevice, from the
// CreateDevice hook; the game makes one per map change): otherwise the old device and its swap chain on the same window
// stay alive and the next screen never shows. Reset drops and recreates its device objects.
//
// Duplicate and Delete go through Lua (Scripting::EditorCopy / EditorDelete -> the prelude's make_copy, the same copy
// as level files' Begin Actor Copy=, and remove_actor, which takes a guard's gun and gear along).
//
// Saving: every edit is remembered with the value it set (the engine's text for it, at the moment it was set, so a
// guard that walks off afterwards still saves where he was put). Save writes Mods\<folder>\Levels\<map>.txt: when
// that file was applied at this mission's start, its blocks first (Scripting reports them with the actors they made or
// changed), with the editor's changes merged in and deleted actors dropped, then the editor's own additions (Begin
// Actor Copy= the map actor it all started from, with every property set on the way), changes to map actors (Begin
// Change) and deletions. After a save that file's blocks are what the editor merges into next time.
#include "pch.h"
#include "features.h"

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include <map>
#include <set>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr float kPi = 3.14159265358979f;
    constexpr float kRotToRad = 2.0f * kPi / 65536.0f;

    struct V3 { float x = 0, y = 0, z = 0; };
    V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    float Len(V3 a) { return std::sqrt(Dot(a, a)); }
    V3 Norm(V3 a) { const float l = Len(a); return l > 1e-6f ? a * (1.0f / l) : V3{ 1, 0, 0 }; }

    std::atomic<bool> g_open{ false };
    bool g_ready = false;              // ImGui context + backends
    IDirect3DDevice9* g_dev = nullptr; // device the DX9 backend was initialised with
    bool g_keyHeld = false;
    float g_mx = 0, g_my = 0;
    std::mutex g_mouseLock;
    float g_pendX = 0, g_pendY = 0, g_wheel = 0, g_lookX = 0, g_lookY = 0;
    bool g_buttons[3] = {};
    LARGE_INTEGER g_freq{}, g_last{};
    float g_uiScale = 1.0f;

    // Camera (shared with the ProcessEvent hook)
    using ProcessEvent_t = void(__fastcall*)(void* obj, void* edx, void* func, void* parms, void* result);
    ProcessEvent_t oProcessEvent = nullptr;
    std::mutex g_camLock;
    int g_calcViewName = 0, g_offLoc = -1, g_offRot = -1;
    bool g_camActive = false, g_haveGameCam = false;
    V3 g_camPos, g_gamePos;
    float g_camPitch = 0, g_camYaw = 0, g_gamePitch = 0, g_gameYaw = 0;
    bool g_freeCam = true, g_looking = false;
    float g_camSpeed = 800.0f, g_lookSpeed = 1.0f;

    // UI state
    char g_actorFilter[96] = "";
    char g_propFilter[96] = "";
    bool g_hideInternal = true, g_showNames = false, g_snap = false;
    float g_grid = 16.0f;
    void* g_sel = nullptr;
    struct Row { void* actor; std::string name, cls; float dist; };
    std::vector<Row> g_rows;
    DWORD g_rowsTick = 0;
    std::vector<Engine::PropRow> g_props;
    void* g_propsOf = nullptr;
    struct Cell { std::string value; char edit[1024]; bool editing = false; std::string error; };
    std::unordered_map<uint64_t, Cell> g_cells;   // (prop, index) -> value shown / being edited
    DWORD g_cellsTick = 0;
    std::string g_status;
    bool g_dragging = false, g_dragVertical = false;
    V3 g_dragOffset;

    // Changes for saving: per actor, the properties set (key, with "(index)" for arrays -> value text when set)
    using Props = std::vector<std::pair<std::string, std::string>>;
    struct Change
    {
        bool added = false;        // made in the editor (Duplicate)
        bool unsaved = false;      // can't go in a level file (a copy of something that isn't part of the map)
        std::string copy, cls;     // added: Copy= this map actor, or Class= this class
        std::string label;         // added: its Name= in the level file, once saved
        Props props;
        uint32_t order = 0;        // order of the first edit
    };
    std::unordered_map<void*, Change> g_changes;
    uint32_t g_changeOrder = 0;
    std::vector<std::string> g_deleted;   // map actors deleted, by name
    struct FileRecord { std::string folder; std::vector<Editor::LevelBlock> blocks; };
    std::map<std::string, FileRecord> g_records;   // folder (lower case) -> what its level file did at this mission's start
    bool g_filesApplied = false;                   // this mission started with the level files applied (not a savegame)
    char g_saveFolder[64] = "MyLevel";
    bool g_confirmOverwrite = false;
    bool g_freeze = true;                          // freeze the world while the editor is open
    ULONGLONG g_refreezeAt = 0;                    // a duplicate briefly unfreezes the world so it takes its pose

    uint64_t CellKey(void* prop, int index) { return (uint64_t(reinterpret_cast<uintptr_t>(prop)) << 16) | uint32_t(index & 0xFFFF); }

    // ----- the free camera's hook -----

    void __fastcall hkProcessEvent(void* obj, void* edx, void* func, void* parms, void* result)
    {
        oProcessEvent(obj, edx, func, parms, result);
        if (!g_calcViewName || !func || !parms) return;
        if (*reinterpret_cast<const int*>(static_cast<const uint8_t*>(func) + 0x20) != g_calcViewName) return;
        if (g_offLoc < 0 || g_offRot < 0)
        {
            g_offLoc = Engine::ParamOffset(func, "CameraLocation");
            g_offRot = Engine::ParamOffset(func, "CameraRotation");
            if (g_offLoc < 0 || g_offRot < 0) { g_calcViewName = 0; LOG("Editor: [!] PlayerCalcView has no camera parameters"); return; }
        }
        float* loc = reinterpret_cast<float*>(static_cast<uint8_t*>(parms) + g_offLoc);
        int* rot = reinterpret_cast<int*>(static_cast<uint8_t*>(parms) + g_offRot);
        std::lock_guard<std::mutex> lock(g_camLock);
        if (g_camActive)
        {
            loc[0] = g_camPos.x; loc[1] = g_camPos.y; loc[2] = g_camPos.z;
            rot[0] = int(g_camPitch); rot[1] = int(g_camYaw); rot[2] = 0;
        }
        else
        {
            g_gamePos = { loc[0], loc[1], loc[2] };
            g_gamePitch = float(int16_t(rot[0] & 0xFFFF));
            g_gameYaw = float(rot[1]);
            g_haveGameCam = true;
        }
    }

    // ----- camera maths -----

    struct View { V3 pos, fwd, right, up; float tanH = 1, tanV = 1, w = 1, h = 1; };

    View CurrentView()
    {
        View v;
        float pitch, yaw;
        {
            std::lock_guard<std::mutex> lock(g_camLock);
            v.pos = g_camActive ? g_camPos : g_gamePos;
            pitch = g_camActive ? g_camPitch : g_gamePitch;
            yaw = g_camActive ? g_camYaw : g_gameYaw;
        }
        const float p = pitch * kRotToRad, y = yaw * kRotToRad;
        v.fwd = { std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p) };
        v.right = { -std::sin(y), std::cos(y), 0 };
        v.up = { -std::sin(p) * std::cos(y), -std::sin(p) * std::sin(y), std::cos(p) };
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        v.w = ds.x; v.h = ds.y;
        void* ctrl = Engine::GetObject(Engine::Player(), "Controller");
        float fov = std::clamp(Engine::GetFloat(ctrl, "FOVAngle", 90.0f) * g_settings.fovMultiplier, 1.0f, 170.0f);
        v.tanV = std::tan(fov * 0.5f * kPi / 180.0f) * 0.75f;
        v.tanH = v.tanV * (v.w / std::max(1.0f, v.h));
        return v;
    }

    bool Project(const View& v, V3 p, ImVec2& out)
    {
        const V3 d = p - v.pos;
        const float z = Dot(d, v.fwd);
        if (z < 5.0f) return false;
        out.x = v.w * 0.5f + Dot(d, v.right) / (z * v.tanH) * v.w * 0.5f;
        out.y = v.h * 0.5f - Dot(d, v.up) / (z * v.tanV) * v.h * 0.5f;
        return true;
    }

    V3 Ray(const View& v, float mx, float my)
    {
        const float nx = (mx - v.w * 0.5f) / (v.w * 0.5f) * v.tanH;
        const float ny = -(my - v.h * 0.5f) / (v.h * 0.5f) * v.tanV;
        return Norm(v.fwd + v.right * nx + v.up * ny);
    }

    V3 ActorLoc(void* a) { float l[3] = {}; Engine::Location(a, l); return { l[0], l[1], l[2] }; }

    // ----- setup -----

    void ApplyScale()
    {
        ImGuiStyle style;
        ImGui::StyleColorsDark(&style);
        style.ScaleAllSizes(g_uiScale);
        style.FontScaleMain = g_uiScale;
        style.WindowRounding = 4.0f * g_uiScale;
        style.Colors[ImGuiCol_WindowBg].w = 0.92f;
        ImGui::GetStyle() = style;
    }

    void Shutdown()
    {
        if (!g_ready) return;
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ready = false;
        g_dev = nullptr;
    }

    bool Setup(IDirect3DDevice9* dev)
    {
        if (g_ready && g_dev == dev) return true;
        Shutdown();
        if (!Game::hWnd) return false;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        Victory::AddFonts();               // the default font first (the editor's), then the ceremony's
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;          // no imgui.ini next to the game
        io.MouseDrawCursor = true;         // our own cursor: the game keeps warping the Windows one
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        RECT rc;
        GetClientRect(Game::hWnd, &rc);
        g_uiScale = g_settings.editorScale > 0 ? std::clamp(g_settings.editorScale, 0.5f, 4.0f)
                                               : std::clamp(float(rc.bottom - rc.top) / 900.0f, 1.0f, 3.0f);
        ApplyScale();
        if (!ImGui_ImplWin32_Init(Game::hWnd) || !ImGui_ImplDX9_Init(dev))
        {
            LOG("Editor: [!] couldn't start the overlay");
            ImGui::DestroyContext();
            return false;
        }
        g_ready = true;
        g_dev = dev;
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        return true;
    }

    void SetOpen(bool open)
    {
        if (open == g_open.load()) return;
        if (open)
        {
            RECT rc;
            GetClientRect(Game::hWnd, &rc);
            g_mx = float(rc.right - rc.left) * 0.5f;
            g_my = float(rc.bottom - rc.top) * 0.5f;
            if (g_freeze) Engine::SetPause(true);
            g_refreezeAt = 0;
            g_rowsTick = 0;
            {
                std::lock_guard<std::mutex> lock(g_camLock);
                if (g_haveGameCam) { g_camPos = g_gamePos; g_camPitch = g_gamePitch; g_camYaw = g_gameYaw; }
                g_camActive = g_freeCam && g_haveGameCam;
            }
            LOG("Editor: open%s", g_camActive ? " (free camera)" : "");
        }
        else
        {
            Engine::SetPause(false);
            g_refreezeAt = 0;
            for (bool& b : g_buttons) b = false;
            std::lock_guard<std::mutex> lock(g_camLock);
            g_camActive = false;
            g_looking = g_dragging = false;
            LOG("Editor: closed");
        }
        g_open = open;
    }

    // ----- changes -----

    std::string Lower(std::string v) { for (auto& c : v) c = char(tolower(static_cast<unsigned char>(c))); return v; }
    std::string Trim(const std::string& v)
    {
        const size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    }
    std::wstring Wide(const std::string& v)
    {
        if (v.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, v.data(), int(v.size()), nullptr, 0);
        std::wstring w(size_t(std::max(n, 0)), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, v.data(), int(v.size()), w.data(), n);
        return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string v(size_t(std::max(n, 0)), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), v.data(), n, nullptr, nullptr);
        return v;
    }
    bool IsLocRot(const std::string& k) { return _stricmp(k.c_str(), "Location") == 0 || _stricmp(k.c_str(), "Rotation") == 0; }
    void SetProp(Props& p, const std::string& key, const std::string& value)
    {
        for (auto& kv : p) if (_stricmp(kv.first.c_str(), key.c_str()) == 0) { kv.second = value; return; }
        p.emplace_back(key, value);
    }

    // An edit that worked: remember the value it set.
    void NoteChange(void* actor, void* prop, int index, const std::string& key)
    {
        std::string v;
        if (!actor || !prop || !Engine::Export(actor, prop, index, v)) return;
        Change& c = g_changes[actor];
        if (!c.order) c.order = ++g_changeOrder;
        SetProp(c.props, key, v);
    }
    void NoteKey(void* actor, const char* name)
    {
        std::vector<Engine::PropRow> rows;
        Engine::Properties(actor, rows);
        for (const auto& r : rows)
            if (_stricmp(r.name.c_str(), name) == 0) { NoteChange(actor, r.prop, 0, r.name); return; }
    }

    // The level-file block (any mod folder) that made or changed this actor.
    const Editor::LevelBlock* RecordOf(void* obj, const char* kind, const FileRecord** file = nullptr)
    {
        if (!obj) return nullptr;
        for (const auto& [k, f] : g_records)
            for (const auto& b : f.blocks)
                if (b.obj == obj && !b.gone && b.kind == kind) { if (file) *file = &f; return &b; }
        return nullptr;
    }

    size_t ChangeCount()
    {
        size_t n = g_changes.size() + g_deleted.size();
        for (const auto& [k, f] : g_records)
            for (const auto& b : f.blocks) if (b.gone) ++n;
        return n;
    }

    // What a copy of 'src' needs in a level file: Copy= the map actor it all started from (Class= for one a level file
    // made from a class), and every property set on the way - on 'src' and on what 'src' itself was copied from. (A
    // level file makes all its copies before setting any properties, so copying a copy wouldn't carry those over.)
    void Inherit(void* src, Change& c)
    {
        auto addAll = [&](const Props& p) { for (const auto& kv : p) if (!IsLocRot(kv.first)) SetProp(c.props, kv.first, kv.second); };
        const auto it = g_changes.find(src);
        if (it != g_changes.end() && it->second.added)
        {
            c.copy = it->second.copy;
            c.cls = it->second.cls;
            c.unsaved = it->second.unsaved;
            addAll(it->second.props);
            return;
        }
        const FileRecord* file = nullptr;
        if (const Editor::LevelBlock* b = RecordOf(src, "actor", &file))
        {
            std::vector<const Editor::LevelBlock*> path{ b };
            for (int guard = 0; guard < 32 && !path.back()->copy.empty(); ++guard)
            {
                const Editor::LevelBlock* up = nullptr;
                for (const auto& o : file->blocks)
                    if (o.kind == "actor" && !o.label.empty() && _stricmp(o.label.c_str(), path.back()->copy.c_str()) == 0) { up = &o; break; }
                if (!up || up == path.back()) break;
                path.push_back(up);
            }
            if (!path.back()->copy.empty()) c.copy = path.back()->copy; else c.cls = path.back()->cls;
            for (auto p = path.rbegin(); p != path.rend(); ++p) addAll((*p)->props);
        }
        else
        {
            c.copy = Engine::Name(src);
            c.unsaved = !Engine::FromMap(src);
            for (const auto& [k, f] : g_records)
                for (const auto& o : f.blocks) if (o.obj == src && !o.gone && o.kind == "change") addAll(o.props);
        }
        if (it != g_changes.end()) addAll(it->second.props);
    }

    std::string NewLabel(const std::string& base, const std::set<std::string>& used)
    {
        std::string b;
        for (char ch : base) b += (isalnum(static_cast<unsigned char>(ch)) || ch == '_') ? ch : '_';
        if (b.empty()) b = "Actor";
        for (int n = 1;; ++n)
        {
            std::string l = b + "_Copy" + std::to_string(n);
            if (!used.count(Lower(l))) return l;
        }
    }

    // Writes Mods\<folder>\Levels\<map>.txt (see the top of the file). force: overwrite a file that wasn't applied at
    // this mission's start (a savegame was loaded), which would lose what it did.
    void Save(bool force)
    {
        g_confirmOverwrite = false;
        const std::string folder = Trim(g_saveFolder);
        const std::string map = Scripting::MapName();
        if (Mods::Dir().empty()) { g_status = "Save: mods are off ([Mods] Mods = 0 in SCCTRemaster.ini)"; return; }
        if (map.empty() || _stricmp(map.c_str(), "menu") == 0) { g_status = "Save: start a mission first"; return; }
        if (folder.empty() || folder[0] == '_' || folder[0] == '.' || folder.find_first_of("\\/:*?\"<>|") != std::string::npos)
        {
            g_status = "Save: the mod folder needs a plain name (not starting with _ or .)";
            return;
        }
        const std::wstring dir = Mods::Dir() + Wide(folder) + L"\\";
        const std::wstring path = dir + L"Levels\\" + Wide(map) + L".txt";
        const std::string key = Lower(folder);
        auto rit = g_records.find(key);
        const bool have = rit != g_records.end();
        if (!have && !force && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            g_confirmOverwrite = true;
            g_status = "Save: Mods\\" + folder + "\\Levels\\" + map + ".txt wasn't applied this time (a savegame?)";
            return;
        }

        std::vector<Editor::LevelBlock> out;
        std::set<std::string> deletes, labels;
        int skipped = 0;
        std::set<void*> keep;   // changes this file can't hold stay for another save
        if (have)
            for (const auto& b : rit->second.blocks) if (b.kind == "actor" && !b.label.empty()) labels.insert(Lower(b.label));
        auto overlay = [&](Editor::LevelBlock& nb) {
            const auto it = nb.obj ? g_changes.find(nb.obj) : g_changes.end();
            if (it != g_changes.end()) for (const auto& [k, v] : it->second.props) SetProp(nb.props, k, v);
        };
        std::set<void*> merged;
        if (have)
            for (const auto& b : rit->second.blocks)
            {
                Editor::LevelBlock nb = b;
                if (b.kind == "actor")
                {
                    if (b.gone) continue;   // deleted in the editor
                    overlay(nb);
                }
                else if (b.kind == "change")
                {
                    if (b.gone)
                    {
                        if (labels.count(Lower(b.name))) continue;   // it changed an actor this file added, now gone
                        if (!deletes.insert(Lower(b.name)).second) continue;
                        nb = Editor::LevelBlock{};
                        nb.kind = "delete";
                        nb.name = b.name;
                        nb.head = "Delete Name=" + b.name;
                    }
                    else overlay(nb);
                }
                else if (b.kind == "delete" && !deletes.insert(Lower(b.name)).second) continue;
                if (nb.obj) merged.insert(nb.obj);
                out.push_back(std::move(nb));
            }
        std::vector<std::pair<uint32_t, void*>> order;
        for (const auto& [obj, c] : g_changes) order.push_back({ c.order, obj });
        std::sort(order.begin(), order.end());
        for (const auto& [o, obj] : order)
        {
            Change& c = g_changes[obj];
            if (merged.count(obj) || !Engine::Alive(obj)) continue;
            Editor::LevelBlock nb;
            nb.obj = obj;
            nb.props = c.props;
            if (c.added)
            {
                if (c.unsaved || (c.copy.empty() && c.cls.empty())) { ++skipped; keep.insert(obj); continue; }
                if (c.label.empty()) c.label = NewLabel(c.copy.empty() ? c.cls : c.copy, labels);
                labels.insert(Lower(c.label));
                nb.kind = "actor";
                nb.copy = c.copy;
                nb.cls = c.cls;
                nb.label = c.label;
                nb.head = "Begin Actor " + (c.copy.empty() ? "Class=" + c.cls : "Copy=" + c.copy) + " Name=" + c.label;
            }
            else
            {
                if (!Engine::FromMap(obj)) { ++skipped; keep.insert(obj); continue; }   // made while playing: no name to find it by
                nb.kind = "change";
                nb.name = Engine::Name(obj);
                nb.head = "Begin Change Name=" + nb.name;
            }
            out.push_back(std::move(nb));
        }
        for (const auto& n : g_deleted)
            if (deletes.insert(Lower(n)).second)
            {
                Editor::LevelBlock nb;
                nb.kind = "delete";
                nb.name = n;
                nb.head = "Delete Name=" + n;
                out.push_back(std::move(nb));
            }

        std::string text = "; " + map + " for the mod in Mods\\" + folder + ", saved by the SCCT Remaster level editor.\r\n"
                           "; Saving from the editor rewrites this file (comments aren't kept). See Mods\\LEVELS.txt.\r\n";
        for (const auto& b : out)
        {
            text += "\r\n" + b.head + "\r\n";
            if (b.kind == "delete") continue;
            for (const auto& [k, v] : b.props) text += "  " + k + "=" + v + "\r\n";
            text += "End\r\n";
        }

        CreateDirectoryW(Mods::Dir().c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        CreateDirectoryW((dir + L"Levels").c_str(), nullptr);
        const std::wstring ini = dir + L"mod.ini";
        const bool newMod = GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES;
        if (newMod)
            if (FILE* f = _wfopen(ini.c_str(), L"wb"))
            {
                const std::string t = "[Mod]\r\nName=" + folder + "\r\nAuthor=\r\nVersion=1.0\r\nDescription=Made with the in-game level editor.\r\n"
                                      "Enabled=1\r\nPriority=0\r\n";
                fwrite(t.data(), 1, t.size(), f);
                fclose(f);
            }
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) { g_status = "Save: couldn't write Mods\\" + folder + "\\Levels\\" + map + ".txt"; LOG("Editor: [!] couldn't write %ls", path.c_str()); return; }
        const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        fclose(f);
        if (!ok) { g_status = "Save: writing the file failed (disk full?)"; return; }

        // That file now holds everything: it's what the editor merges into next time.
        for (auto& b : out) b.gone = false;
        FileRecord& fr = g_records[key];
        fr.folder = folder;
        fr.blocks = std::move(out);
        for (auto it = g_changes.begin(); it != g_changes.end();)
            it = keep.count(it->first) ? std::next(it) : g_changes.erase(it);
        g_deleted.clear();
        if (newMod) Mods::AddNew(Wide(folder));
        if (_wcsicmp(g_settings.editorFolder.c_str(), Wide(folder).c_str()) != 0)
        {
            g_settings.editorFolder = Wide(folder);
            Config::Save(L"Editor", L"SaveFolder", g_settings.editorFolder.c_str());
        }
        g_status = "Saved Mods\\" + folder + "\\Levels\\" + map + ".txt (" + std::to_string(fr.blocks.size()) + " block(s))";
        if (skipped) g_status += "; " + std::to_string(skipped) + " change(s) to things made while playing can't be saved";
        if (!g_settings.levelChanges) g_status += "; level changes are off ([Mods] LevelChanges = 0)";
        LOG("Editor: saved %ls (%d block(s), %d skipped)%s", path.c_str(), int(fr.blocks.size()), skipped, newMod ? ", new mod folder" : "");
    }

    // ----- actions -----

    void SetLocation(void* a, V3 p)
    {
        if (g_snap && g_grid > 0)
        {
            p.x = std::round(p.x / g_grid) * g_grid;
            p.y = std::round(p.y / g_grid) * g_grid;
            p.z = std::round(p.z / g_grid) * g_grid;
        }
        static std::vector<Engine::PropRow> props;
        static void* propsOf = nullptr;
        static void* locProp = nullptr;
        if (propsOf != a)
        {
            Engine::Properties(a, props);
            propsOf = a;
            locProp = nullptr;
            for (const auto& r : props) if (_stricmp(r.name.c_str(), "Location") == 0) { locProp = r.prop; break; }
        }
        if (!locProp) return;
        char text[128];
        snprintf(text, sizeof(text), "(X=%.2f,Y=%.2f,Z=%.2f)", p.x, p.y, p.z);
        std::string err;
        if (Engine::Import(a, locProp, 0, text, err)) NoteChange(a, locProp, 0, "Location");
        else g_status = "Move: " + err;
    }

    void Duplicate()
    {
        if (!g_sel || !Engine::Alive(g_sel)) return;
        const View v = CurrentView();
        const float r = std::clamp(Engine::GetFloat(g_sel, "CollisionRadius", 40.0f), 20.0f, 400.0f);
        const V3 at = ActorLoc(g_sel) + v.right * (r * 2.0f + 30.0f);
        const float loc[3] = { at.x, at.y, at.z };
        std::string err;
        const std::string from = Engine::Name(g_sel);
        void* copy = Scripting::EditorCopy(g_sel, loc, err);
        if (!copy) { g_status = "Duplicate failed: " + (err.empty() ? std::string("the game refused (blocked?)") : err); return; }
        Change c;
        c.added = true;
        Inherit(g_sel, c);
        c.order = ++g_changeOrder;
        g_changes[copy] = std::move(c);
        NoteKey(copy, "Location");
        NoteKey(copy, "Rotation");
        g_sel = copy;
        g_rowsTick = 0;
        g_status = "Duplicated " + from + " -> " + Engine::Name(copy);
        if (g_changes[copy].unsaved) g_status += " (can't be saved: " + from + " isn't part of the map)";
        // a character copied while the world is frozen would stand in its bind pose until it unfreezes: let the world
        // run for a moment so it takes its pose
        if (g_freeze && Engine::GetObject(copy, "Controller"))
        {
            Engine::SetPause(false);
            g_refreezeAt = GetTickCount64() + 200;
        }
    }

    void Delete()
    {
        if (!g_sel || !Engine::Alive(g_sel)) return;
        if (g_sel == Engine::Player()) { g_status = "That's Sam: he stays"; return; }
        const std::string name = Engine::Name(g_sel);
        const bool fromMap = Engine::FromMap(g_sel);
        std::string err;
        void* victim = g_sel;
        int belongings = 0;
        if (!Scripting::EditorDelete(victim, err, belongings)) { g_status = "Delete failed: " + err; return; }
        bool recorded = false;
        for (auto& [k, f] : g_records)
            for (auto& b : f.blocks)
                if (b.obj == victim && !b.gone) { b.gone = true; b.obj = nullptr; recorded = recorded || b.kind == "actor"; }
        auto it = g_changes.find(victim);
        const bool added = it != g_changes.end() && it->second.added;
        if (it != g_changes.end()) g_changes.erase(it);
        bool saved = true;
        if (!added && !recorded)
        {
            if (fromMap) g_deleted.push_back(name);
            else saved = false;
        }
        g_sel = nullptr;
        g_rowsTick = 0;
        g_propsOf = nullptr;
        g_status = "Deleted " + name;
        if (belongings > 0) g_status += " and " + std::to_string(belongings) + " thing(s) it carried";
        if (!saved) g_status += " (not saved: it wasn't part of the map)";
    }

    void Focus()
    {
        if (!g_sel || !Engine::Alive(g_sel)) return;
        const View v = CurrentView();
        const float r = std::clamp(Engine::GetFloat(g_sel, "CollisionRadius", 40.0f), 20.0f, 2000.0f);
        std::lock_guard<std::mutex> lock(g_camLock);
        g_camPos = ActorLoc(g_sel) - v.fwd * std::max(250.0f, r * 5.0f);
    }

    void* Pick(const View& v, float mx, float my)
    {
        const V3 dir = Ray(v, mx, my);
        const V3 end = v.pos + dir * 60000.0f;
        float hit[3] = {};
        const float s[3] = { v.pos.x, v.pos.y, v.pos.z }, e[3] = { end.x, end.y, end.z };
        void* pawn = Engine::Player();
        void* traced = pawn ? Engine::Trace(pawn, s, e, hit) : nullptr;
        float best = 1e30f;
        if (traced)
        {
            best = Len(V3{ hit[0], hit[1], hit[2] } - v.pos);
            const std::string cls = Engine::ClassName(traced);
            if (cls.find("LevelInfo") != std::string::npos) traced = nullptr;   // level geometry: nothing to select
        }
        // actors without collision: by their radius around their location, if in front of what the trace hit
        void* sphere = nullptr;
        float sphereT = best;
        std::vector<void*> actors;
        Engine::Actors(actors);
        for (void* a : actors)
        {
            if (a == pawn || a == traced || Engine::GetBool(a, "bHidden")) continue;   // hidden helpers: from the list only
            const V3 l = ActorLoc(a);
            const float t = Dot(l - v.pos, dir);
            if (t <= 10.0f || t > sphereT) continue;
            const float r = std::clamp(Engine::GetFloat(a, "CollisionRadius", 30.0f), 16.0f, 300.0f);
            if (Len(l - (v.pos + dir * t)) > r) continue;
            sphere = a;
            sphereT = t;
        }
        return sphere ? sphere : traced;
    }

    // ----- panels -----

    bool Contains(const std::string& hay, const char* needle)
    {
        if (!needle[0]) return true;
        std::string h = hay, n = needle;
        for (auto& c : h) c = char(tolower(static_cast<unsigned char>(c)));
        for (auto& c : n) c = char(tolower(static_cast<unsigned char>(c)));
        return h.find(n) != std::string::npos;
    }

    void RefreshRows()
    {
        std::vector<void*> actors;
        Engine::Actors(actors);
        const V3 from = CurrentView().pos;
        g_rows.clear();
        for (void* a : actors)
            g_rows.push_back({ a, Engine::Name(a), Engine::ClassName(a), Len(ActorLoc(a) - from) / 100.0f });
        std::sort(g_rows.begin(), g_rows.end(), [](const Row& a, const Row& b) { return a.dist < b.dist; });
        g_rowsTick = GetTickCount();
    }

    void SelectionBar()
    {
        if (g_sel && !Engine::Alive(g_sel)) g_sel = nullptr;
        if (!g_sel) { ImGui::TextDisabled("Nothing selected: click something in the world, or pick it below."); return; }
        ImGui::Text("Selected: %s", Engine::Name(g_sel).c_str());
        if (ImGui::Button("Focus (F)")) Focus();
        ImGui::SameLine();
        if (ImGui::Button("Duplicate (Ctrl+D)")) Duplicate();
        ImGui::SameLine();
        if (ImGui::Button("Delete (Del)")) Delete();
    }

    void ActorsPanel()
    {
        SelectionBar();
        ImGui::Separator();
        if (!g_rowsTick || GetTickCount() - g_rowsTick > 1500) RefreshRows();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##actorfilter", "filter by name or class", g_actorFilter, sizeof(g_actorFilter));
        std::vector<const Row*> shown;
        for (const auto& r : g_rows)
            if (Contains(r.name, g_actorFilter) || Contains(r.cls, g_actorFilter)) shown.push_back(&r);
        ImGui::TextDisabled("%d of %d actors, nearest to the camera first", int(shown.size()), int(g_rows.size()));
        if (ImGui::BeginTable("actors", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                              ImVec2(0, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000.0").x);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(shown.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
                {
                    const Row& r = *shown[size_t(i)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(r.actor);
                    if (ImGui::Selectable(r.name.c_str(), g_sel == r.actor, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        g_sel = r.actor;
                        if (ImGui::IsMouseDoubleClicked(0)) Focus();
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.cls.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", r.dist);
                }
            ImGui::EndTable();
        }
    }

    void Refresh(Cell& c, void* prop, int index)
    {
        if (c.editing) return;
        Engine::Export(g_sel, prop, index, c.value);
        strncpy_s(c.edit, c.value.c_str(), _TRUNCATE);
    }

    void PropertyRow(const Engine::PropRow& p, int index)
    {
        Cell& c = g_cells[CellKey(p.prop, index)];
        if (c.value.empty() && !c.editing) Refresh(c, p.prop, index);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (p.dim > 1) ImGui::Text("%s[%d]", p.name.c_str(), index); else ImGui::TextUnformatted(p.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s, declared in %s%s", p.type.c_str(), p.name.c_str(), p.owner.c_str(),
                                                     p.editable ? "" : " (not shown in UnrealEd)");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushID(int(CellKey(p.prop, index) & 0x7FFFFFFF));
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | (p.internal ? ImGuiInputTextFlags_ReadOnly : 0);
        if (ImGui::InputText("##v", c.edit, sizeof(c.edit), flags))
        {
            std::string err;
            if (Engine::Import(g_sel, p.prop, index, c.edit, err))
            {
                c.error.clear();
                g_status = "Set " + p.name + " = " + c.edit;
                NoteChange(g_sel, p.prop, index, p.dim > 1 ? p.name + "(" + std::to_string(index) + ")" : p.name);
            }
            else
            {
                c.error = err;
                g_status = p.name + ": " + err;
            }
            c.editing = false;
            Refresh(c, p.prop, index);
        }
        c.editing = ImGui::IsItemActive();
        if (!c.error.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "!");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.error.c_str());
        }
        ImGui::PopID();
    }

    void InspectorPanel()
    {
        if (g_sel && !Engine::Alive(g_sel)) g_sel = nullptr;
        if (!g_sel)
        {
            ImGui::TextDisabled("Select an actor: click it in the world, or pick it in the Actors list.");
            return;
        }
        if (g_propsOf != g_sel)
        {
            Engine::Properties(g_sel, g_props);
            g_propsOf = g_sel;
            g_cells.clear();
        }
        if (GetTickCount() - g_cellsTick > 500)
        {
            for (auto& [k, c] : g_cells) if (!c.editing) c.value.clear();
            g_cellsTick = GetTickCount();
        }
        ImGui::TextWrapped("%s", Engine::FullName(g_sel).c_str());
        const V3 l = ActorLoc(g_sel);
        ImGui::TextDisabled("at (%.0f, %.0f, %.0f)", l.x, l.y, l.z);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        ImGui::InputTextWithHint("##propfilter", "filter properties", g_propFilter, sizeof(g_propFilter));
        ImGui::SameLine();
        ImGui::Checkbox("hide engine internals", &g_hideInternal);
        if (ImGui::BeginTable("props", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                              ImVec2(0, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Value (Enter applies)", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableHeadersRow();
            std::string group;
            bool groupOpen = true;
            for (const auto& p : g_props)
            {
                if (g_hideInternal && p.internal) continue;
                if (!Contains(p.name, g_propFilter)) continue;
                if (p.owner != group)
                {
                    group = p.owner;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    groupOpen = ImGui::TreeNodeEx(group.c_str(), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  (g_propFilter[0] ? ImGuiTreeNodeFlags_DefaultOpen : 0));
                    ImGui::TableNextColumn();
                }
                if (!groupOpen) continue;
                for (int i = 0; i < p.dim; ++i) PropertyRow(p, i);
            }
            ImGui::EndTable();
        }
    }

    void ToolsPanel()
    {
        ImGui::SeparatorText("Camera");
        {
            bool fc = g_freeCam;
            if (ImGui::Checkbox("Free camera", &fc))
            {
                g_freeCam = fc;
                std::lock_guard<std::mutex> lock(g_camLock);
                if (fc && g_haveGameCam && !g_camActive) { g_camPos = g_gamePos; g_camPitch = g_gamePitch; g_camYaw = g_gameYaw; }
                g_camActive = fc && g_haveGameCam;
            }
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::SliderFloat("Fly speed", &g_camSpeed, 50.0f, 6000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::SliderFloat("Look speed", &g_lookSpeed, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SeparatorText("Placing");
        ImGui::Checkbox("Snap to grid", &g_snap);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        ImGui::InputFloat("##grid", &g_grid, 8.0f, 32.0f, "%.0f units");
        ImGui::Checkbox("Show actor names", &g_showNames);
        if (ImGui::Checkbox("Freeze the world", &g_freeze))
        {
            g_refreezeAt = 0;
            Engine::SetPause(g_freeze);
            g_settings.editorPause = g_freeze;
            Config::Save(L"Editor", L"EditorPause", g_freeze ? L"1" : L"0");
        }
        ImGui::SeparatorText("Saving");
        const std::string map = Scripting::MapName();
        ImGui::Text("%d unsaved change(s)", int(ChangeCount()));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::InputText("Mod folder", g_saveFolder, sizeof(g_saveFolder))) g_confirmOverwrite = false;
        const std::string folder = Trim(g_saveFolder);
        ImGui::TextDisabled("Saves to Mods\\%s\\Levels\\%s.txt", folder.c_str(), map.c_str());
        if (ImGui::Button("Save (Ctrl+S)")) Save(false);
        if (g_confirmOverwrite)
        {
            ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "That file exists, but this mission didn't start with it applied");
            ImGui::TextWrapped("(a savegame was loaded, so the editor can't tell what it added). Saving now replaces it with "
                               "only this session's changes. To keep it, start the mission from the menu and edit then.");
            if (ImGui::Button("Replace it anyway")) Save(true);
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) g_confirmOverwrite = false;
        }
        if (!g_records.empty())
        {
            std::string applied;
            for (const auto& [k, f] : g_records) applied += (applied.empty() ? "" : ", ") + f.folder;
            ImGui::TextDisabled("Level files applied this mission: %s", applied.c_str());
        }
        else if (!g_filesApplied)
            ImGui::TextDisabled("This mission came from a savegame: level files weren't applied.");
        ImGui::SeparatorText("Mod tools");
        if (ImGui::Button("Reload Lua scripts")) { Scripting::Reload(); g_status = "Scripts reloaded (see the log)"; }
        bool dump = Textures::Dumping();
        if (ImGui::Checkbox("Dump textures to Mods\\_TextureDump", &dump)) Textures::SetDumping(dump);
        if (ImGui::Button("Write the actor list")) { Scripting::DumpLevelNow(); g_status = "Actor list written to Mods\\_LevelDump"; }
        ImGui::SeparatorText("CLUSTERFUCK");
        const int won = Clusterfuck::Victories();
        if (ImGui::Button(won ? "Watch your victory again" : "Preview the victory ceremony"))
        {
            SetOpen(false);
            Clusterfuck::PreviewVictory();
        }
        ImGui::SameLine();
        if (ImGui::Button("Preview a mission toast")) Clusterfuck::PreviewToast();
        if (won) ImGui::TextColored(ImVec4(1, 0.82f, 0.3f, 1), "CLUSTERFUCK beaten %d time(s)!", won);
        else ImGui::TextDisabled("Nobody has beaten CLUSTERFUCK here yet (the preview writes a sample certificate to System).");
        ImGui::SeparatorText("Editor");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::SliderFloat("UI size", &g_uiScale, 0.75f, 3.0f, "%.2f")) {}
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            ApplyScale();
            wchar_t buf[32];
            swprintf(buf, 32, L"%.2f", g_uiScale);
            Config::Save(L"Editor", L"UIScale", buf);
        }
        ImGui::SeparatorText("Controls");
        ImGui::TextWrapped("Right mouse + WASD: fly (Q/E down/up, Shift faster, wheel: speed). Click: select. Drag the "
                           "selected actor: move it (Shift: up/down). F: focus. Ctrl+D: duplicate. Del: delete. Ctrl+S: save. "
                           "~: close.");
    }

    // ----- the 3D view: input and drawing -----

    void DrawBox(ImDrawList* dl, const View& v, V3 c, V3 e, ImU32 col)
    {
        V3 p[8];
        for (int i = 0; i < 8; ++i) p[i] = c + V3{ (i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y, (i & 4) ? e.z : -e.z };
        static const int edges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
        for (const auto& ed : edges)
        {
            ImVec2 a, b;
            if (Project(v, p[ed[0]], a) && Project(v, p[ed[1]], b)) dl->AddLine(a, b, col, 2.0f);
        }
    }

    void DrawWorld(const View& v)
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (g_showNames)
        {
            std::vector<void*> actors;
            Engine::Actors(actors);
            struct L { float d; ImVec2 s; void* a; };
            std::vector<L> labels;
            for (void* a : actors)
            {
                const V3 l = ActorLoc(a);
                const float d = Len(l - v.pos);
                ImVec2 s;
                if (d < 3000.0f && Project(v, l, s)) labels.push_back({ d, s, a });
            }
            std::sort(labels.begin(), labels.end(), [](const L& x, const L& y) { return x.d < y.d; });
            if (labels.size() > 150) labels.resize(150);
            for (const auto& l : labels)
            {
                dl->AddCircleFilled(l.s, 3.0f * g_uiScale, IM_COL32(120, 200, 255, 200));
                dl->AddText(ImVec2(l.s.x + 5, l.s.y - 7), IM_COL32(200, 230, 255, 220), Engine::Name(l.a).c_str());
            }
        }
        if (g_sel && Engine::Alive(g_sel))
        {
            const V3 c = ActorLoc(g_sel);
            const float r = std::clamp(Engine::GetFloat(g_sel, "CollisionRadius", 24.0f), 8.0f, 2000.0f);
            const float h = std::clamp(Engine::GetFloat(g_sel, "CollisionHeight", 24.0f), 8.0f, 2000.0f);
            DrawBox(dl, v, c, { r, r, h }, IM_COL32(255, 220, 60, 255));
            ImVec2 o, ax;
            if (Project(v, c, o))
            {
                const float len = std::max(60.0f, r * 1.5f);
                if (Project(v, c + V3{ len, 0, 0 }, ax)) dl->AddLine(o, ax, IM_COL32(255, 60, 60, 255), 3.0f);
                if (Project(v, c + V3{ 0, len, 0 }, ax)) dl->AddLine(o, ax, IM_COL32(60, 255, 60, 255), 3.0f);
                if (Project(v, c + V3{ 0, 0, len }, ax)) dl->AddLine(o, ax, IM_COL32(80, 140, 255, 255), 3.0f);
                ImVec2 top;
                if (Project(v, c + V3{ 0, 0, h + 10 }, top))
                    dl->AddText(ImVec2(top.x - 20, top.y - 18 * g_uiScale), IM_COL32(255, 230, 120, 255), Engine::Name(g_sel).c_str());
            }
        }
    }

    // Mouse and keys over the 3D view (not over a panel). Called after ImGui::NewFrame.
    void Interact(float dt)
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool overUI = io.WantCaptureMouse;
        const bool typing = io.WantCaptureKeyboard;
        // look while the right button is held (it started over the view)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !overUI && g_camActive) g_looking = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) g_looking = false;
        float lx, ly;
        {
            std::lock_guard<std::mutex> lock(g_mouseLock);
            lx = g_lookX; ly = g_lookY;
            g_lookX = g_lookY = 0;
        }
        if (g_camActive)
        {
            std::lock_guard<std::mutex> lock(g_camLock);
            const float k = 9.0f * g_lookSpeed;   // rotation units (65536 = 360 degrees) per mouse count
            g_camYaw += lx * k;
            g_camPitch = std::clamp(g_camPitch - ly * k, -16000.0f, 16000.0f);
            if (!typing)
            {
                const float p = g_camPitch * kRotToRad, y = g_camYaw * kRotToRad;
                const V3 fwd{ std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p) };
                const V3 right{ -std::sin(y), std::cos(y), 0 };
                const bool ctrl = io.KeyCtrl;
                const float f = ctrl ? 0.0f : float(ImGui::IsKeyDown(ImGuiKey_W)) - float(ImGui::IsKeyDown(ImGuiKey_S));
                const float r = ctrl ? 0.0f : float(ImGui::IsKeyDown(ImGuiKey_D)) - float(ImGui::IsKeyDown(ImGuiKey_A));
                const float u = ctrl ? 0.0f : float(ImGui::IsKeyDown(ImGuiKey_E)) - float(ImGui::IsKeyDown(ImGuiKey_Q));
                const float speed = g_camSpeed * (io.KeyShift ? 4.0f : 1.0f) * dt;
                g_camPos = g_camPos + fwd * (f * speed) + right * (r * speed) + V3{ 0, 0, u * speed };
            }
        }
        if (!overUI && io.MouseWheel != 0)
            g_camSpeed = std::clamp(g_camSpeed * std::pow(1.25f, io.MouseWheel), 50.0f, 6000.0f);

        const View v = CurrentView();
        // click: select; press on the selected actor and drag: move it
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overUI && !g_looking)
        {
            void* picked = Pick(v, io.MousePos.x, io.MousePos.y);
            if (picked && picked == g_sel)
            {
                g_dragging = true;
                g_dragVertical = io.KeyShift;
                const V3 a = ActorLoc(g_sel);
                const V3 dir = Ray(v, io.MousePos.x, io.MousePos.y);
                const V3 n = g_dragVertical ? Norm(V3{ v.fwd.x, v.fwd.y, 0 }) : v.fwd;
                const float den = Dot(dir, n);
                const float t = std::fabs(den) > 1e-4f ? Dot(a - v.pos, n) / den : 0.0f;
                g_dragOffset = a - (v.pos + dir * t);
            }
            else
            {
                g_sel = picked;
                if (picked) g_status = "Selected " + Engine::Name(picked) + " (" + Engine::ClassName(picked) + ")";
            }
        }
        if (g_dragging)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !g_sel || !Engine::Alive(g_sel)) g_dragging = false;
            else if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)
            {
                const V3 a = ActorLoc(g_sel);
                const V3 dir = Ray(v, io.MousePos.x, io.MousePos.y);
                const V3 n = g_dragVertical ? Norm(V3{ v.fwd.x, v.fwd.y, 0 }) : v.fwd;
                const float den = Dot(dir, n);
                if (std::fabs(den) > 1e-4f)
                {
                    const float t = Dot(a - g_dragOffset - v.pos, n) / den;
                    V3 to = v.pos + dir * t + g_dragOffset;
                    if (g_dragVertical) { to.x = a.x; to.y = a.y; }
                    SetLocation(g_sel, to);
                }
            }
        }
        // keys
        if (!typing)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) Focus();
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) Delete();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) Duplicate();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save(false);
        }
        DrawWorld(v);
    }

    void DrawUI()
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float w = vp->WorkSize.x, h = vp->WorkSize.y;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 10, vp->WorkPos.y + 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(w * 0.25f, h - 20), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Level Editor  (~ closes)", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            if (ImGui::BeginTabBar("tabs"))
            {
                if (ImGui::BeginTabItem("Actors")) { ActorsPanel(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Tools")) { ToolsPanel(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + w - w * 0.30f - 10, vp->WorkPos.y + 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(w * 0.30f, h - 20), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoCollapse)) InspectorPanel();
        ImGui::End();
        if (!g_status.empty())
        {
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + w * 0.5f, vp->WorkPos.y + h - 10), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.8f);
            if (ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
                ImGui::TextUnformatted(g_status.c_str());
            ImGui::End();
        }
    }
}

void Editor::Init()
{
    g_freeze = g_settings.editorPause;
    strncpy_s(g_saveFolder, Narrow(g_settings.editorFolder).c_str(), _TRUNCATE);
    if (g_settings.editorKey > 0 || Plush::Available())   // the camera hook: the free camera, and the plush's camera
    {
        if (const uintptr_t a = Mem::Find("ProcessEvent", "55 8B EC 83 EC 1C A1 ? ? ? ? 85"))
            Mem::Detour("ProcessEvent", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkProcessEvent), reinterpret_cast<void**>(&oProcessEvent));
    }
    LOG("Editor: %s (key 0x%X)%s", g_settings.editorKey > 0 ? "available" : "off", g_settings.editorKey,
        g_settings.editorKey > 0 && !oProcessEvent ? ", [!] no free camera" : "");
}

bool Editor::IsOpen() { return g_open.load(std::memory_order_relaxed); }

bool Editor::RenderCamera(float pos[3], float fwd[3])
{
    std::lock_guard<std::mutex> lock(g_camLock);
    if (!g_camActive && !g_haveGameCam) return false;
    const V3 p = g_camActive ? g_camPos : g_gamePos;
    const float pitch = (g_camActive ? g_camPitch : g_gamePitch) * kRotToRad, yaw = (g_camActive ? g_camYaw : g_gameYaw) * kRotToRad;
    pos[0] = p.x; pos[1] = p.y; pos[2] = p.z;
    fwd[0] = std::cos(pitch) * std::cos(yaw); fwd[1] = std::cos(pitch) * std::sin(yaw); fwd[2] = std::sin(pitch);
    return true;
}

void Editor::AddMouseDelta(float dx, float dy)
{
    std::lock_guard<std::mutex> lock(g_mouseLock);
    if (g_looking) { g_lookX += dx; g_lookY += dy; }
    else { g_pendX += dx; g_pendY += dy; }
}

bool Editor::WndProc(UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_open.load() || !g_ready) return false;
    switch (msg)
    {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        if (wp == WPARAM(g_settings.editorKey)) return true;   // the toggle itself: neither the game nor a text field
        ImGui_ImplWin32_WndProcHandler(Game::hWnd, msg, wp, lp);
        return true;
    case WM_MOUSEWHEEL:
    {
        std::lock_guard<std::mutex> lock(g_mouseLock);
        g_wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA);
        return true;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_MOUSEMOVE:
        return true;   // buttons are read directly, the position comes from the raw mouse
    default:
        return false;
    }
}

void Editor::OnNewDevice()
{
    if (g_open.load())
    {
        Engine::SetPause(false);
        g_open = false;
        std::lock_guard<std::mutex> lock(g_camLock);
        g_camActive = false;
        LOG("Editor: closed (map change)");
    }
    for (bool& b : g_buttons) b = false;
    g_looking = g_dragging = false;
    g_sel = nullptr;
    g_propsOf = nullptr;
    g_props.clear();
    g_cells.clear();
    g_rows.clear();
    g_rowsTick = 0;
    g_changes.clear();
    g_deleted.clear();
    g_confirmOverwrite = false;
    g_refreezeAt = 0;
    {
        std::lock_guard<std::mutex> lock(g_camLock);
        g_haveGameCam = false;
    }
    if (g_ready)
    {
        Shutdown();   // releases the overlay's device reference, textures and buffers
        LOG("Editor: overlay released the old device");
    }
}

void Editor::OnReset(bool before)
{
    if (!g_ready) return;
    if (before) ImGui_ImplDX9_InvalidateDeviceObjects();
    else ImGui_ImplDX9_CreateDeviceObjects();
}

void Editor::OnPresent(IDirect3DDevice9* dev)
{
    if (!Game::hWnd) return;
    const bool front = GetForegroundWindow() == Game::hWnd;
    if (!g_calcViewName && oProcessEvent) g_calcViewName = Engine::NameIndex("PlayerCalcView");
    if (g_settings.editorKey > 0 && g_settings.editorKey < 256)
    {
        const bool down = front && (GetAsyncKeyState(g_settings.editorKey) & 0x8000);
        if (down && !g_keyHeld) SetOpen(!g_open.load());
        g_keyHeld = down;
    }
    // The overlay also carries the CLUSTERFUCK victory ceremony and toasts (victory.cpp), editor open or not.
    const bool open = g_open.load(), victory = Victory::Wants();
    if (!open && !victory) return;
    if (open && g_refreezeAt && GetTickCount64() >= g_refreezeAt)
    {
        g_refreezeAt = 0;
        if (g_freeze) Engine::SetPause(true);
    }
    if (!Setup(dev)) { if (open) SetOpen(false); return; }

    ImGuiIO& io = ImGui::GetIO();
    RECT rc;
    GetClientRect(Game::hWnd, &rc);
    io.DisplaySize = ImVec2(float(rc.right - rc.left), float(rc.bottom - rc.top));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = std::clamp(float(double(now.QuadPart - g_last.QuadPart) / double(g_freq.QuadPart)), 1e-4f, 0.1f);
    io.DeltaTime = dt;
    g_last = now;
    io.MouseDrawCursor = open;   // the editor's own cursor; none for the ceremony alone
    if (open)
    {
        {
            std::lock_guard<std::mutex> lock(g_mouseLock);
            g_mx = std::clamp(g_mx + g_pendX, 0.0f, io.DisplaySize.x - 1);
            g_my = std::clamp(g_my + g_pendY, 0.0f, io.DisplaySize.y - 1);
            g_pendX = g_pendY = 0;
            if (g_wheel != 0) { io.AddMouseWheelEvent(0, g_wheel); g_wheel = 0; }
        }
        io.AddMousePosEvent(g_mx, g_my);
        const int vks[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
        for (int i = 0; i < 3; ++i)
        {
            const bool b = front && (GetAsyncKeyState(vks[i]) & 0x8000);
            if (b != g_buttons[i]) { io.AddMouseButtonEvent(i, b); g_buttons[i] = b; }
        }
    }

    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    if (open)
    {
        Interact(dt);
        DrawUI();
    }
    if (victory) Victory::Draw(dt);
    ImGui::EndFrame();
    ImGui::Render();

    // Draw into the back buffer whatever render target the frame ended on.
    IDirect3DSurface9* back = nullptr;
    IDirect3DSurface9* oldRT = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) dev->SetRenderTarget(0, back);
    if (SUCCEEDED(dev->BeginScene()))
    {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        dev->EndScene();
    }
    if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
    if (back) back->Release();
}

void Editor::OnMapLoad()
{
    g_records.clear();
    g_filesApplied = false;
    g_changes.clear();
    g_deleted.clear();
}

void Editor::LevelFilesApplied() { g_filesApplied = true; }

void Editor::Record(const std::string& folder, std::vector<LevelBlock>&& blocks)
{
    FileRecord& f = g_records[Lower(folder)];
    f.folder = folder;
    f.blocks = std::move(blocks);
}
