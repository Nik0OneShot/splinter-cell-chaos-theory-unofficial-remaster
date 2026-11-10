#pragma once

#include <d3d9.h>

// Feature modules. Each Init() is called once after the game executable is ready
// (first GetSystemTimeAsFileTime call from the CRT), and must tolerate missing patterns.
namespace Framerate  { void Init(); }
namespace Widescreen { void Init(); bool SnapshotActive(); }
namespace RawMouse   { void Init(); bool InGameCursor(int& x, int& y);     // cursor of an open hacking/keypad screen (640x480)
                       bool MenuCursor(int& x, int& y); }                  // last position of any page's cursor (640x480)
namespace Misc       { void Init(); }
namespace Videos     { void Init(); }
namespace Window     { void InitEarly(); void Init(); void SuppressInputFor(DWORD ms); }
namespace Watchdog   { void Init(); void Heartbeat(); }
namespace Perception { void Init(); void OnPlayerTick(void* pawn);
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
                       void OnSaveWritten(const wchar_t* path); void OnGuardEvent(void* ctrl, const uint8_t* ev); }
namespace Movement   { void Init(); void BeforeOpticTick(void* opticCable, float dt); void AfterOpticTick(void* opticCable);
                       void OnPlayerTick(void* pawn); }
// Calling UnrealScript (script, not native) functions on objects; FNames are ints (FName::Index).
namespace Script
{
    void Init(); bool Ready(); int Name(const wchar_t* text); bool Call(void* obj, int function, void* parms);
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
