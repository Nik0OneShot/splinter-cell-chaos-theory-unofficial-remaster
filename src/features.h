#pragma once

#include <d3d9.h>
#include <string>
#include <vector>

// Feature modules. Each Init() is called once after the game executable is ready
// (first GetSystemTimeAsFileTime call from the CRT), and must tolerate missing patterns.
namespace Framerate  { void Init(); }
namespace Widescreen { void Init(); bool SnapshotActive(); }
namespace RawMouse   { void Init(); bool InGameCursor(int& x, int& y);     // cursor of an open hacking/keypad screen (640x480)
                       bool MenuCursor(int& x, int& y); }                  // last position of any page's cursor (640x480)
namespace Misc       { void Init(); }
namespace Mods       { void Init();   // Mods folder: mods' System\ and Data\ files replace or add game files
                       struct Info { std::wstring name, folder, dir; };   // dir: full path with a trailing backslash
                       const std::vector<Info>& Loaded();                 // enabled mods, lowest priority first
                       const std::wstring& Dir();                         // <game>\Mods\ (empty when mods are off)
                       void AddNew(const std::wstring& folder); }         // a mod folder the level editor just made
namespace Scripting  { void Init(); void OnPlayerTick(void* pawn, float dt);   // Lua scripts in each mod's Scripts folder
                       void OnMapLoad(const wchar_t* map, const wchar_t* options);
                       // The engine texture (UTexture of that size, or a mip of it) found on the stack from 'from', as
                       // "Package.Group.Name"; used to name texture dumps.
                       bool TextureName(const void* from, size_t bytes, unsigned w, unsigned h, std::string& out);
                       void Reload(); void DumpLevelNow();   // the editor's Tools buttons
                       void* EditorCopy(void* original, const float loc[3], std::string& error);   // through Lua's make_copy
                       bool EditorDelete(void* obj, std::string& error, int& belongings);   // with its gun, gear...
                       std::string MapName(); }                           // the current map, "" before the first
namespace Textures   { void Init(); bool Active();   // texture packs: dumping and replacing textures
                       void OnCreateTexture(IDirect3DTexture9* tex); void OnPresent();
                       IDirect3DBaseTexture9* Substitute(IDirect3DBaseTexture9* tex);
                       void OnUpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst);
                       bool Dumping(); void SetDumping(bool on); }
// Engine objects for the level editor (implemented with Lua's reflection in scripting.cpp).
namespace Engine
{
    struct PropRow { void* prop = nullptr; std::string name, owner, type; int dim = 1; bool internal = false, editable = false; };
    void* Player(); bool Alive(void* obj); void Actors(std::vector<void*>& out);
    std::string Name(void* obj); std::string ClassName(void* obj); std::string FullName(void* obj);
    bool Location(void* actor, float out[3]);
    void Properties(void* obj, std::vector<PropRow>& out);
    bool Export(void* obj, void* prop, int index, std::string& out);   // Unreal text syntax
    bool Import(void* obj, void* prop, int index, const std::string& text, std::string& error);
    void SetPause(bool on);
    int NameIndex(const char* text);                    // FName index (0 if the name doesn't exist)
    int ParamOffset(void* func, const char* name);      // a UFunction parameter's offset, -1 if none
    float GetFloat(void* obj, const char* name, float def);
    bool GetBool(void* obj, const char* name);
    bool FromMap(void* obj);                            // loaded with the map (its name is the same every time)
    void* GetObject(void* obj, const char* name);       // an object property's value (alive), or null
    void* Trace(void* tracer, const float start[3], const float end[3], float hit[3]);
}
// In-game level editor: an overlay opened with EditorKey.
namespace Editor     { void Init(); bool IsOpen(); void AddMouseDelta(float dx, float dy);
                       bool WndProc(UINT msg, WPARAM wp, LPARAM lp);   // true: the editor took the message
                       void OnPresent(IDirect3DDevice9* dev); void OnReset(bool before);
                       void OnNewDevice();   // the game is about to create a new device: release the old one
                       bool RenderCamera(float pos[3], float fwd[3]);   // the camera of the frame being drawn
                       // What a mod's level file did at this mission's start (so Save keeps it): its blocks, each with
                       // the actor it made or changed. Scripting's level-change applier reports these.
                       struct LevelBlock { std::string kind, head, label, copy, cls, name; void* obj = nullptr; bool gone = false;
                                           std::vector<std::pair<std::string, std::string>> props; };
                       void OnMapLoad();             // a new map: forget the old one's level files
                       void LevelFilesApplied();     // a mission start (not a savegame): the level files are applied now
                       void Record(const std::string& folder, std::vector<LevelBlock>&& blocks); }
