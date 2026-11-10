#pragma once

#include <string>

struct Settings
{
    // [Display]
    int   width = 0;                  // 0 = desktop resolution
    int   height = 0;
    bool  widescreen = true;          // Hor+ FOV and HUD/menu/scope scaling
    bool  hudWidescreenMode = true;   // push corner HUD panels out towards the screen edges
    int   hudEdgeOffset = 100;        // how far (640x480 units, at 16:9) corner panels move outwards
    float fovMultiplier = 1.0f;
    bool  unlockResolutions = true;
    bool  highResVideos = true;
    int   fmvAspect = 1;              // 1 = keep movie aspect, fill screen; 0 = inside 4:3 area; -1 = stretch (vanilla)
    bool  skipIntro = false;
    bool  disableSafeMode = true;
    bool  windowedAltTabFix = true;
    bool  steamOverlayFix = true;     // start Steam's overlay UI when Steam doesn't (it never does for this game)
    int   shadowMapResolution = 0;    // 0 = game default (2048)
    bool  shadowFiltering = true;
    int   gameLanguage = 0;           // 0 English, 4 Italian, 5 Polish, 6 Russian (HUD text tweaks)
    int   refreshRate = 0;            // fullscreen refresh rate: 0 = desktop rate, -1 = game's choice, N = Hz
    int   windowMode = 2;             // 0 = exclusive fullscreen, 1 = windowed, 2 = borderless fullscreen
    bool  skippableVideos = true;     // Space/Enter/Esc/click/controller skips videos
    bool  stretchLoadingScreens = false; // false = loading screen backgrounds keep 4:3 so the picture lines up
    int   vsync = -1;                 // -1 = game setting, 0 = off, 1 = on

    // [Framerate]
    int   fpsLimit = -1;              // -1 = monitor refresh rate, 0 = unlimited, N = cap
    int   logicRate = 60;             // AI/NPC/physics actors tick at most this often (0 = off)
    bool  throttleAI = false;         // off: guards' scripted sequences (chairs, keypads, doors) stall at 60 Hz
    bool  throttleNPCs = false;
    bool  throttlePhysicsProps = true;
    float maxFrameTime = 0.1f;        // vanilla clamps one frame's delta to 0.1 s
    bool  preciseLimiter = true;
    bool  refreshSync = true;         // borderless/windowed at refresh rate: start frames in step with vblank

    // [Mouse]
    bool  rawInput = true;
    float rawSensitivity = 1.0f;
    float menuCursorSpeed = 1.0f;     // 1 = menu cursor moves like the desktop cursor, 0 = original
    bool  recaptureMouse = true;      // capture the mouse again when the window is activated (alt-tab back)
    bool aimAccuracyFix = true;       // mouse accuracy penalty from the view's turn speed over 0.1 s, not one frame's input (0 = original)
    bool perfectStillAim = true;      // a perfectly still mouse lets accuracy settle to a perfect shot (0 = original floor)
    float aimFreeTurnSpeed = 90.0f;   // view turn speed (deg/s, over 0.1 s) below which turning costs no accuracy
    float aimFullTurnSpeed = 360.0f;  // view turn speed (deg/s) that costs the most accuracy
    bool aimLog = true;               // [AIM] log lines when turning costs accuracy
    float opticCableSpeed = 1.0f;     // optic cable turn speed, frame-rate independent (0 = original, frame-rate dependent)

    // [Graphics]
    bool  amdThermalFix = false;      // experimental: ps_1_1 legacy shader replacement
    int   shaderReplaceMask = 7;      // bit0 texreg2gb, bit1 texbem, bit2 texm3x3vspec
    int   spoofVendor = 0;            // 0 off, 1 NVIDIA, 2 AMD, 3 Intel
    bool  shaderPack = true;          // replace the SM1.1 specular shaders with ours (shaders/pack.hlsl)
    int   shaderPackToggleKey = 0x75; // VK_F6: switch the pack on/off live (0 = no key)
    int   shaderHighlightKey = 0x7A;  // VK_F11: magenta highlight through shader families (Shift: single shaders)
    int   shaderModel = 3;            // game's render path: 1 = SM1.1, 2 = SM2.0, 3 = SM3.0, 0 = in-game setting
    std::wstring d3d9Chain;           // optional wrapper DLL to load instead of the system d3d9
    bool  logShaders = false;
    bool  logRenderTargets = false;

