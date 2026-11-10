# Splinter Cell Chaos Theory â€“ Unofficial Remaster

A fan-made overhaul of the PC version of *Tom Clancy's Splinter Cell Chaos Theory* (Steam 1.05). One
self-contained `d3d9.dll` fixes the game on modern PCs and reworks the guard AI. It comes with a curated set of
audio, controller and renderer components.

**Status:** `1.0.0-dev.142`. The whole campaign has been played through on Expert with the mod (3440x1440, AMD
RX 9060 XT) with no progression bugs. The new CLUSTERFUCK difficulty is still being tested.

## Features

### Performance and graphics

| | |
|---|---|
| **No 100 FPS cap** | A precise, low-CPU frame limiter replaces the hard cap: your refresh rate by default, any cap, or unlimited (`FPSLimit`). |
| **Smooth frame pacing** | Renders through the bundled [DXVK](https://github.com/doitsujin/dxvk) (Direct3D 9 on Vulkan) with Vulkan vsync and latency reduction. Windows' own D3D9 stutters badly at high frame rates in borderless mode. Without a Vulkan 1.3 GPU and driver, it falls back to Windows' D3D9 automatically. |
| **Shader Model 3.0** | The game often ended up on its oldest render path (Shader Model 1.1), with sparkly, blown-out specular and no parallax mapping, HDR or soft shadows. The mod starts it on the Shader Model 3.0 path: per-pixel lighting, parallax mapping, HDR with tone mapping, soft shadows. On the 1.1 path, the mod's own shader pack replaces the harsh original specular. |
| **Widescreen and ultrawide** | Hor+ FOV, HUD and text scaling, corner HUD moved towards the screen edges, scope and camera overlays, every resolution and refresh rate in the menu, cutscene aspect correction, high-res video support. Loading screens stay 4:3 with their progress bar in place instead of being stretched. |
| **Borderless fullscreen** | The default window mode: no display-mode switch, instant alt-tab, per-monitor DPI aware. Windowed and exclusive fullscreen are still available. |
| **AMD thermal vision** | Renders correctly on current AMD cards through DXVK. |
| **Physics at high frame rates** | Havok props never update faster than `LogicRate` (60 Hz), which keeps their physics stable at high frame rates. |
| **Steam overlay** | Steam loads its overlay into Chaos Theory but never starts it; the mod does. |
| **No pop-up lag** | Notifications and screenshot tools popping up over the game no longer make it lag. |
| **Skippable videos** | Space, Enter, Esc, a click or controller A/B/Start ends logos, the intro and cutscenes. |
| **Misc** | No safe-mode prompt after a crash, windowed alt-tab fix, shadow-map resolution, optional intro skip. |

### Controls and audio

| | |
|---|---|
| **Raw mouse input** | Fixes erratic or lost mouse movement with high polling-rate mice: WM_INPUT on its own thread, one delta per frame, no Windows acceleration. The menu cursor moves like your Windows cursor. |
| **Aim accuracy** | Moving the mouse no longer wrecks your accuracy at high frame rates. Turning costs accuracy by how fast your view really turns, and a perfectly still aim gives perfect shots. |
| **Optic cable** | No longer turns faster the higher your frame rate (it was about 5.5x too fast at 165 FPS). |
| **Hacking and keypads** | Clicks always pick what's under the cursor, the cursor no longer skips or jumps on wide screens, and HUD icons no longer jitter across the screen. |
| **EAX audio** | Routes the game's `eax.dll` through the bundled DSOAL, so EAX works with no registry edits. |
| **Controllers** | Loads the bundled Xidi directly, so no `dinput8.dll` proxy is needed. |

### Movement

| | |
|---|---|
| **Wall jump** | The wall jump from Splinter Cell 1 and Pandora Tomorrow, removed in Chaos Theory: jump next to a wall and press jump again in the air to kick off it. In a split-jump spot, a third press goes straight into the split jump. If a bounce leaves Sam hanging from a railing, he climbs over it, and holding jump while falling catches a split jump. Height, reach, push, animation and noise are tunable. |
| **Even diagonal speed** | Fixes the original bug where W+A or W+D moves Sam faster and louder than one key. |
| **Finer speed control** | The mouse wheel steps through 9 walking speeds instead of 5 (`SpeedStages`, 2-41). |
| **Crouching** | Crouching and standing are instant and smooth. Standing footsteps are as quiet as crouched ones, crouch spam no longer makes Sam's steps silent, and he only rolls on landing when you press crouch. |
| **Unstick** | Frees Sam when he gets trapped in level geometry (a vanilla collision bug, for example in a Lighthouse corridor), never through a wall. |

### Guard AI

| | |
|---|---|
| **Guard personalities** | Every guard is faction-role-personality (e.g. guerilla-technician-paranoid) with his own sight, field of view, perception, search, aim, health, shadow sense and hearing. Difficulty decides how common tough guards are; detection itself is the same on every difficulty. Named characters keep their authored values. Fully configurable in `SCCTRemaster_Guards.ini`. |
| **MGS2-style perception** | **Shadows:** guards notice Sam's shadow from every shadow-casting light. **Peripheral vision:** sight is full in the middle of a guard's view and fades towards the edge. **Directional lighting:** each guard sees the side of Sam facing him. **Backlighting:** a lit wall, window, fire or the sky behind Sam outlines him even in darkness. **Partial cover** counts, and crouching is no harder to spot at a distance than standing. |
| **Reaction times** | Guards need the human minimum of 0.25 s plus their personality's own time to react, longer when Sam is hard to make out. |
| **Call-outs** | The guard who spots Sam needs a moment to tell the others. Take him down first and nobody hears it. A guard who finds Sam again has to call it in again. |
| **Drawing and aiming** | A guard who spots Sam turns, draws his gun if it was put away and raises it before shooting. Then he has to actually aim: his aim settles over time and is worse while he or Sam moves, in the dark, and when he's wounded. The original's guaranteed first hit and rubber-band hit chance are gone. |
| **Under fire** | Guards flinch when hit, get suppressed and duck under your fire, and may fire a warning shot first. |
| **Noise** | Gunfire, loud noises and guards' footsteps drown out Sam's noise (shown on the sound meter), and walking in step with a guard covers your footsteps. |
| **Focus** | A guard fighting another NPC (the Bathhouse fight, or any faction fight) ignores Sam unless he's right in front of him or shoots at him. |
| **Frame-rate fixes** | Guards no longer turn, react or fire faster at high frame rates (one bug had them firing a bullet every frame), and UAV camera heads no longer snap onto Sam. |
| **Stuck guards** | Guards no longer get stuck against closed doors, and a guard left hanging in mid-air (a vanilla Lighthouse bug) is put back where he stood. |

### Fixes and gameplay

| | |
|---|---|
| **Savegame difficulty** | Fixes the original bug where loading a save dropped the mission to the lowest difficulty. |
| **Fair hacking** | Hacks never run out of locks (on Expert the hardest hacks had none, so fragment clicks silently did nothing), and hacking a device again is no harder. Failing or timing out still sounds the alarm. |
| **Bathhouse ending** | The elite guards at the end come in 15 s later instead of instantly, and never teleport in right next to you. Shetland's timing is untouched. |
| **Bathhouse crash** | Fixes an original crash when two guards block each other's path. |

### CLUSTERFUCK difficulty

A fourth difficulty after EXPERT in the difficulty menu, for people who want to suffer. It's opt-in, and it's
meant to be unfair.

- **Guards:** every guard is elite, with vanilla night vision (darkness doesn't hide you), flashlights and clips
  that never run out. They draw, react and aim faster. Call-outs are near instant and reach the whole level, and
  there are no warning shots.
- **Alarms:** missions with an alarm system start on base alert, and every body a guard finds raises another alarm.
- **The world:** knocked-out guards wake up after 2.5 minutes, alerted guards search forever, cameras spot you
  instantly, turrets shred you, guards investigate lights you put out, and every wall mine you defuse goes off.
- **Sam:** 50 HP, no medkits, half ammo, no ammo pickups, no light or sound meter. Footsteps stay as loud as normal.
- **Hacking:** every hack is the hardest kind, with no locks.
- **Ironman:** no manual saves. The mod saves once at the start of every mission.
- **Permadeath:** when Sam dies, every save of that playthrough is deleted (and only that one). A
  `REST IN PEACE` text file takes their place in the save folder: how he died, who killed him and how far he got.
- **Victory:** every mission you survive gets a "MISSION SURVIVED" banner. Beat all 11 missions in one life and the
  end of Kokubo Sosho turns into **YOU ACTUALLY DID IT!**: a full-screen ceremony with a fanfare, confetti,
  fireworks and a medal (the Order of the Clusterfuck), your rank, badges and stats. A
  `YOU ACTUALLY DID IT - CLUSTERFUCK <id>.txt` certificate goes in the save folder and on your desktop
  (`ClusterfuckCertificateOnDesktop = 0` keeps it off the desktop). The difficulty menu shows a star after
  CLUSTERFUCK for each time it's been beaten. The level editor's Tools tab can preview the ceremony.
- **An award:** beat CLUSTERFUCK and you take a little something into every mission after that, at any difficulty.
  What it is, you'll have to find out.

`Clusterfuck = 0` in `SCCTRemaster.ini` removes the menu entry.

## Mods

The remaster adds mod support to single-player and co-op, which never had any. It's being built in phases; the
**Mods folder**, **Lua scripting**, **texture packs**, **level changes** and the start of an **in-game level editor** are in:

- Every folder in `Mods\` (next to `System\` and `Data\`) is a mod. Its `System\` and `Data\` folders mirror the
  game's: a file with the same path as a game file replaces it, and a new file is added as if it had shipped with
  the game. The game's own files are never changed.
- An optional `mod.ini` gives the mod's name, author, version, on/off switch and priority (when two mods replace
  the same file, the higher priority wins).
- **Lua scripting:** Lua 5.4 scripts in a mod's `Scripts\` folder react to the game (a level starting, every frame,
  key presses, timers) and can read and change any UnrealScript property of any object (Sam, guards, the level) and
  call its functions. Scripts are sandboxed: no file access, no running programs. `Mods\SCRIPTING.txt` documents
  the API; the level editor reloads scripts while playing.
- **Texture packs:** the level editor dumps every texture the game loads (named with the game's own texture names where
  possible), and any image in a mod's `Textures\` folder replaces the texture with the same fingerprint, at any
  resolution: PNG, TGA, JPG and BMP (compressed to DXT with mipmaps automatically) or DDS. `Mods\TEXTURES.txt`
  explains it.
- **Level changes:** a text file per map in a mod's `Levels\` folder adds actors, changes the ones in the map (any
  property, in UnrealEd's text syntax) or removes them, every time the mission starts. The level editor lists every actor of
  the current level so you know their names. `Mods\LEVELS.txt` explains it.
- **Level editor (in progress):** press `~` in a level to open it. The world freezes and a free camera flies through it
  (right mouse + WASD). Click an actor to select it, drag it to move it, duplicate it (guards too) or delete it (with
  the gun and gear it carries); the Actors tab lists every actor, the Inspector shows and edits any property in
  UnrealEd's text syntax, and the Tools tab has the camera, grid snapping, actor names, freezing the world, the UI size
  and the mod tools. **Ctrl+S saves** the edits as a level file (`Mods\<folder>\Levels\<map>.txt`), merged into what
  that file already did, so a level can be built up over several sessions.
- `Mods\README.txt` explains the rest, and `Mods\_Example` is a template to copy.

Coming next for the editor: walls and rooms, then placing guards, patrols, lights and triggers from a palette.

## Install

1. Download the latest release zip from the [Releases](../../releases) page and extract it anywhere.
2. Run `Install.bat`. It finds the Steam install, moves conflicting mods into
   `System\_backup_before_remaster\<date>\` (FusionFix/WidescreenFix, Ultimate ASI Loader, SCFix, the old wined3d
   fix, Creative ALchemy) and copies the remaster in. Nothing gets deleted.
   - If the old wined3d AMD fix was installed, it's kept as `d3d9_wined3d.dll`. Set
     `D3D9Chain = d3d9_wined3d.dll` in `SCCTRemaster.ini` to keep rendering through it while using every other fix.

When updating, the installer keeps your `SCCTRemaster.ini` and `SCCTRemaster_Guards.ini` and writes the new
versions next to them as `*.default.ini`. New options default to on even when your file doesn't list them.

To uninstall, delete the files the installer copied and move the backup back.

## Settings

Everything can be switched off or tuned. All settings live in `System\SCCTRemaster.ini` and
`System\SCCTRemaster_Guards.ini` (guards and perception), both documented inline. A log is written to
`System\SCCTRemaster.log`.

With `LogPerception = 1` in `SCCTRemaster_Guards.ini`, the log also records what the guards see and hear, and
**F7** / **F8** log the guards' AI state and the lighting check around you. Turn it off for normal play.

## Build

Requirements: Visual Studio 2022 or newer with the C++ workload (it includes CMake) and the Windows SDK.

1. Download `SplinterCellChaosTheory.WidescreenFix.zip` from
   [ThirteenAG's release](https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/scct) into a
   `references\` folder in the repository. The build takes DSOAL and Xidi from it.
2. Run:

```bash
powershell -ExecutionPolicy Bypass -File tools/build.ps1
```

The output goes to `dist\SCCT-Unofficial-Remaster-v<version>\` and a matching `.zip`.

## How it works

`d3d9.dll` is a proxy: the game imports `Direct3DCreate9` statically, so Windows loads it before the game starts.
It forwards to the bundled DXVK (`d3d9_dxvk.dll`, set by `D3D9Chain`) or to the system `d3d9.dll`. Patching is
deferred until the game's C runtime first calls `GetSystemTimeAsFileTime`, i.e. after the Steam DRM stub has run.
All code patches are located by byte pattern, and each one is verified to match exactly once. The
reverse-engineering notes are in the comments at the top of each `src/*.cpp` file.

## Known issues

- **AMD thermal vision on Windows' D3D9:** fixed by the default DXVK renderer. Without DXVK, `AMDThermalFix` is an
  experimental shader fix, and the old wined3d chain works as a fallback.
- **HUD light meter:** with directional lighting on, the meter still shows Sam's overall light level (all sides);
  a guard on his dark side sees less.
- **Short stutters on autosave:** the game writes about 5 MB per save; the 200-300 ms hitch is the original save
  code.
- **Widescreen HUD:** ported from ThirteenAG's 2024 WidescreenFix. FusionFix's newer per-element HUD layout isn't
  ported yet.

## Credits

- Widescreen logic and several patterns: **ThirteenAG**,
  [WidescreenFixesPack](https://github.com/ThirteenAG/WidescreenFixesPack) (MIT).
- Hooking: [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD-2-Clause).
- Scripting: [Lua](https://www.lua.org) by Lua.org, PUC-Rio (MIT).
- Texture packs: [stb](https://github.com/nothings/stb) image libraries by Sean Barrett (MIT / public domain).
- Level editor: [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT).
- Bundled: [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors (zlib).
- Bundled: [DSOAL](https://github.com/kcat/dsoal) and [OpenAL Soft](https://github.com/kcat/openal-soft)
  (LGPL), [Xidi](https://github.com/samuelgr/Xidi) by Samuel Grossman (BSD-3-Clause).

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

The mod's own code is under the [MIT License](LICENSE). Third-party code and binaries keep their own licenses.

This is an unofficial fan project, not affiliated with Ubisoft. You need to own the game.