namespace Videos     { void Init(); }
namespace Window     { void InitEarly(); void Init(); void SuppressInputFor(DWORD ms); }
namespace Watchdog   { void Init(); void Heartbeat(); }
namespace Perception { void Init(); void OnPlayerTick(void* pawn); bool LightAt(void* sam, const float point[3], float& out);
                       bool BeforePawnTick(void* pawn); void AfterPawnTick(void* pawn); }   // turn watch (LogPerception)
namespace Guards     { void Init(); void OnPawnTick(void* pawn); void OnPlayerTick(void* pawn);
                       float ShadowSense(void* pawn); float HearingSense(void* pawn); const wchar_t* Label(void* pawn);
                       float ReactionExtra(void* pawn); }   // seconds above the reaction minimum, -1 = unknown
namespace Reaction   { void Init(); void OnPlayerTick(void* pawn);
                       bool WantsEvents(); bool AllowEvent(void* ctrl, const uint8_t* ev); }   // focus: drop events about Sam
namespace Noise      { void Init(); void OnPlayerTick(void* pawn); void OnNpcTick(void* pawn); }   // noise pollution, footstep sync
namespace Aim        { void Init(); }   // mouse accuracy over 1/30 s, perfectly still aim
namespace Alert      { void Init(); void OnPlayerTick(void* pawn); void OnNpcTick(void* pawn);
                       bool WantsEvents(); void OnEvent(void* ctrl, const uint8_t* ev); }   // flinch, suppression   // draw time before the first shot, call-out before the group knows
namespace Watch      { void OnPlayerTick(void* pawn); }
namespace Scripted   { void Init(); void OnPlayerTick(void* pawn); }   // level scripts: Bathhouse ending delay
namespace Clusterfuck { void Init(); bool Active(); const wchar_t* CampaignId(); void OnPlayerTick(void* pawn); void OnNpcTick(void* pawn);   // CLUSTERFUCK difficulty
                       void OnMapLoad(const wchar_t* map, const wchar_t* options); void OnSaveLoaded(const wchar_t* path);
                       void OnSaveWritten(const wchar_t* path); void OnGuardEvent(void* ctrl, const uint8_t* ev);
                       void Celebrated(const wchar_t* id);   // the victory ceremony was seen to the end
                       void PreviewVictory(); void PreviewToast(); int Victories(); }   // the editor's Tools buttons
// The Niko plush, CLUSTERFUCK's unlock (plush.cpp; its gameplay is Lua in scripting.cpp's prelude).
namespace Plush      { void Init(); bool Available(); bool Active();   // loaded; and given in missions (unlocked or forced)
                       // The plushes to draw (in hand or thrown); the one in his hand is drawn at 'hand' (his weapon bone).
                       void SetItems(void* sam, void* const* actors, int count, void* handActor, const float* hand, int handYaw,
                                     int vision);   // EGoggle.CurrentMode
                       void Meow(const float pos[3], int kind);                    // 0 thrown, 1 a bounce
                       void OnSetVSConstants(IDirect3DDevice9* dev, UINT start, const float* data, UINT count);
                       void OnClear(DWORD flags); void OnDraw(IDirect3DDevice9* dev);
                       void OnStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src);   // before the copy
                       void OnBeforeClear(IDirect3DDevice9* dev, DWORD flags);
                       void OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code);
                       void OnSetVertexShader(IDirect3DVertexShader9* vs);
                       void OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code);   // the goggles' shaders
                       void OnSetPixelShader(IDirect3DPixelShader9* ps);
                       void OnSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface);
                       void OnPresent(IDirect3DDevice9* dev); void OnNewDevice(); void OnReset(bool before); }