    // [Audio]
    bool  eaxFix = true;

    // [Controller]
    bool  useXidi = true;

    // [Gameplay]
    bool  unstickPlayer = true;       // nudge Sam free when level geometry traps him
    bool  rescueStuckNPCs = true;     // put characters frozen mid-fall (knocked out of the level) back
    bool  fixStuckDoors = true;       // open a closed door for a guard whose AI thinks it is open
    bool  wallBounce = true;          // SC1/PT-style wall jump: press jump again in the air against a wall
    float wallBounceUp = 700.0f;      // upward speed of the kick (units/s; Sam's own jump is 520)
    float wallBounceOut = 150.0f;     // a steered kick leaves the wall at half of this or more
    float wallBounceReach = 64.0f;    // how far a wall may be from Sam's side (units)
    int   wallBounceMax = 1;          // kicks per jump (2 = double wall jumps)
    bool  wallBounceSplit = true;     // hold jump after a bounce to snap into a split-jump spot
    bool  splitFromFall = true;       // ...and while falling (jump held)
    float wallBounceSplitHold = 0.2f; // seconds jump must be held for that (shorter = a tap: just bounce)
    float splitCatchDamage = 0.5f;    // a split that catches a fall does this much of the fall damage (never fatal)
    std::wstring wallBounceAnim;      // animation played on the kick
    float wallBounceAnimTime = 0.35f; // how long it is held before the normal fall animation takes over
    int   wallBounceLeg = 0;          // 0 the leg on the wall's side kicks, 1 the other leg, 2 whole body
    float wallBounceKickStrength = 0.5f; // how far the kicking leg goes towards the animation's pose (0..1)
    std::wstring wallBounceFrontAnimR, wallBounceFrontAnimL; // leg pose for a wall in front (right / left leg)
    float wallBounceFrontFrame = 0.3f;   // frame of that animation used as the pose (0..1)
    float wallBounceFrontStrength = 1.0f; // how far the leg goes towards it (0..1)
    float wallBounceBoost = 400.0f;      // speed in the direction held on the movement keys (0 = off)
    float wallBounceSteerTime = 0.3f;    // how long after the bounce that direction can still change it (s)
    float wallBounceMinSpeed = 200.0f;   // horizontal speed at take-off needed for a bounce
    bool  wallBounceStanding = false;    // standing jumps bounce too (straight up)
    float wallBounceStandingUp = 850.0f; // upward speed of a standing bounce
    bool  rollOnlyWhenCrouching = true;  // landing from a fall only rolls when crouch was pressed
    bool  crouchInstantSwitch = true;    // a crouch/stand press mid-transition reverses it at once
    bool  quietStandingSteps = true;     // standing footsteps make the same noise as crouched ones (per speed)
    bool  crouchNoiseFix = true;         // steps during a crouch transition count as standing; skipped steps are made
    bool  hackingClickFix = true;        // [Mouse] hacking screen: click on press, 150 ms grace for a number just left
    bool  clusterfuck = true;            // CLUSTERFUCK difficulty in the menu (after Expert)
    bool  clusterfuckDesktop = true;     // beating CLUSTERFUCK also puts the victory certificate on the desktop
    int   nikoPlush = 1;                 // the Niko plush: 0 never, 1 once CLUSTERFUCK is beaten, 2 always (to try it)
    int   nikoPlushVolume = 100;         // meow volume (%)
    float nikoPlushNoise = 600.0f;       // how far guards hear it land (units)
    float nikoPlushMinLight = 0.2f;      // its brightness in the dark (1 = fully lit)
    float nikoPlushFullLight = 100.0f;   // the light level (Sam's meter scale) at which it's fully lit
    float nikoPlushHdrScale = 1.0f;      // brightness multiplier when drawn into the HDR scene
    int   nikoPlushDebug = 0;            // [Debug] NikoPlushDebug: 2 = without the depth test, 3 = plain magenta
    bool  fairHacking = true;            // hack locks never run out; hacking a device again is no harder (failing still sounds the alarm)
    bool  logCursor = true;              // [Debug] log menu-cursor jumps ([CURSOR])
    bool  logCrouch = false;             // [Debug] log Sam's animation channels after each crouch toggle
    bool  climbOverRailings = true;      // hanging from a railing's top bar (or any ledge with a floor over it), forward climbs over
    bool  wallBounceNoise = true;     // the kick sounds like a jump (and guards can hear it)
    bool  evenDiagonalSpeed = true;   // forward+strafe keys move as fast (and as loud) as one key
    bool  guardPersonalities = true;  // faction-role-personality per guard (SCCTRemaster_Guards.ini)
    int   detectionBaseline = 2;
    bool  fixSoloDifficulty = true;   // a leftover co-op session must not change a solo mission's difficulty      // detection uses this difficulty's values on all difficulties (1-3, 0 = original)
    int   speedStages = 9;            // mouse-wheel speed stages between slowest and fastest (vanilla 5, 0 = game's own)
    float bathhouseEndingDelay = 15.0f; // Bathhouse bombs: seconds before the elite guards come in after each defusal (0 = at once)
    float bathhouseSpawnClearance = 6.0f; // ...and not while Sam is within this many metres of their spawn point (0 = any time)

    // [Mods]
    bool  mods = true;                // load mods from <game>\Mods\<name>\ (System\ and Data\ files replace or add game files)
    bool  scripts = true;             // run mods' Lua scripts (Mods\<name>\Scripts\*.lua)
    int   reloadScriptsKey = 0;       // with Ctrl held: reload every mod's scripts (0 = off; the editor's Tools tab has it)
    bool  logModFiles = true;
    bool  texturePacks = true;        // replace textures from mods' Textures folders
    bool  levelChanges = true;        // apply mods' Levels\<map>.txt when a mission starts
    int   levelDumpKey = 0;           // with Ctrl held: list the level's actors in Mods\_LevelDump (0 = off; the editor's Tools tab has it)
    int   editorKey = 0xC0;           // [Editor] opens/closes the level editor (0xC0 = ~, unbound in the game; 0 = off)
    bool  editorPause = true;         // [Editor] freeze the world while the editor is open
    float editorScale = 0.0f;         // [Editor] UI size (0 = from the screen height)
    std::wstring editorFolder = L"MyLevel";   // [Editor] SaveFolder: the mod folder Save writes level files to
    bool  dumpTextures = false;       // save every texture the game loads to Mods\_TextureDump
    bool  dumpTexturesDDS = false;    // dump as DDS (the texture's own format) instead of PNG
    int   dumpTexturesKey = 0;        // with Ctrl held: switch dumping on/off while playing (0 = off; the editor's Tools tab has it)         // log each game file a mod provides, the first time the game opens it

    // [Debug]
    int   logLevel = 1;
    bool  logActorClasses = false;
    int   captureKey = 0x79;          // VK_F10: D3D9 frame capture (0 = disabled)
    float autoCaptureAt = 0.0f;
    bool  logShaderInputs = false;    // [Debug] log each SM1.1 shader pair's textures/constants once ([SHADER])       // [Debug] capture one frame this many seconds after start (0 = off)
    bool  perfLog = true;             // log a [PERF] breakdown for every second with frame drops
    bool  saveAudit = false;          // debug: log Sam's health/inventory/ammo/alarms at saves, loads and health changes
    bool  watchVisCrouchMul = false;  // debug: log every instruction that reads GameInfo.VisCrouchMul
    bool  logHudImages = false;       // log every distinct HUD/menu image once (widescreen layout work)
    bool  stuckDiagnostics = false;   // log when Sam is trying to move but stays in place
    int   slowKey = 0;                // hold to run at 30 FPS, e.g. 0x7A = F11 (0 = disabled)
};

extern Settings g_settings;

namespace Config
{
    void Load(const std::wstring& iniPath);
    void Dump();
    void Save(const wchar_t* section, const wchar_t* key, const wchar_t* value);   // writes one value to SCCTRemaster.ini
}