// The CLUSTERFUCK victory ceremony and mission toasts, drawn in the editor's ImGui overlay (victory.cpp).
namespace VictoryText { struct Data; }
namespace Victory    { void AddFonts(); bool Wants(); void Draw(float dt);
                       void Celebrate(const VictoryText::Data& data, const std::wstring& certificate, const std::wstring& desktop);
                       void Toast(const std::wstring& title, const std::wstring& line, bool chime); }
namespace Movement   { void Init(); void BeforeOpticTick(void* opticCable, float dt); void AfterOpticTick(void* opticCable);
                       void OnPlayerTick(void* pawn); }
// Calling UnrealScript (script, not native) functions on objects; FNames are ints (FName::Index).
namespace Script
{
    void Init(); bool Ready(); int Name(const wchar_t* text); bool Call(void* obj, int function, void* parms);
    int NameAdd(const wchar_t* text);   // like Name, but creates the name when it doesn't exist
    // CallNative with the caller's result buffer and (optionally) locals, which EX_LocalVariable (00h) arguments read from.
    bool CallNativeEx(void* obj, void* code, const uint8_t* args, void* result, uint8_t* locals = nullptr);
    int StateName(void* obj);
    // Natives: the function's code (GNatives / UFunction.Func), called with bytecode arguments built by Args.
    void* NativeCode(void* obj, int function);
    void* Native(int index);
    void** NativeSlot(int index);   // GNatives entry (to hook a native the VM calls by number)
    void* Function(void* obj, int function);
    bool NameText(int name, wchar_t* out, size_t len);   // FName text   // UFunction (the current state's version first)
    bool CallNative(void* obj, void* code, const uint8_t* args);
    struct Args
    {
        std::vector<uint8_t> b;
        void Name(int name); void Float(float f); void Int(int i); void Byte(uint8_t v); void Bool(bool v); void Skip();
        const uint8_t* End();
        void Raw(const void* p, size_t n);
    };
}
namespace Stuck      { bool Wanted(); void Init(); void BeforePlayerTick(void* pawn); void AfterPlayerTick(void* pawn, float dt); }

// The d3d9.dll proxy itself.
namespace D3D9Proxy
{
    // True when Direct3D runs through DXVK (D3D9Chain) and it has not fallen back to Windows' D3D9.
    // Auto settings then let DXVK's vsync pace frames: VSync -1 = on, FPSLimit -1 = no cap, no RefreshSync.
    bool UsingDxvk();
}

// D3D9 side (called from the proxy).
namespace D3D9Hooks
{
    void OnCreate(IDirect3D9* d3d);
    void OnDevice(IDirect3DDevice9* device);
}

namespace ShaderPack
{
    using PFN_CreatePS = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
    // After the game created 'original' from 'function': if the pack replaces it, create ours next to it.
    void OnCreated(IDirect3DDevice9* dev, const DWORD* function, IDirect3DPixelShader9* original, PFN_CreatePS create);
    IDirect3DPixelShader9* Swap(IDirect3DPixelShader9* shader);   // what to bind for the game's shader
    void OnNewDevice();                                         // forget the old device's shaders
    void OnPresent();                                           // toggle key
}

namespace AmdFix
{
    // Returns replacement bytecode for a known-problematic legacy shader, or nullptr.
    const DWORD* Replacement(const DWORD* function, int* kind);
    void OnCreated(IDirect3DPixelShader9* shader, int kind);
    void OnSetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* shader);
    void OnSetTextureStageState(IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
    void OnSetPixelShaderConstantF(UINT start, const float* data, UINT count);
}

// Shared game state.
namespace Game
{
    extern HWND hWnd;
    // UObject::GetFullName (thiscall, wrapped as fastcall).
    using GetFullNameFn = wchar_t* (__fastcall*)(void* obj, void* edx, wchar_t* buffer);
    extern GetFullNameFn GetFullName;
    // Class names of an object, most-derived first (e.g. ESam, EPlayerPawn?, EPawn, Pawn, Actor, Object).
    // Returns the count, or -1 if reading failed.
    int ClassChain(void* obj, wchar_t (*names)[64], int max);
}
