# Changelog

## 1.0.0-dev.142
(dev.122 to dev.142)
- **The CLUSTERFUCK award.** Beat CLUSTERFUCK and you take a little something into every mission after that, at any
  difficulty. What it is, you'll have to find out. It works in normal vision and with every goggle mode (night
  vision, thermal, electromagnetic), lit by the light where it is.
- **F10 frame captures** list the game's copies of the screen (StretchRect), mark the back buffer (BB) and end with
  the bytecode of every shader the frame used.

## 1.0.0-dev.121
- **CLUSTERFUCK victory: "YOU ACTUALLY DID IT!"** Beating the whole game on CLUSTERFUCK now counts for something.
  - The mod spots each won mission: the level's `EndMission(true)` sets the mission manager's status
    (`EMissionManager.m_eMissionStatus`, `EPlayerController.MISSION` +5C0h, +B4h = 1) while Sam is alive. Each won
    mission is stored in the campaign (`Won <map> = time and the mission's stats`) and gets a "MISSION SURVIVED" banner
    with a chime (`CLUSTERFUCK - 4 / 11 - Penthouse`). A death after the mission is won doesn't count.
  - Winning Kokubo Sosho in a campaign that played all 11 solo missions in one life is beating the game: a
    full-screen ceremony over the final fade-out (a white flash, the title slamming in with a rainbow shimmer, a
    sunburst, confetti cannons and rain, fireworks, an original synthesised fanfare, a swinging medal: the Order of
    the Clusterfuck), then the stats, a rank (THE GHOST OF THE CLUSTERFUCK for no kills, THE SANDMAN for knockouts,
    THE ONE-MAN CLUSTERFUCK for 200 kills...) and badges (FLAWLESS on the first life, STUBBORN after 10, PACIFIST,
    MERCIFUL, PHANTOM, JANITOR, SPEED DEMON, MARATHON...). Enter ends it. If it isn't seen to the end, it plays again
    at the next main menu.
  - A certificate, `YOU ACTUALLY DID IT - CLUSTERFUCK <id>.txt`, goes in the save folder (next to where the REST IN
    PEACE notes go) and on the desktop (`[Gameplay] ClusterfuckCertificateOnDesktop`, on): big block letters, the
    medal, what they lived through, the campaign's dates and how many lives it took (every earlier campaign that
    died), a table of every mission's time, kills, knockouts, injuries, bodies found and times spotted, the badges
    and a "seal of authenticity".
  - The difficulty menu shows a star after CLUSTERFUCK for every time it was beaten (up to five).
  - Winning Kokubo Sosho in a campaign that started later says so (no medal); winning it again in a campaign that
    already won says "YOU DID IT. AGAIN." A Sam who dies after his campaign's victory still loses its saves, but the
    campaign keeps its victory.
  - The level editor's Tools tab has "Preview the victory ceremony" (a sample campaign and
    `System\YOU ACTUALLY DID IT - PREVIEW.txt`; "Watch your victory again" once there is one) and "Preview a mission
    toast".
  - The editor's overlay now also draws while the editor is closed, for the ceremony and the banners.

## 1.0.0-dev.120
- **Level editor, step 3: saving.** Ctrl+S (or Save in the Tools tab) writes the edits as a level file,
  `Mods\<folder>\Levels\<map>.txt`, for the mod folder named in the Tools tab (`[Editor] SaveFolder`, MyLevel at
  first; a new folder gets a `mod.ini` and counts at once, no restart).
  - Every edit is remembered with the value it set, the engine's own text for it at that moment, so a placed guard is
    saved where he was put even if he walks off afterwards.
  - The level-change applier now reports what each file did (its blocks and the actors they made or changed), so when
    the mission started with that file applied, saving keeps the file's blocks and merges the edits into them (moved
    actors get their new Location, deleted ones drop out, changes to a deleted map actor become `Delete`), then adds
    the editor's own copies (`Begin Actor Copy=` the map actor it all started from, with every property set along the
    way: a file makes its copies before setting properties, so copying a copy wouldn't carry them), changes
    (`Begin Change`) and deletions. Saving again doesn't duplicate anything.
  - After a savegame the files aren't applied, so the editor asks before replacing an existing file.
  - Changes to things made while playing (a guard's weapon, effects) have no fixed name and aren't saved; the status
    line says so.
- **Deleting takes along what the actor carries:** a guard's gun, flashlight and gear (actors it owns or that are
  attached to it, and theirs) go with him, in the editor and in level files' `Delete`. Characters and controllers are
  never taken along (Sam standing on a deleted crate stays), and an actor the game won't destroy is hidden; a hidden
  character also loses its AI. Sam can't be deleted.
- **Copies no longer share the original's native handles:** `copyprops` skipped engine bookkeeping but still copied
  `HavokRigidBody`, `HavokPhantom` (the Havok body and phantom the game makes per character), `m_hLipSynchData`,
  `DynOccId`, `impactID` and `nInRainCount`. Copied guards that were killed didn't fall like ragdolls; this is the
  likely cause. To check, the log now lists what differs between the first copy of each class and its original
  (`[LUA] Level editor:` lines), and the physics state when a copy or its original dies (and 1.5 s later).
- **Freeze the world** checkbox in the Tools tab (saved as `[Editor] EditorPause`).
- **Duplicates no longer stand in the bind pose** while the world is frozen: duplicating a character lets the world run
  for 0.2 s so it takes its pose.

## 1.0.0-dev.119
- **Level editor, step 2: free camera, picking, moving, duplicating, deleting.**
  - **Free camera** while the editor is open: hold the right mouse button to look, WASD to fly, Q/E down/up, Shift
    four times faster, the mouse wheel over the world changes the speed (Tools: fly and look speed). It replaces the
    view the engine asks the player controller for every frame (the script event `PlayerCalcView`, through a detour
    of `UObject::ProcessEvent`), so it works with the world frozen and leaves Sam alone; it starts where the game's
    camera was and hands back when the editor closes.
  - **Click to select:** a ray through the cursor (the widescreen Hor+ field of view); `Actor.Trace` finds what it hits
    with collision, and visible actors without collision are picked within their collision radius. The selection gets
    a box, its axes (red X, green Y, blue Z) and its name. **Drag** the selected actor to move it in the plane facing
    the camera (Shift: only up/down), through `SetLocation`; **Snap to grid** rounds it. **F** flies the camera to it.
  - **Duplicate** (Ctrl+D) makes a copy beside it (the same copy as level files' `Copy=`: guards get their body, AI
    and controller), **Delete** (Del) destroys it (or hides it and removes its collision if the game won't). The
    Actors list is sorted from the camera; double-click focuses.
  - **Show actor names** labels the nearest 150 actors within 30 m. **UI size** slider (saved as `[Editor] UIScale`);
    the default is larger (screen height / 900).
  - Changes are counted for saving, which is the next step.
- Lua: the copy behind `Begin Actor Copy=` is now one function shared with the editor.

## 1.0.0-dev.118
- **Level editor: leaving a level after using it no longer gets stuck on the loading screen.** The game makes a new
  graphics device for every map change, and the editor overlay (Dear ImGui's DX9 backend) held a reference to the old
  one plus textures and buffers on it, so the old device and its swap chain on the same window stayed alive: presents
  slowed to about 30 fps and the menu never appeared. The overlay now lets go as soon as the game starts creating the
  new device (CreateDevice hook) and reconnects the next time it's opened; the selection is cleared on a map change.

## 1.0.0-dev.117
- **In-game level editor, step 1: the overlay** (`[Editor] EditorKey` 0xC0, the ~ key, which the game leaves unbound).
  - Opening it freezes the world (`LevelInfo.Pauser`, `EditorPause` 1), drops every game input event (in
    `CauseInputEvent`), gives the raw mouse to the editor's own cursor and the keyboard to its text fields.
  - **Actors** lists every actor of the level, nearest to Sam first, filterable by name or class.
  - **Inspector** shows every property of the selected actor, grouped by the class declaring it, in UnrealEd's text
    syntax (the engine's own `ExportText`), and edits it (Enter applies, through `ImportText`; Location, Rotation,
    DrawScale, StaticMesh, collision and the like through the game's own functions). Engine internals are hidden
    and read-only. Edits are live only for now: saving to a level file is the next step.
  - **Tools** reloads Lua scripts, switches texture dumping on and off and writes the actor list.
  - Drawn with Dear ImGui (MIT, `third_party/imgui`) just before each Present; it scales with the screen height.
- **The Ctrl+F8 / F9 / F11 shortcuts are off by default** (`LevelDumpKey`, `DumpTexturesKey`, `ReloadScriptsKey` 0):
  the game binds F8 to QuickLoad, F9 to Screenshot and F11 to Brightness, so Ctrl+F8 could quickload. The editor's
  Tools tab does all three.

## 1.0.0-dev.116
- **Level changes: copied guards fall when knocked out or killed.** dev.115's copies shot (and hit) Sam without
  crashing, but one knocked out froze standing: characters fall as Havok ragdolls, and their Havok shape is made natively
  at level start (`EPawn.CreateHavokShape`, never called from script). A copy now makes its own.
- LEVELS.txt: `EGameplayObject` is the game's breakable-object class (a guard shot the test barrel away); props that
  should stay need `bDamageable=False`.

## 1.0.0-dev.115
- **Level changes: copied guards can shoot.** dev.114's copies animated and fought, but the game crashed on their first
  shot: the gunfire is announced to the shooter's AI zone (0x10BE0150 reads the pawn's zone at +428h), and the copy had
  none, because the copy only kept links to bStatic/bNoDelete actors and AI zones are neither. Links are now kept to any
  actor loaded with the map (it has a linker, UObject +10h) and still dropped for actors spawned while playing.

## 1.0.0-dev.114
- **Level changes: copied guards animate and don't crash when they spot Sam.** dev.113's copies were visible and their
  AI reacted, but they T-posed and the game crashed as one spotted Sam: the pawn reads its controller's
  `EnemyInfo[iEnemy]` (0x1094C5D0), and the enemy list is only filled for the map's controllers at level start, so
  the copy's was empty. A copy's controller now gets `EnemyInfo` and its indexes from the original's controller, and
  the copy relinks its animations to its body (`LinkNPCAnims`, `SetupSkelMesh`), which it didn't have when it spawned.
- Lua: `copyprop(dst, src, name)` deep-copies one property.

## 1.0.0-dev.113
- **Level changes: copied guards no longer break (and the game no longer crashes leaving the level).** dev.112's copy
  also copied the engine's own object bookkeeping, which Object.uc exposes as properties (ObjectInternal with the
  object's index and hash links, Outer, ObjectFlags), so the copies looked destroyed (`the object no longer exists`),
  stood around as invisible collision, and the game crashed cleaning up the level. The copy now skips everything in
  UObject (below +28h) and every property flagged native (400h), 800h or 400000h (XLevel, Level, Region, Touching, ...).
- A copied character whose original had its body streamed out (far from Sam) gets the body from `BackupMesh`.

## 1.0.0-dev.112
- **Level changes: copying actors (`Begin Actor Copy=<name>`)**, the way to add guards. dev.111's added guards were
  invisible and without AI: a map's guards get their body (`Mesh`/`BackupMesh`, streamed in near the player), AI pattern,
  zones, behaviour and weapons from properties set in the editor, and the engine only gives pawns a controller during
  level startup (`Pawn.PostBeginPlay` checks `Level.bStartup`). A copy spawns the original's class, copies every
  designer-set property with the engine's deep copy (`UProperty::CopyCompleteValue`) except position, tag, runtime
  state and links to runtime actors (only references to fixed map actors such as AI zones are kept), and gives a copied
  character a controller of the same class as the original's.
- Lua: `copyprops(dst, src)`.
- Note: a character's `DrawScale` changes its value but not its look in this game; scaling works on props.

## 1.0.0-dev.111
- **Level changes** (mod support, phase 5; `[Mods] LevelChanges` 1). Documented for modders in `Mods\LEVELS.txt`:
  - `Mods\<mod>\Levels\<map>.txt` adds actors (`Begin Actor Class=... End Actor`), changes actors already in the map
    (`Begin Change Name=... End Change`) and removes them (`Delete Name=...`). Applied every time the mission starts
    (a URL with MISSIONDIFF, so Retry too), not when a savegame loads, since the save already has them.
  - Properties use UnrealEd's text syntax and the engine's own parser (UProperty::ImportText, vtable +8Ch), so any
    property works, objects by their full name included. Location, Rotation, StaticMesh, DrawType, DrawScale(3D),
    Physics, the collision flags and size go through the game's functions (SetLocation, SetStaticMesh, SetCollision,
    ...). `@Label` points at an actor added in the same file. An actor the game won't destroy is hidden and loses its
    collision instead.
  - Every line that doesn't work is logged with its line number and the reason; the log sums up each map's changes.
  - **Ctrl+F8** (`LevelDumpKey`) lists every actor of the current level (name, class, location, tag, static mesh) in
    `Mods\_LevelDump\<map>.txt`.
- Lua: `actor(name)` finds a level's actor by name, and `importtext(obj, property, text [, index])` sets a property
  from text syntax.

## 1.0.0-dev.110
- **Texture dumps: more of them get the game's texture name.** dev.109 captured the textures (835 dumped from the menu
  and Lighthouse), but only the loading screens were named: the uploader (0x10C8CAF0) receives the texture's render
  interface, which sits inside the engine texture object rather than at its start. The name search now also looks a
  little before such an interface for the texture object.
- **Dumping is incremental:** textures already in `Mods\_TextureDump` from an earlier session aren't written again; a
  fingerprint-only file is renamed when the game's name is found this time.

## 1.0.0-dev.109
- **Texture packs: level textures are captured.** The renderer fills most textures with IDirect3DTexture9::LockRect
  on each mip level (passing the full rectangle) and only a few through a level's surface; dev.107 only watched the
  surfaces, so a mission's textures were missed. Both ways are hooked now, and a lock with a rectangle covering the
  whole level counts as a full upload.

## 1.0.0-dev.108
- **Texture dumping saves what's already loaded:** switching dumping on (Ctrl+F9) now also saves every texture that's
  already in memory, read back from the texture (these files have only the fingerprint as their name; textures the
  game loads while dumping is on also get the game's own name).
- The log notes the first texture dumped and any file or folder it couldn't write, and textures whose format can't be
  saved.

## 1.0.0-dev.107
- **Texture packs: textures are actually captured.** The renderer never locks a texture itself: it takes the mip level
  as a surface (GetSurfaceLevel) and locks that, so dev.106 saw no uploads and dumped nothing. The surface's
  LockRect/UnlockRect are hooked now, and a lock of a texture's top-level surface is fingerprinted.
- The log counts texture uploads seen, replaced and dumped (`[TEX]`, every few seconds while it changes).

## 1.0.0-dev.106
- **Texture packs** (mod support, phase 2; `[Mods] TexturePacks` 1). Documented for modders in `Mods\TEXTURES.txt`:
  - **Dumping:** Ctrl+F9 (`DumpTexturesKey`) or `DumpTextures = 1` saves every texture the game loads to
    `Mods\_TextureDump\` as PNG (or DDS with `DumpFormat = dds`), once each, named
    `<game's texture name>__<fingerprint>.png`. The game's name is found by looking for the engine texture being
    uploaded and is only there to help find things.
  - **Replacing:** any image in a mod's `Textures\` folder (subfolders too) whose name ends in a texture's 16-digit
    fingerprint replaces it, at any resolution. PNG, TGA, JPG and BMP are compressed to DXT when the original was
    compressed and get a full mip chain; DDS (DXT1/3/5 or uncompressed, with or without mipmaps) is used as saved.
    The higher-priority mod wins when two replace the same texture.
  - How: a texture's fingerprint is a hash of its top mip level as the game uploads it (pixels, size, format), taken
    when it's unlocked; textures with a replacement are swapped for it in SetTexture (and through
    UpdateTexture), and the replacement is freed with its last user. Textures that change all the time are left alone.
- stb_image, stb_image_write and stb_dxt (MIT / public domain) are compiled in (`third_party/stb`). The package adds
  `Mods\TEXTURES.txt`.

## 1.0.0-dev.105
- **Lua: calling game functions with parameters works** (the test mod's K key killed Sam or crashed the game). This
  engine flags function parameters differently from stock Unreal Engine 2 (parameter 20h, out 40h, return value 100h
  instead of 80h, 100h, 400h, read from Engine.u and Core.u), so no parameters were found: `SetLocation` was called
  without its new location and moved Sam to a garbage position. A function whose parameters don't add up to its
  parameter size is now refused with an error instead of being called.
- **One-letter names are read correctly** (the log showed `X&Name:`): the engine's name table is 8-bit text, which
  was misread as 16-bit for names one character long. This affected the Lua dump and log lines naming such fields.

## 1.0.0-dev.104
- **Lua: vectors work.** `sam.Location.X` was nil, so vector maths failed (the L key of the test mod). Vector fields
  are now always `X`, `Y`, `Z` and rotator fields `Pitch`, `Yaw`, `Roll`; other structs' fields can be read and written
  in any case, and engine class names are compared without case. (The cause turned out to be one-letter names being
  misread, fixed in dev.105.)
- **Lua: a handler is only switched off after 10 failures in a row.** An error in one branch (a key press) no longer
  disables the whole handler, which is why the test mod's J key stopped working until a reload.

## 1.0.0-dev.103
- **Lua scripting** (mod support, phase 3; `[Mods] Scripts` 1). Every enabled mod's `Scripts\*.lua` files run in Lua
  5.4 when the game starts, in mod load order. Documented for modders in `Mods\SCRIPTING.txt`:
  - Events `map`, `level` (the level is ready, after every map load, savegame load and reload) and `tick`; timers
    `after`, `every`, `cancel`, and coroutines with `run`/`wait`.
  - `player()`, `level()`, `map()`, `time()`, `actors(class)`, `objects(class)`, `class(name)`.
  - Objects read and write any UnrealScript property by name (`sam.Health = 50`):
    numbers, bools, names, strings, objects (class-checked), structs as tables, static arrays as lists, dynamic
    arrays read-only. Their UnrealScript functions are methods (`obj:Function(...)`), script or native; out
    parameters come back as extra results. `valid`, `isa`, `nameof`, `classof`, `fullname`, and `dump(obj, filter)`
    to log an object's properties and functions.
  - `key(name)` / `pressed(name)`, `vec(x, y, z)` with vector maths, `print` (to the log), `require`, `shared`.
  - Each mod has its own globals. The sandbox has no io, os.execute, debug library, dofile/loadfile or compiled
    chunks; a call running over a second is stopped, a handler failing 10 times in a row is switched off, and using a
    destroyed object is an error rather than a crash. Ctrl+F11 (`ReloadScriptsKey`) reloads every script.
  - Engine side: property and function layouts come from the engine's own reflection (FindField, the property
    classes, UProperty::Serialize, ProcessEvent); script functions are called through ProcessEvent, natives with
    their arguments as bytecode constants or local variables.
- Lua 5.4.9 (MIT) is compiled into `d3d9.dll` (`third_party/lua`). The package adds `Mods\SCRIPTING.txt` and an
  example script in `Mods\_Example\Scripts`.

## 1.0.0-dev.102
- **Mods folder** (mod support, phase 1; `[Mods] Mods` 1). Every folder in the new `Mods\` folder next to `System\` and
  `Data\` is a mod. Its `System\` and `Data\` folders mirror the game's: a file with the same path as a game file
  replaces it whenever the game (or Bink, for videos) opens it, and a file the game doesn't have is added to the
  game's folder listings, so a new package in `Data\Maps` or `Data\StaticMeshes` is found like a shipped one. The
  game's own files are never touched.
  - `mod.ini` (optional): `Name`, `Author`, `Version`, `Description`, `Enabled`, `Priority`. When two mods replace
    the same file, the higher priority wins. Folders starting with `_` are ignored.
  - `.ini`, `.log`, `.exe`, `.dll` and `.asi` files can't be replaced (the ini files hold the player's settings).
  - The log lists the loaded mods with how many files each replaces and adds, and (`LogModFiles` 1) each file a mod
    provides the first time the game opens it (`[MODS]`).
  - Without any mod files nothing is hooked. With them, only read-only opens of files a mod provides are redirected,
    and only listings of folders a mod has files in are rebuilt.
  - The package now includes `Mods\README.txt` and an example mod, `Mods\_Example`.

## 1.0.0-dev.101
- **CLUSTERFUCK now shows in the difficulty menu.** The row was added in dev.100, but the list only draws as many
  rows as its page allows (3), so it sat out of sight below EXPERT. The list now shows 4.

## 1.0.0-dev.100
- **New difficulty: CLUSTERFUCK** (`[Gameplay] Clusterfuck` 1). It's a fourth row after EXPERT in the difficulty
  menu, Expert underneath. The mod keeps track of each CLUSTERFUCK campaign and its saves in
  `System\SCCTRemaster_Campaigns.ini`. On a CLUSTERFUCK campaign:
  - **Guards:** every guard rolls paranoid, sharp-eyed or veteran, with elite stats on top (225 HP, top accuracy,
    gas immunity). All have vanilla night vision, so darkness doesn't hide you from them, and flashlights. Their
    clips never run out and they never reload. They draw twice as fast, react 40% faster and settle their aim
    twice as fast. A call-out takes a quarter of the time and reaches every guard in the level. No warning shots.
  - **Alarms:** base alert at the start of every mission that has an alarm system, raised one alarm at a time
    through the game's own routine (armour from the 2nd alarm, helmets from the 3rd). Any body a guard finds raises
    an alarm 1.5 s later unless you stop him. Lighthouse, Seoul One and Seoul Two have no alarm system.
  - **The world:** knocked-out guards wake up after 2.5 minutes. Alerted guards search forever. Cameras detect you
    instantly, and turrets fire at once, faster, harder and without spread. Lights put out with the OCP are
    investigated like shot-out ones. Every wall mine you defuse goes off.
  - **Sam:** 50 HP, no medkits, half the starting ammo, no ammo pickups, no light or sound meter. Footstep sync and
    noise cover from gunfire are off; footsteps are as loud as normal.
  - **Hacking:** every hack is the hardest kind: 10 s and no locks, so you have to find the right address.
  - **Comfort fixes off:** the Bathhouse ending guards come in at once again.
  - **Ironman:** the Save page and F5 are refused; the mod saves once at the start of every mission
    (`CLUSTERFUCK <campaign> <map>`).
  - **Permadeath:** when Sam dies, every save of that campaign (only that one) is deleted through the game's own
    delete, so it also leaves the Load list. A `REST IN PEACE - CLUSTERFUCK <campaign>.txt` takes their place in the
    save folder: where and how he died, who killed him, how far he got, his last mission's stats. Retry is refused
    after the death.

## 1.0.0-dev.98
- **Guards duck under fire** (`[Perception] SuppressionDuck` 1.2, `SuppressionDuckLevel` 0.6; 0 = off). Your near
  misses and ricochets already pinned guards and threw off their aim (dev.87); now heavy fire also makes them
  duck. When a guard's suppression reaches 0.6 (about two near misses close together), he keeps firing from a
  crouch for about 1.2 s, and keeps his head down while the fire keeps coming. His shoot routine sets his stance
  every frame from its own stand/crouch setting, and the mod switches that to crouched while he's suppressed,
  the same as when the game tells a guard to fire crouched, so his crouched animations follow. The crouch blend
  from dev.86 smooths the change. Log: `[COMBAT] ... ducks under Sam's fire`.

## 1.0.0-dev.97
- **Fixed guards firing every frame** (`[Perception] NPCShotInterval` 0.066, 0 = original). A guard shooting at a
  spot he can't see into, like you behind a closed sliding door, restarted his burst on every AI tick, and every
  restart fired a shot at once: one shot per frame. That's 30 a second at the consoles' 30 fps, and about 150 a
  second each at high frame rates (in the Kokubo Sosho log, two guards fired 7,000 shots into a door in 30
  seconds). Now an NPC's shots are at least 0.066 s apart; normal automatic fire is about 0.07 s, so it's
  unaffected.
- The per-shot `[COMBAT] ... shoots at Sam` lines are limited to about 6 a second; the shots in between are
  counted into the next line.

## 1.0.0-dev.96
- **Fair hacking** (`[Gameplay] FairHacking` 1, 0 = original). Your failed hacking clicks were all on the address
  fragments, and the hack had no "locks" to lock them with. The game picks one of 9 hack difficulty levels (8 locks
  and 26 s down to 0 locks and 10 s) from the device, the mission difficulty, the EEV, and how often you've
  already hacked that device. On Expert, the hardest devices get 0 locks, and a fragment click then silently does
  nothing. Now:
  - locks never run out: a highlighted fragment always locks when you click it;
  - hacking a device again is no harder than the first time;
  - failing or timing out of a hack still sounds the alarm, as before.
- `[HACK] hack started: difficulty level N of 0-8, x s, n locks` when a hack begins.

## 1.0.0-dev.95
- **Hacking: clicks on the IP's digit boxes use the box under the cursor too** (they had the same stale-hover
  problem as the numbers in dev.93), and every left click on the hacking screen is now logged: the button press,
  what it landed on (a number, a digit box or nothing) and why the game ignored it if it did. In the dev.94 run
  none of the three failed clicks reached the number check, so this shows where they go.

## 1.0.0-dev.94
- **Fixed HUD icons flicking back and forth across the screen** (the green objectives crosshair and the notes
  icon in the bottom-right panel, and some small bars in the top-right one). The widescreen HUD leaves the
  menu cursor where it is when it passes through a corner panel, and it recognised the cursor as any small image
  whose corner or centre sat on the cursor point. During play the game keeps a hidden cursor that sweeps across
  the screen as you turn, so each panel icon it touched was drawn at its old 4:3 spot for a frame, about 300 px
  to the left. Now only the cursor sprite itself (white, about 32x32) is left in place.

## 1.0.0-dev.93
- **Hacking: a click always picks the number under the cursor.** The game doesn't confirm the number your click
  lands on: it confirms the one it last registered as "hovered", which only changes when the mouse moves (and
  only over an unlocked number). Click a number that had scrolled under a resting cursor, or one the cursor had
  only just reached, and the game confirmed the previous number instead, counting it as a wrong pick. Now the
  number under the cursor at the click becomes the hovered one first. `[HACK]` lines show each click, and when the
  game had a different number as hovered.

## 1.0.0-dev.92
- **Bathhouse ending: held-back guards don't teleport in next to you** (`[Gameplay] BathhouseSpawnClearance` 6,
  0 = off). Once his 15 s are up, a guard waits until you're at least 6 m from his spawn point. In testing, the
  last guard (at the exit, after the third bomb) appeared 4.5 m from Sam.

## 1.0.0-dev.91
- **Bathhouse ending: only the guards wait now.** In dev.90 the whole script paused for the 15 s, so Shetland
  turned up late and the next bomb was planted late. Now just the guards' part (their teleport and their walk/
  patrol orders) is held back and run 15 s later; Shetland and the bombs keep the original timing.
- A save made during that wait no longer loses the guards: after loading, any guard the script has already passed
  but who is still waiting under the level comes in 15 s after the load.

## 1.0.0-dev.90
- **Guards' first shot no longer always hits.** Since dev.86 the hook that removes the original's guaranteed first
  hit never went in: its search pattern matched two places in the game, so it was skipped (the log said "first
  shot always hits (hook not found)"). Warning shots never worked for the same reason. Both work now.
- **Fair hit roll** (`[Perception] FairHitRoll` 1). Underneath, the game's hit roll is a rubber band: a guard who
  hasn't hit you in his last few shots is *sure* to hit, however low his chance (in the Bathhouse log a 3% shot
  hit and killed you). So all the aim factors (settling, you moving, darkness, suppression) did nothing for a
  guard's first shots. Now a shot hits with exactly its chance.
- **A guard's aim settles from when his gun is up.** It used to count from when he spotted you, so the gun raise
  used most of it up. Now his first shot after raising his gun is at about a fifth of his accuracy, and his aim
  reaches full over the next second or so.
- **Bathhouse ending: the elite guards come in 15 s later** (`[Gameplay] BathhouseEndingDelay`, 0 = original).
  During Shetland's bomb sequence in the furnace room, the script teleported elite Displace guards in at the
  entrance the instant you defused each bomb (and one at the exit after the last). Now the script waits 15 s
  first. Shetland's walk to the next bomb waits too, but that bomb's timer only starts once he has planted it.
- `[COMBAT] ... shoots at Sam: HIT/miss (chance ...)` for every guard shot, and `[SCRIPT]` lines for every scripted
  teleport (which script, where to, who).

## 1.0.0-dev.89
- **Call-outs: a guard who finds you again has to call it in again.** A guard who had been in a fight earlier
  passed every new sighting on to his mates instantly, so killing him a second after he noticed you didn't help:
  his mates already knew. (In the Bathhouse log: a soldier from a fight 15 s earlier heard you, and the guards around
  him were told in that same instant, before he'd even turned round.) Now once a guard hasn't had you for 3 s
  (`[Perception] CallOutAgain`, replaces `CallOutReset`), finding you again is a new call-out, even mid-fight; only
  while he keeps you in sight do his updates go out at once.
- **Hitting a guard holds his call-out back** until he recovers from the flinch, so a guard you're shooting can't
  get the words out.
- **Call-outs take a bit longer:** `CallOutTime` 2 -> 2.5 s (x0.6-1.6 by personality: quick, alert guards
  about 1.5-1.8 s, average 2.5 s, lazy ones over 3 s).
- New `[ALERT]` log lines for a guard who passes it on at once (fighting with you in sight) and one who joins a
  group that is already fighting.

## 1.0.0-dev.88
- **Fixed a crash in the Bathhouse** (and anywhere else it could happen). When two guards block each other's path
  (in the Bathhouse, a bodyguard trying to push past another during the alarm call), the game swaps their AI goals,
  and the original code wrote through a null pointer when one of them had no goal to hand over. The swap is now
  skipped in that case and the game carries on; a `[FIX]` line in the log marks each time it happens. This is a bug
  in the original game, but dev.86's gradual sprint start may have made guards bunch up into it more often.

## 1.0.0-dev.87
More fair-fight behaviour for guards, on top of dev.86:
- **Flinching** (`[Perception] FlinchTime` 0.6). A guard you hit can't fire back for about half a second, and his
  aim has to settle again afterwards.
- **Suppression** (`SuppressionAim` 0.6, `SuppressionDecay` 1.2, `SuppressionPin` 0.3). Your shots landing around a
  guard (near misses, ricochets) throw his aim off and pin him: each near miss stops him firing for 0.3 s and builds
  a suppression level that cuts his hit chance (down to 0.4x when fully suppressed) and fades over a second or two.
- **Warning shots** (`WarningShotChance` 0.5, `WarningShotRange` 8, `WarningShotPause` 0.8). A guard who spots you
  at least 8 m away, while you haven't fired on anyone in the last 10 s, sometimes fires a deliberate burst past you
  first, then pauses before firing for real.
- **Darkness and goggles** (`DarknessAim` 0.7). The darker you are, the more guards miss, from 3 m and fully at 10 m
  (down to 0.3x in deep shadow). Guards wearing night vision or thermal goggles aren't affected.
- **Wounds** (`WoundedAim` 0.5). A hurt guard aims worse (at half health: 0.75x).
- `[COMBAT]` log lines show flinches, suppression, warning bursts and every factor in a guard's hit chance.

## 1.0.0-dev.86
Fairer guards in a fight:
- **Guards actually have to aim** (`[Perception] AimSettleTime` 1.2, `AimTrackSpeed` 0.8, `AimWhileMoving` 0.6,
  `FirstShotHit` 0). The original doesn't aim at all: whether a guard's shot hits is a steady quota (his accuracy,
  x2.5 within 1 m), and his first shot always hits. Now his hit chance starts at a fifth when he gets you in his
  sights and builds up to normal over about a second, drops the faster you move across his view (halved when
  running sideways 3 m away), and is lower while he's moving himself. No more guaranteed first hit.
- **Reaction time in combat** (`CombatReaction` 1, `ReacquireAfter` 0.3). Peeking out or stepping round a corner
  no longer gets you shot that instant: a guard who gets you back in his sights after you were out of them for
  0.3 s needs his reaction time (about 0.3-0.6 s by personality) before he can fire.
- **Sprint windup** (`NPCRunAccel` 450). Guards reached a full run in about a quarter of a second. Above patrol
  pace their speed now builds up gradually (walk to full run in about 0.6 s).
- **Crouch/stand blend** (`StanceBlendTime` 0.35). Guards crouching or standing up while standing still snapped
  into the other pose in one frame; they now blend out of the old pose.
- Logged with `LogPerception = 1`: `[COMBAT]` (reaction holds, hit chance changes) and `[STANCE]` (blends).

## 1.0.0-dev.85
- **Turning now costs accuracy by how fast your view really turns** (`[Mouse] AimFreeTurnSpeed` 90,
  `AimFullTurnSpeed` 360). Even with dev.84 the game judged raw mouse input: the threshold sat at an input level
  that, while aiming (the aiming camera turns slower for the same input), is a fairly slow turn on screen, so
  modest movements still blew the shot. Now it measures how fast your view actually turns over 0.1 s, in degrees
  per second: no cost at all below 90 deg/s, rising to the weapon's full penalty at 360 deg/s. Small back-and-forth
  corrections cancel out. Both limits can be set in `SCCTRemaster.ini`.
- `[AIM]` log lines (`AimLog`, on) each time turning costs accuracy: how fast the view turned, the penalty, and
  what the original would have charged, so the limits can be tuned from real numbers.

## 1.0.0-dev.84
- **Moving the mouse no longer wrecks your accuracy at high frame rates** (`[Mouse] AimAccuracyFix`, on). Your
  gun gets less accurate when you turn the camera, judged by how fast it turned during one frame. At 30 fps a
  tiny adjustment is spread over 33 ms; at 160 fps (worse with high polling rate mice or high sensitivity) the
  same nudge lands in a single 6 ms frame, reads more than five times faster, and the camera then eased down from
  the maximum, so the slightest touch threw your accuracy out. Accuracy is now judged by the mouse speed over the
  last 1/30 s, exactly what one 30 fps frame saw, at any frame rate. (A gamepad works as before.)
- **Perfectly still aim gives perfect shots** (`[Mouse] PerfectStillAim`, on). The weapons never got better than
  their base accuracy (SC-20K 0.5, pistol 0.8), which still scatters shots a little. Now, with the mouse completely
  still (not the faintest movement), accuracy keeps settling past that until shots go exactly where you aim
  (about a second). Any mouse movement brings the usual small imperfection back at once.

## 1.0.0-dev.83
- **UAV camera heads no longer snap onto you** (`[Perception] UAVTurnSpeed`, 0.6). The drone's camera head turns
  toward what it's looking at by a fixed step every frame (attacking: 27 degrees a frame), so at 160 fps it swung 90
  degrees onto you in 0.025 s (0.11 s at the original 30 fps). It now turns by time, at 0.6 times the original
  30 fps speed (90 degrees in 0.18 s when attacking), and slower again while the drone is drawing on you, like the
  guards. Idle scanning still keeps up with its sweep.
- The frame-rate scan missed this one because the step goes through the engine's "turn at most this much" helper
  rather than a blend. Every other caller of that helper was checked: they all scale the step by the frame time.

## 1.0.0-dev.82
- **Scanned the whole game for frame-rate-dependent code.** All 2.9 million instructions of the game code were
  searched for the patterns that make behaviour depend on frame rate: values blended or decayed by a fixed fraction
  per update, rotations stepped a fixed amount per tick, timers counting frames, random rolls made every tick, and
  integer easing. All 264 per-tick scripts (Tick, PlayerTick, PlayerMove...) were checked the same way. What it
  found:
  - Already fixed: guard turning (dev.77/dev.80, three places) and your noise level fade (dev.81).
  - **Fixed now:** guards firing at a spot rather than at you (suppressive fire) re-scattered their aim 20% of
    ticks, so five times as often at 160 fps. It now goes at the original rate.
  - Fine as they are: the physics world steps a fixed 1/60 s as often as real time needs; AI timers, Sam's
    movement and climbing, the sticky camera, the UAV's scanning sweep and guard idle fidgets all use the frame
    time.
  - Left alone (too small to matter): a 30% per-tick roll that makes a shooting guard crouch once his 5 s cooldown
    is over (about 0.1 s at 30 fps vs 0.02 s now), and the optic cable's purely visual jitter.
- **UAVs:** they turn through the same AI turning code as guards, made frame-rate independent in dev.80, and your
  dev.81 log shows no fast turn by the UAV's body. A hovering drone faces you mostly by pitching down, which the
  turn watch didn't look at, so `[TURN] ... pitched N deg` lines are now logged too (with `LogPerception = 1`).

## 1.0.0-dev.81
- **Your noise level fades at the intended speed at any frame rate** (`[Perception] NoiseDecayRate`, 30). After
  each noise you make, your level on the sound meter fades by 10% every frame, a rate tuned for 30 fps. At 160 fps it
  faded more than five times too fast (halving every 0.04 s instead of 0.22 s), so your noises stopped counting
  almost at once. That level is what ambient noise (machinery, thunder, and now guard noise) has to beat to cover
  you, so you were covered more easily than the game intended. It now fades by time. You'll see your bar on the
  meter linger a little after a loud step or landing.

## 1.0.0-dev.80
- **The rest of guard turning no longer depends on frame rate.** dev.77 fixed two of the three places that turn a
  guard; the third (the goal animation step that the shoot goal and most others run every tick, so it covers
  shooting, searching and patrolling) still turned him a quarter of the way every frame. Your dev.79 log caught a
  guard turning 166 degrees in 0.35 s with it, and a search turn jumping 41 degrees in one frame after a pause.
  All three now step by time (each as fast as at the original 30 fps; a shooting guard takes two steps a tick, as
  in the original), and no single step covers more than two frames. This should also cure the guards that
  snapped round toward a direction while you sneaked up on them.
- Confirmed in your playtest: draw time and gun raise feel natural.

## 1.0.0-dev.79
- **Guards draw a gun they had put away** (`[Perception] GunDrawTime`, 0.6 s). Relaxed guards (the "normal"
  stance) carry no gun in their hands, and the game just put it there in an instant when they engaged, so dev.78's
  raise couldn't hide it. Now such a guard plays his holster animation backwards on his upper body (his hand brings
  the gun round to the front), then raises it as in dev.78. He shoots only once it's up.
- The raise now plays above every animation layer the game uses (channel 19). In dev.78 it shared a channel with
  facial expressions, and the relaxed walking-arms layer above it partly covered it.
- Fixed: a guard who was knocked out before he could fire could, when he next spotted Sam, open fire at once
  without drawing (left-over state from the earlier fight).
- `[DRAW]` lines show where a guard's weapon was when he spotted Sam (in hand, put away, hidden, which bone).

## 1.0.0-dev.78
- **Guards actually raise their gun** (`[Perception] GunRaiseTime`, 0.7 s; `GunRaiseDelay`, 0.1 s). The game has
  no draw animation: when a guard engaged you, his pose switched from gun-down to gun-up in under 0.2 s. Now a guard
  whose gun was down keeps his upper body in his own gun-down pose for a moment, then brings the gun up over 0.7 s
  (0.4 to 1.1 s by personality), with his legs free to turn or walk meanwhile. He doesn't shoot until it's up.
- `[DRAW]` log lines say when a guard raises his gun and from which pose.

## 1.0.0-dev.77
- **Guards no longer snap round at high frame rates** (`[Perception] TurnRate`, 30). Guards turn to face what
  they look or aim at by a fixed step every AI tick, tuned for 30 fps. At 160 fps that made a guard who spotted you
  swing round onto you in 0.05–0.08 s with his gun coming up at the same time, which is why the draw still looked
  instant in dev.76. Turning now goes at the same speed whatever your frame rate (90% of a turn in 0.27 s), which
  may also be behind the guards you saw snapping toward a direction.
- **Guards turn slower while drawing on you** (`DrawTurnSpeed`, 0.6): from the moment a guard spots you until his
  first shot he turns at 0.6 times that speed (90% in 0.45 s), so you see him bring his gun round onto you.
- With `LogPerception = 1`, `[DRAW]` lines now also show how a guard's gun comes up (his gun-up flag, wait
  animation and animation channels when he spots you and over the next half second).

## 1.0.0-dev.76
- **Guards raise their gun before they shoot** (`[Perception] DrawTime`, 1 s). In the original a guard who spotted
  you fired in the same instant, at close range before you could do anything. Now his first shot comes 1 s after
  he spots you (0.6 to 1.6 s depending on his personality: paranoid guards are quickest), and he has his gun up and
  on you for at least 0.35 s first (`DrawAimTime`). That's your moment to take him down, duck out of sight or run.
  Once he's shooting he keeps his normal rate of fire; after 5 s without shooting at you (`DrawReady`) he has to
  draw again. A guard who has been tracking you for a while (gun already up) only needs the short aim.
- **Guards have to call it in before the others know** (`[Perception] CallOutTime`, 2 s). In the original, the
  moment one guard saw you, every guard of his group in the same area knew where you were and came at you in the
  same frame. Now the guard who spots you keeps it to himself for 2 s (1.2 to 3.2 s by personality) before the
  guards around him hear about it. Knock him out, kill or grab him before then and nobody hears it from him. His
  gunfire is still heard as before, and a guard who is already in a fight, or called it in within the last 20 s
  (`CallOutReset`), passes it on at once.
- Logged with `LogPerception = 1`: `[DRAW]` (when a guard has his gun on you and when he opens fire) and `[ALERT]`
  (when he spots you, and whether he called it in or was stopped first).

## 1.0.0-dev.75
- **Noise pollution: guards drown out your noises** (`[Perception] NoisePollution`, on). In the original only placed
  sources (machinery, sound triggers, thunder) could cover your sounds; now what guards do counts too:
  - **Gunfire** and other loud noises (explosions, grenades, shattering, alarms) at the weapon's or noise's own level,
    fading over 1 s (`GunfireFade`), so sustained fire keeps covering you and a single shot gives you a moment.
  - **Footsteps** of every walking (`GuardWalkNoise` 20 dB) or running (`GuardRunNoise` 35 dB) guard, out to 8 m
    (`FootstepMaskRange`).
  - Sound falls off 6 dB per doubling of distance. A guard who is shooting, walking or running hears less himself
    (his own noise is right on top of him), and guards near him hear less too.
  - It covers the noises your body makes (footsteps, landings, whistling, your voice, your gunshots). Distractions
    you throw are judged as before.
  - **On the HUD sound meter:** guard noise around you raises the meter's ambient level. While it's above your noise
    level, no one hears you, the same rule the game uses for machinery.
- **Footstep sync: copy a guard's pace and he covers your footsteps** (`[Perception] FootstepSync`, on). Walk at
  the same speed (within 20%, `SyncSpeed`) and heading (within 25°, `SyncAngle`) as a moving guard within 6 m
  (`SyncRange`), and after 0.75 s (`SyncTime`) you're in step: your footsteps make no noise anyone can hear, and the
  sound meter's ambient level sits just above them. Falling out of step (a corner, a pause) gives you half a second
  of grace.
- Logged as `[NOISE]` with `LogPerception = 1`: who didn't hear which noise and what drowned it out, and each time
  you fall into or out of step.

## 1.0.0-dev.74
- **Diagnostics for guards snapping round instead of turning** (with `LogPerception = 1`): sometimes a guard
  swings to face a new direction in an instant, which can get you caught while sneaking up on him. Every time a
  guard turns 60 degrees or more within 0.15 s, the log now gets a `[TURN]` line with how far and how fast he
  turned, where you were relative to his facing, the game code that set his rotation, his AI states, the
  animations he was playing and the events he had just received (e.g. a footstep he heard). No behaviour change
  yet: this is to find what snaps him round so it can be smoothed.

## 1.0.0-dev.73
- **Guards fighting each other keep their eyes on the fight** (`[Perception] Focus`, on). In the Bathhouse private
  baths, the Displace mercs and the ISDF fight while you get away, and the original made that near impossible: they
  still turned on you at the first glimpse or footstep. Now a guard who is fighting another NPC (one that was in his
  sights or hurt him in the last 3 s, `FocusMemory`) doesn't notice you and ignores every noise you make, unless
  you're within 4 m (`FocusRange`) and 30 degrees (`FocusCone`) of where he's facing, or for 5 s (`FocusBreak`)
  after you shoot at him or hurt him. It goes by each guard's own enemies, so it works in any map or mod where
  factions fight. Logged as `[FOCUS]` with `LogPerception = 1`.
- **Peeks can be picked up in 0.15 s** (`PeekReactionMin`, was 0.25): a guard already looking at the spot reacts
  faster than the 0.25 s a first sighting takes.

## 1.0.0-dev.72
- **Guards have reaction times** (`[Perception] Reaction` in `SCCTRemaster_Guards.ini`, on). Originally a guard
  who saw you clearly attacked in the same instant (shout, cover or open fire), and in a fight he shot the moment
  you peeked out. Now:
  - Every guard needs **0.25 s** (`ReactionMin`, the average human reaction time: nobody is faster) plus his
    personality's `Reaction` of seeing you before he reacts: veteran +0.05–0.10 s, paranoid +0.05–0.15,
    sharp-eye +0.10–0.15, average +0.20–0.30, noob +0.30–0.45, lazy +0.35–0.50 (technicians +0.05, scientists
    +0.15). Rolled per guard, a little different each time.
  - The harder you are to make out, the longer it takes: x1.25 mostly visible, x1.6 partly, x2.2 barely.
  - Out of view before he reacts and he never saw you. If he was at least half way there, he goes to check where
    he saw you (`Glimpse`).
  - **Peeking:** once he knows you're there (fighting you, or saw or heard you in the last 10 s), coming back into
    view after 0.5 s out of it needs 60% of his time (`PeekReactionFactor`), at least 0.25 s (`PeekReactionMin`).
  - With `LogPerception = 1` each one is logged: `[REACT] guard 9.1 m from Sam (...) saw Sam: reacted after
    0.41 s`.

## 1.0.0-dev.71
- **The cursor no longer skips on the right side of any screen** (the mission briefing, and the cursor some
  pages show during play), the same fix the hacking screens got in dev.56. The widescreen HUD moves the
  bottom-right and objective panels out to the screen edge by recognising small white images there, and the
  cursor is one too: whenever it entered those areas it was drawn 100 units further right. dev.61 had limited
  the cursor exception to hacking/keypad screens. The game's "in a menu" flag really means "a menu video is
  playing", so a briefing with no video running counted as gameplay. Now the cursor image of any page is
  recognised by sitting exactly on the cursor point. A HUD icon that an unseen cursor merely passes over still
  moves out, so the dev.61 fix stays.

## 1.0.0-dev.70
- **Work lamps only light you where their beam reaches.** The guards' light maths lit a spotlight's whole frame,
  right up to its edges, at full strength: the Penthouse alley work lamp, pointed at the garage door, counted
  Sam as lit at 110 (of ~120 "fully visible") in a spot that was dark on screen, and 0 just 75 cm further.
  Now a spotlight's light on Sam fades like the round beam you see: full in the middle 40% of its frame, nothing
  at the edge (`SpotlightBeam = 1`, `BeamCore = 0.4`). Shadow detection uses the same beam.
- **Partial cover works now.** dev.69 looked for Sam's light points in the wrong place, so it fell back to his
  middle and chest. It now uses the actual list of bones the game measures Sam's light at.
- **The log says what made a guard react.** With `LogPerception = 1`, every AI event a guard within 30 m gets is
  logged by name, e.g. `[EVENT] guard 12.3 m from Sam (...) got AI_SEE_ENEMY_BARELY (72) about Sam`.

## 1.0.0-dev.69
- **Partial cover: a lit foot behind the trash bags no longer gives you away.** The game judges how lit Sam is
  from four points on him (head, spine and both feet) and averages them; any guard who can see Sam at all gets
  that whole value. Crouched in the dark Penthouse alley corner, one foot reached a lamp's light, so the meter
  read 27.5 (a quarter of the lamp's 110) while the rest of Sam was dark, and the guard investigated. Now each
  guard only counts the points he can actually see, with the same line check as the guards' own sight test
  (`PartialCover = 1`). Directional lighting now uses the same four points as well, so a lamp at Sam's side also
  counts less there (before, it was measured at his middle and chest and missed a lit foot completely).
- **Backlighting is gentler and needs a real outline.** The background is now the average of the five lines
  past Sam instead of half the brightest one plus half the average, so one lit patch beside him counts for a
  fifth. The "hidden part" test from dev.68 now uses the guards' sight line check too (the old one went straight
  through the trash bags).

## 1.0.0-dev.68
- **Guards stop spotting your shadow from across the map.** With `ShadowDetection = 1`, every shadow-casting
  lamp counts, but the check only asked whether the guard could see the spot where your shadow lands, so a
  guard 18 m down the Penthouse alley noticed a shadow from a lamp and came over. The lamps the remaster adds
  now need the shadow within 10 m of the guard (`ShadowSightRange`) and the lamp to light the spot it falls on
  at least 40 on the light meter's scale (`ShadowMinLight`). The designers' own shadow lights are unchanged.
- **Backlighting ignores the parts of Sam the guard can't see.** The five lines past Sam (head, middle, legs,
  sides) used to count a lit wall behind him even when cover (trash bags, a crate) hid that part from the guard;
  now a hidden part doesn't outline him, and if all of him is hidden there's no backlight at all.

## 1.0.0-dev.67
- **Shader pack: floors stop glinting as you move.** The floor's reflection of its surroundings (which ignores
  how dark the spot is) now keeps only a quarter of the bump detail, is blurred and half as strong, and the
  lighting and highlights on bumpy surfaces calm noisy compressed bump maps before the maths, also up close, so
  a dark floor no longer sparkles when the camera moves.

## 1.0.0-dev.66
- **Shader pack: no more glittering floors.** Shiny floors (like the Bank stairwell's) add a reflection of a
  stored environment picture through their bump map, regardless of how dark the spot is; with compressed bump
  maps that reflection flickered from pixel to pixel into glitter. The remaster's version keeps the reflection
  bumpy up close but smooths it into a soft sheen wherever the bump detail is finer than a pixel (further away,
  shallow angles).

## 1.0.0-dev.65
- **Shader pack: bump base lighting** for surfaces like the Bank stairwell floor: the light's first pass now uses
  the exact light direction instead of the game's coarse lookup texture, so a light patch on a floor or wall lit
  at a shallow angle is no longer blotchy and stepped, and the normal maps' compression blocks are softened.
- Fixed: with the shader pack's F6 switch, the game could get stuck on the mission start screen (after the game
  re-created its graphics device, replaced shaders were applied to the wrong ones).

## 1.0.0-dev.64
- **Shader pack, first part: specular** (`[Graphics] ShaderPack`, Shader Model 1.1 path). The game's specular
  shaders looked the light and eye directions up in tiny 8-bit cube maps and faked the highlight's sharpness
  with a chain of clamped multiplies: harsh, banded highlights, and bumpy surfaces (compressed normal maps,
  some without mipmaps) glittered. The remaster's own versions (shaders/pack.hlsl, compiled into the mod: no
  extra loading or caching) use the full-precision vectors, a real specular curve matched to each material's
  glossiness, and smooth the highlight where the surface detail is finer than a pixel, so it no longer sparkles.
- **F6 switches the shader pack on and off live** (`ShaderPackToggleKey`), to compare the same view.
- **F11 shader highlight** (`ShaderHighlightKey`, for the shader pack work): paints one group of the game's
  shaders magenta per press (Shift+F11: one shader at a time), logged as `[HIGHLIGHT]`.
- `[Debug] LogShaderInputs` logs what each Shader Model 1.1 shader is given (for the shader pack work).

## 1.0.0-dev.63
- **Shader Model 3.0 rendering** (`[Graphics] ShaderModel = 3`). The game had been drawing everything with its
  oldest path, Shader Model 1.1: that's where the sparkly, blown-out specular on walls and stairs came from, and
  parallax mapping, HDR, tone mapping and soft shadows did nothing even when switched on. The mod now starts the
  game on its Shader Model 3.0 path (per-pixel lighting and specular, parallax mapping, HDR with tone mapping,
  soft shadows); the first level load shows "Caching SM 3.0 shaders". `ShaderModel = 0` leaves it to the game's
  own option.
- `[Debug] AutoCaptureAt`: capture a frame by itself some seconds after start (for diagnosing rendering).

## 1.0.0-dev.62
- **Guards sit down, fall asleep, use keypads and open doors properly again.** The logic rate ran guards at
  60 updates per second while doors and the rest of the world ran every frame, and their scripted sequences
  (sliding into a chair, stepping up to a keypad, opening a door, each waiting for a move or an animation to
  finish) could wait forever: sleepers stood next to their chair, guards kept walking into a door after typing
  its code or froze while opening it. Guards now update every frame like in the original (`ThrottleAI` and
  `ThrottleNPCs` default to 0); the logic rate only applies to physics props.

## 1.0.0-dev.61
- **HUD icons no longer shift around while looking around.** Some menu pages keep taking the (invisible) cursor
  during normal play, e.g. after an alt-tab, and the hacking-cursor fixes took that for an open hacking screen:
  whichever bottom-right icon the invisible cursor passed over was left in its 4:3 position. Only the hacking and
  keypad device screens count now.

## 1.0.0-dev.60
- **Hacking clicks register reliably** (`[Mouse] HackingClickFix`, on). The game handles a hacking click when the
  button is *released*, checking again what's under the cursor at that moment: with the numbers scrolling and the
  mouse moving on, that often missed (nothing happened) or caught another number. Now the click counts the moment
  you press, and if the cursor tip has just slipped off a number (or it just scrolled past), a number that was
  under it within the last 0.15 s still counts.

## 1.0.0-dev.59
- **Hacking selection after the cursor re-centres.** The game re-centres the hacking cursor (for example after a
  number locks in) without updating where it checks for the cursor, and each mouse event only refreshes its own
  direction: after moving only sideways, it still checked at the old height. While such a screen is open, both
  directions are now refreshed every frame.

## 1.0.0-dev.58
- **Hacking/keypad selection follows a resting cursor.** The game only checks what's under the cursor when the
  mouse moves, and with raw mouse input a still mouse sends nothing, so numbers scrolling under a resting cursor
  weren't picked up until you moved it again. While such a screen is open, the cursor is now re-checked every
  frame.

## 1.0.0-dev.57
- **Hacking/keypad clicks land where the cursor is.** Those screens pick the part under the cursor with a ray
  from the camera, which the game built for a 4:3 screen: the original field of view and the cursor spread over
  the full screen width. With widescreen the scene is rendered wider and the cursor is drawn in the centred 4:3
  area, so the ray missed the drawn cursor (mostly up and down, more the further from the centre). The ray now
  uses the rendered field of view and the drawn cursor's position.

## 1.0.0-dev.56
- **Hacking/keypad cursor no longer jumps on the right side of the screen.** The widescreen HUD moves the corner
  panels outwards by recognising small white images in the right-hand corner areas, and the in-game cursor is a
  small white image too: whenever it entered those areas it was drawn 100 units further right, then snapped back
  when it left. The cursor of an open hacking/keypad screen is now left where it is.

## 1.0.0-dev.55
- `WallBounceMax` is back to 1 by default (one kick per jump); set it to 2 for double wall jumps.

## 1.0.0-dev.54
- **Every wall-bounce kick looks the same.** The kick blended the split pose at half strength over whatever
  Sam's leg was doing, and by the second bounce of a double wall jump his in-air animation has switched to a
  straight-legged one, so the leg shot out stiff and wide. The kicking leg is now first held in the knees-bent
  pose of a running jump's first bounce, and the kick blends over that.

## 1.0.0-dev.53
- **Holding jump for a split is no longer triggered by accident.** It used to count any jump press, so the
  second press of a double wall jump snapped Sam into the split, and even a tap stopped him in mid-air over a
  split spot (losing the bounce boost). Now jump has to stay down for 0.2 s (`WallBounceSplitHold`), and after a
  bounce only the press that made the bounce counts: taps always just bounce.
- **Double wall jumps:** `WallBounceMax` now defaults to 2, so Sam can kick off a second wall in the same jump.

## 1.0.0-dev.52
- **Steam overlay works** (`[Display] SteamOverlayFix`, on): Steam loads its overlay into Chaos Theory but never
  starts the process that draws it, so Shift+Tab, the FPS counter and F12 screenshots did nothing. The mod now
  starts it a few seconds after launch when Steam hasn't (the same way Steam does for other games).
- **No more lag when a window pops up over the game** (ShareX screenshots, notifications, overlays): DXVK's
  latency sleep (`dxvk.latencySleep` in `dxvk.conf`) misjudged frame timing whenever Windows had to composite
  the game, making every frame take ~75 ms (about 40 fps) for as long as the popup was visible. It's now off;
  such a popup costs at most one slow frame.

## 1.0.0-dev.51
- **Cursor diagnostics** (`[Debug] LogCursor`, on for now): the log notes when an on-screen cursor (hacking,
  keypads, menus) jumps by more than the mouse moved it, or takes an unusually big step, to find the cause of
  the hacking-screen skips.

## 1.0.0-dev.50
- **Cursor in hacking/keypad screens no longer skips.** In-game screens with a cursor took its movement from
  the original game's mouse handling, which warps the Windows cursor back to the centre after every move: with
  high polling-rate mice that loses or doubles steps and drifts to one side. Those cursors now use the same
  raw mouse input as aiming, scaled by your Windows pointer speed, so they move like your desktop cursor.

## 1.0.0-dev.49
- **Standing footsteps as quiet as crouched ones** (`QuietStandingSteps`, on): guards hear Sam's footsteps the
  same whether he's standing or crouched; only speed (and the floor) makes a difference, so walking slowly
  upright past someone is as quiet as doing it crouched.

## 1.0.0-dev.48
- **Catching a fall with a split still hurts** (`SplitCatchDamage`, 0.5): if Sam was falling fast enough to
  take fall damage, the split costs him half of what that fall speed would do on landing (the game's own
  formula). It never kills him, so catching yourself is always worth it. Logged as `[MOVE] the split caught a
  fall at ... units/s: N damage`.

## 1.0.0-dev.47
- **Split snap needs jump held.** After a wall bounce, Sam only snaps into a split-jump spot while you keep
  holding jump; a tap just bounces, so wall jumps can be chained without splitting by accident.
- **Split from a fall** (`SplitFromFall`, on): holding jump while falling, Sam also snaps into a split-jump spot
  he falls past, without a bounce, e.g. to save himself from a long drop. After dropping out of a split, the
  spot you left won't grab you again until you land, so split, drop, split... works where the level allows.

## 1.0.0-dev.46
- **Bounce into a split jump: smooth.** Instead of appearing in the split pose instantly, Sam now glides into
  place over 0.3 s (position and facing eased) while the split pose blends in over the same time.

## 1.0.0-dev.45
- **Bounce into a split jump: straight into the pose, and more reliable.** The split's intro (Sam drops to the
  floor-level start and climbs up for 2.5 s) is skipped: once a bounce takes Sam over a split-jump spot he holds
  there and, at the split's height, is put straight into the split pose, centred between the two walls. The
  spot is also looked for up to 50 units either way across the corridor, since it is a narrow band and the
  bounce's sideways push often carried Sam just off it. Logged as `[MOVE] split placed at ...`.

## 1.0.0-dev.44
- **Bounce into a split jump, second try.** The split check only passes where a split from the ground starts
  (standing height above the floor), so in the air above the spot it failed, and the steering boost had
  carried Sam past the spot by the time he came down. Now the check is made as if Sam stood on the floor below
  him: once he's over a split-jump spot he stops drifting, drops to the split's starting height and goes into
  the split from there. Logged as `[MOVE] wall bounce 1 is over a split-jump spot` / `-> split jump`, or
  `no split-jump spot under the bounce`.

## 1.0.0-dev.43
- **Wall bounce into a split jump, for real.** It needed a second jump press on the exact frame Sam was inside a
  split-jump spot, and the steering boost usually carried him straight past it. Now, after a bounce, Sam snaps
  into the split as soon as he passes through a split-jump spot, on the way up or down, with no extra press.
  Logged as `[MOVE] wall bounce 1 -> split jump`.
- The crouch debug log (`[Debug] LogCrouch`) is off by default again.

## 1.0.0-dev.42
- **Wall bounce goes up, not back.** Running at a wall and bouncing pushed Sam away from it (150 units/s)
  unless you steered. Now a bounce only takes you away from the wall when you hold a direction away from it
  (or along it, for the boost); holding forward into the wall, or nothing, kicks you straight up it with just
  a small nudge off the wall, keeping your speed along it if you came in at an angle.

## 1.0.0-dev.41
- **Smooth crouch/stand while moving too.** When Sam walks or runs, a second animation layer (the jog,
  blended in by speed) covers the walk layer, and it switched stance with no blend at all, so crouch spam while
  moving still snapped. The instant switch now reverses that layer as well, and whenever the game itself
  switches a stance layer without a proper blend, the old pose is faded out from where it was (logged as
  `[CROUCH] smoothed a stance switch the game made`).

## 1.0.0-dev.40
- **Frozen-guard reports: no more false alarms from bodies.** The three Cargo Ship guards the log kept calling
  frozen (`EPeoplesVoiceSoldier5`, `6`, `21`) were knocked-out guards: their physics was off (bodies are), they
  never moved after the save loaded, and their search/follow/revive behaviour was simply left over from before
  they went down. The check only looked at the AI controller's state, which a knocked-out guard can keep at
  `s_Default` (after loading a save) while his body is unconscious. It now also checks the body's state and
  only reports guards on their feet. The `[AI]` lines show the body state too (`body EPawn.s_...`).

## 1.0.0-dev.39
- **Instant crouch/stand, smooth again.** After a few seconds of standing still the game blends in a "relaxed"
  idle pose on top of everything, and that one snaps between standing and crouched. The game fades it out at
  the start of every stance change; the instant switch now does that too, so each press blends smoothly from
  wherever Sam's pose is.

## 1.0.0-dev.38
- **Instant crouch/stand** (`CrouchInstantSwitch`, on). Found the cause of the stagger: after a stance change
  the old pose fades out over 0.3 s, and the game won't switch again until that fade is almost done, so a quick
  second press waited up to 0.3 s or was skipped. Now a press mid-transition reverses it at once, from wherever
  the blend is (no pop). Logged as `[CROUCH] switched to ... at once`.
- The crouch log shows animation names properly.

## 1.0.0-dev.37
- **Crouch-spam noise fix** (`CrouchNoiseFix`, on). Footstep noise comes from the walking animation's steps, so
  spamming crouch could skip steps and make Sam silent, the old Splinter Cell exploit. Now steps while crouch is
  being toggled count as standing steps, and a step that doesn't come while you toggle crouch is made anyway.
  The log says when either happens (`[CROUCH] ...`), which also shows whether crouch spam skips steps at all.
- **Crouch diagnostics** (`[Debug] LogCrouch`, on for now): for 2 s after each crouch toggle, the log lists
  Sam's animation layers, to find where the stand/crouch blend lives before making it reversible.

## 1.0.0-dev.36
- **Wall bounce only from a moving jump.** Sam has to leave the ground at `WallBounceMinSpeed` (200 units/s)
  or faster, so jumping up at something to climb it no longer kicks you off the wall. `WallBounceStanding = 1`
  allows standing bounces again, and they now go straight up (`WallBounceStandingUp` 850) instead of pushing
  you away from the wall.
- **Climbing over railings** (`ClimbOverRailings`, on). A bounce often leaves Sam hanging from a railing's top
  bar, where the game wouldn't let him climb (only let go). Now, hanging from any ledge the game won't hoist
  you onto, holding forward or jump climbs over it onto the floor beyond, with the game's own climb-over move.
  Logged as `[MOVE] climb over the ledge`.
- **Landing roll only when you crouch** (`RollOnlyWhenCrouching`, on). The original also rolled by itself after
  a big drop when you were moving forward; now Sam only rolls if you press crouch in the air or as he lands.

## 1.0.0-dev.35
- **Wall bounce: front-wall kick.** Bouncing off a wall in front of Sam (e.g. jumping straight up at it) no
  longer kicks a leg out sideways: the leg takes the pose of the ledge hoist where he puts a foot up, knee up
  and foot forward against the wall (`WallBounceFrontAnimRight/Left`, `WallBounceFrontFrame`,
  `WallBounceFrontStrength`). Walls to the side keep the split kick.
- **Wall bounce: boxes, crates and other scenery count as walls.** The wall check only saw the level's own
  walls; it now uses the same world trace as the game's scripts (characters still don't count).
- **Wall bounce steering** (`WallBounceBoost` 400, `WallBounceSteerTime` 0.3 s): hold a direction on the
  movement keys when you bounce, or just after, and Sam gets a speed boost that way, e.g. run at a wall, jump,
  jump, hold back = a boost backwards. Running alongside a wall while holding forward boosts you along it. Keys
  pointing into the wall keep the normal push.
- **Diagnostics for the railing problem:** for 4 s after a bounce the log records every change of Sam's state,
  physics and what he's standing on (`[MOVE] after bounce ...`), to see what grabs him on railings.

## 1.0.0-dev.34
- **Wall bounce kick: one kick, leg lower.** The leg now swings out once and back within `WallBounceAnimTime`
  (0.35 s) instead of staying out and replaying the animation. It goes only halfway towards the split pose
  (`WallBounceKickStrength = 0.5`), so it's angled down like in the older games instead of sticking straight
  out to the side; lower values keep it lower still.

## 1.0.0-dev.33
- **One-leg kick, found the cause.** The kick did play on the leg, but Sam's own in-air animation fades every
  other animation channel over 0.2 s, and it kicks in right after a bounce, so the leg was gone before it
  showed. The kick is now held at full strength for its whole time (`WallBounceAnimTime`), then fades out, and
  starts from its first frame on every bounce. Log: `[MOVE] leg kick after 150 ms: held at 1.00 (the game had
  faded it to ...)`.

## 1.0.0-dev.32
- **Wall bounce: much easier to do.** Sam no longer has to be moving into the wall: any wall within
  `WallBounceReach` (64 units) of his side, in any direction, works. Run alongside a wall, jump, press jump
  again and he kicks off it, like in the older games. He keeps most of his speed along the wall. The old
  rule that skipped narrow corridors is gone (Chaos Theory's split jump starts from the ground, so the two
  never clash).
- **Wall bounce into a split jump** (`WallBounceSplit`, on): after a bounce, press jump again in a
  split-jump spot and Sam goes straight into the split, higher up than a split from the ground. Logged as
  `[MOVE] wall bounce 1 -> split jump`.
- **Wall bounce height** 800 -> 700 (`WallBounceUp`).
- **One-leg kick, second try:** in dev.31 the kick played on one leg's channel but never showed. It now loops
  while held (like the whole-body version did), and the bounce checks right away that the animation is really
  on the leg; if not, the whole body plays it as in dev.30. The log shows which one happened, plus the
  animation channel's state 150 ms later, to track down the cause if the leg still doesn't move.

## 1.0.0-dev.31
- **Wall bounce: one-leg kick.** Only the leg on the wall's side kicks now (wall on your right, right leg);
  the rest of Sam keeps his jump pose. `WallBounceLeg = Swap` uses the other leg, `Both` brings back the
  whole-body animation of dev.30.
- **Wall bounce: much higher.** `WallBounceUp` 520 -> 800 and `WallBounceOut` 220 -> 150, so the kick sends
  Sam up like in the older games instead of mostly away from the wall. (Fall damage starts at a landing speed
  of 1000, so values much above ~850 can hurt him.)
- Internal: native script functions (the animation ones) can now be called from the DLL.

## 1.0.0-dev.30
- **Wall bounce** (`[Gameplay] WallBounce`, on by default): the wall jump from Splinter Cell 1 and Pandora
  Tomorrow, removed in Chaos Theory. Jump at a wall and press jump again while you're against it: Sam kicks
  off it upwards and away, to reach ledges a normal jump can't. Chaos Theory has no animation for it, so it
  uses the split jump's start (`WallBounceAnim`, try others). Tunable: `WallBounceUp` (520, like a jump),
  `WallBounceOut` (220), `WallBounceMax` (kicks per jump), `WallBounceAnimTime`, `WallBounceNoise` (the kick
  sounds like a jump, and guards can hear it). It never fires where walls are close on both sides, so the
  split jump keeps working. Logged as `[MOVE] wall bounce`.
- Internal: the script-calling helpers moved into their own module (used by the stuck-door fix and the wall
  bounce).

## 1.0.0-dev.29
- **Guards stuck against closed doors** (`[Gameplay] FixStuckDoors`, on by default). Found with the new F7
  report: a guard standing behind a closed sliding door whose AI flags read "will use door, door open". In
  the original AI, when a guard's hand reaches a door that isn't fully closed (still closing after another
  guard used it), he decides it's open and walks on; the door shuts and he walks into it for good. Now a
  walking guard who has stood still for 1.5 s right at a closed door his AI thinks is open gets the door
  opened the normal way (the door's own trigger, so locked doors stay locked). Logged as `[GUARD] ...
  triggered it to open for him`.
- The F7 report and the passive AI watch skip knocked-out guards and invisible stand-ins, and only flag
  "frozen" guards whose behaviour is in an alert state.

## 1.0.0-dev.28
- AI diagnostics for the door and "frozen guard" reports (with `LogPerception = 1`):
  - **F7** logs every guard within 40 m: his AI state and his behaviour pattern's state, current goal, enemy,
    speed, how long he has stood still, and his door bookkeeping (which door he's using, whether it's
    blocked, the door he remembers as blocked and since when, whether he may open doors at all).
  - The log flags on its own a guard who keeps trying to walk without getting anywhere for 3 s (`[AI]
    PUSHING`) and one who has an enemy but hasn't moved for 20 s (`[AI] FROZEN`).

## 1.0.0-dev.27
- **Hearing per guard** (`Hearing` trait in `SCCTRemaster_Guards.ini`, multiplied across faction, role and
  personality). Every noise in the game (footsteps, doors, objects, gadgets...) has a radius, and a guard
  hears it if he's within it, shortened by walls in between. Each guard now checks noises with the radius
  scaled by his hearing: veterans x1.2, paranoid x1.25, officers x1.05, bodyguards x1.1, technicians and
  scientists x0.85, sailors x0.9, noobs x0.8, lazy x0.75. Walls muffle exactly as before. Shown in the
  `[GUARD]` lines; with `LogPerception = 1` the log shows noises Sam makes near a guard whose hearing changes
  the outcome.

## 1.0.0-dev.26
- Stuck-guard rescue: reading a guard's original spot from the map file failed (the package header was read
  4 bytes off), so dev.25 spotted Lighthouse's top guard but had nowhere to put him. Fixed and tested against
  the real map file (the top guard's post: -3020, 1811, 2097).

## 1.0.0-dev.25
- Stuck-guard rescue fixed: it waited for a guard to be "falling" without moving, but Lighthouse's top guard
  keeps walking about in mid-air (no gravity where he ended up), so its 3-second clock kept restarting. Now
  it only checks the height: 3 seconds "falling" without dropping. The check also runs from Sam's tick every
  250 ms instead of the guard's own, and logs when it first spots one (`"falling" without dropping`).

## 1.0.0-dev.24
- Stuck-guard rescue: a guard who was already stuck when the save was made (never seen standing, no
  navigation anchor) is now put back where the level designer placed him, read from the map file
  (`Data\Maps\<map>.scl`). dev.23 found Lighthouse's top guard stuck but had nowhere to put him.

## 1.0.0-dev.23
- **Guards stuck in mid-air are put back** (`[Gameplay] RescueStuckNPCs`, on by default). Found in Lighthouse:
  the guard on top of the lighthouse (tagged "HighFaller") was walking around up there, and minutes later
  hung in the air 11 m above the sea, 32 m away, in a "falling" state that never progressed (the engine
  doesn't run a fall outside the level). Your saves show it happened between 03:54 and 03:56. The mission
  script drops the lighthouse light assembly 69 m in a tenth of a second whenever it hides it to save
  rendering, and a guard walking next to it can be flung out of the level. Since the mission's finish check
  needs him taken out, this could also block the ending. Now any character "falling" without moving for 3
  seconds is moved back to where he last stood (or his navigation anchor or home point, e.g. when a save was
  made with him already stuck), logged as `[GUARD] ... put back at ...`.

## 1.0.0-dev.22
- F8 (with `LogPerception = 1`) also lists every character in the level (`[CHARS]` lines), nearest to the
  centre of your view first: name and personality, position, distance and height from Sam, and whether it
  is hidden, has a mesh and was drawn just now. Added to identify a figure seen standing out on the sea at
  Lighthouse.

## 1.0.0-dev.21
- **Shadow sense per guard** (`Shadow` trait in `SCCTRemaster_Guards.ini`, multiplied across faction, role and
  personality like the others). A guard checks for Sam's shadow once per AI tick, one light at a time; this
  sets how often he does: noobs x0.25, lazy x0.4, paranoid x1.6, sharpeyes x1.4, veterans x1.3, scientists
  x0.6, technicians x0.8, officers x1.1. 0 = never notices shadows. Shown in the `[GUARD]` lines.
- **Shadow detection from every shadow-casting light actually works now.** The guards' shadow check tests
  the light's `bNPCShadowDetect` flag a second time before using it, so in dev.13-20 the extra lights were
  registered but always skipped. That test is now removed too (with `ShadowDetection = 1`).
- `LogPerception` logs every shadow sighting: which guard, his shadow sense, how far away the shadow was and
  which light cast it.

## 1.0.0-dev.20
- **Directional lighting** (`DirectionalLighting`, `DarkSide` in `[Perception]`). Sam's light level used to be
  one number for every guard, as if each lamp lit him from all sides. Now each guard sees the side of Sam
  facing him: Sam's light is split into the ambient part and each nearby lamp's part, and a lamp counts fully
  for a guard standing on its side, about half from the side, and only `DarkSide` (0.25) when it is behind Sam
  from the guard's view. The ambient part always counts. A guard on the lit side sees exactly the original
  light level, so this only ever makes Sam harder to see from his dark side. Backlighting is applied after it,
  so a lamp behind Sam darkens his front and outlines him at the same time.
- The HUD light meter still shows the overall level (all sides).
- `LogPerception` lines (and F8) show the breakdown: each light's part and its angle from the guard, the
  facing factor, then the background.

## 1.0.0-dev.19
- **Light sources behind Sam count for backlighting.** A lamp or fire right behind part of Sam's silhouette in
  a guard's view now counts at its own brightness. Before, a fireplace behind Sam read as dark: the fire
  light's line to the back of the hearth is blocked by the logs, so the engine's lighting says the surface
  behind the flames is unlit. A light counts when it is within 64 units of the guard's line past Sam, no
  further than the background, and Sam has a clear line to it (allowing for its own fixture). Logged as
  "light source behind him".

## 1.0.0-dev.18
- **Backlighting verified and widened.** The F8 checks confirmed the measurement: at Sam's own spot it
  matches the engine's own light formula, and a dark Sam in front of the lamp-lit archway (background 105-112)
  now counts as about 64 instead of 1. It used to look along a single line through the middle of Sam, which
  missed a bright patch behind only part of him (it hit the stone above the hearth, not the fire). Now it
  samples five lines through his head, middle, legs and left and right edges, and uses half the brightest
  plus half the average.
- Log lines show every sample (`head 105 (4.9 m, 4 lights), middle ...`).

## 1.0.0-dev.17
- **F8 backlight check** (only with `LogPerception = 1`): logs the backlight result as a guard standing at
  your camera would see it (your light level, the wall or sky beyond you, what you'd count as) plus the
  light-by-light breakdown. Backlighting only runs inside a guard's sight check, so before this it could only
  be tested with a conscious guard looking your way.

## 1.0.0-dev.16
- Backlighting diagnostics (`LogPerception = 1`): every 6 s the log compares, at Sam's own spot, the light
  meter, the engine's formula with Sam's own lights and the remaster's level-light measurement, then lists
  the wall point's nearest lights one by one (distance, range, whether the line to the wall is clear, what
  each adds). In dev.15 the wall behind Sam almost always measured near 0; this shows whether that is real
  darkness or a measuring fault.
- The light-set filter for backlighting is active (its code pattern occurs twice; it was skipped in dev.15).

## 1.0.0-dev.15
- **Peripheral vision: the cone was half as wide as it should be.** A guard's `m_VisibilityConeAngle` (60 for
  most guards) is the angle from the centre of his view to its edge, so he sees 120 degrees, not 60 (the
  game's own cone test compares against it directly). dev.13/14 treated it as the full width, so anything
  more than 30 degrees off-centre got the full edge penalty (x0.4). Sight now stays full up to 21 degrees
  (`PeripheralStart` 0.35 of 60) and fades to x0.4 at 60.
- Backlighting only considers lights the engine actually counts (point and spot lights, not other light
  effects) and uses the nearest 32 to the background point. The log now shows the light level Sam ends up
  with instead of a ratio.

## 1.0.0-dev.14
- **Backlighting fixed.** In dev.13 the background always measured as bright as Sam himself (`x1.00` in the
  log), so it never added anything. Sam's light-level function returns his cached value when asked the way
  the sight check asks, and otherwise uses only the lights touching Sam. The background point is now
  measured with the engine's own light formula using the level's lights within range of that point,
  filtered the same way the game picks the lights relevant to guards.
- **Peripheral vision fixed.** The guard's view direction was read as a rotation when the engine returns a
  direction vector, so the angle to Sam was wrong (55-144 degrees with Sam in plain view) and every check
  got the edge penalty (x0.4). It now uses the vector directly.
- `LogPerception` backlight lines now show Sam's light, the background's light, its distance and how many
  lights reach it.

## 1.0.0-dev.13
- **MGS2-style perception** (`[Perception]` in `SCCTRemaster_Guards.ini`). The game's sight check already
  scales Sam's visibility by light, movement (up to x2 at a run), distance and silhouette height. New:
  - **Shadow detection from every shadow-casting light** (`ShadowDetection`): guards notice Sam's shadow
    using the game's own shadow check, which was only enabled for a few lights in 9 maps.
  - **Peripheral vision** (`PeripheralVision`, `PeripheralEdge`, `PeripheralStart`): the original computed how
    far off-centre Sam is and then ignored it. Now sight fades from full in the middle of a guard's view to
    x0.4 at the edge. Movement still counts double, so motion at the edge is still noticed. Uses the guard's
    real eye position and view direction, head included.
  - **Crouched silhouette when standing** (`CrouchSilhouette`): standing is no easier to spot at a distance
    than crouching.
  - **Backlighting** (`Backlighting`, `BacklightStrength`, `SkyBrightness`, `SkyDistance`): a guard looks past
    Sam at what's behind him; if the background (a lit wall, a window, the sky) is brighter than Sam, his
    dark silhouette counts as light on him. The background is traced along the guard's line of sight and its
    light level computed with the engine's own lighting; open or distant background counts as sky.

## 1.0.0-dev.12
- **Savegame difficulty fix** (`[Gameplay] FixSoloDifficulty`, on by default), an original bug: loading a
  savegame dropped the level to the lowest difficulty. Continue/Load Game loads the bare map (no
  `MISSIONDIFF` option, so difficulty 1) and then reads the savegame, which restores the guards' own stats
  but never the difficulty it was started with (`MISSIONDIFF=3`, stored in the file). Everything the
  engine decides by difficulty at runtime then ran on the lowest setting after every load: Sam x0.75 as
  visible to guards, shorter guard awareness distances, medkits healing 3x, wall mines always defusable.
  The remaster now notices the savegame being read and restores its difficulty (verified: an Expert save
  loads on Expert). Guards whose health/aim were set for the wrong difficulty are corrected too (exact
  formula matches only). A network/co-op session can also no longer override a solo mission's difficulty.
  The log shows every step (`Difficulty:` lines).
- Save/load audit (`[Debug] SaveAudit`): Sam's health and maximum, every inventory item and gadget count,
  weapon ammo (clip and reserve) and guard personalities are all saved and restored correctly; the
  difficulty was the only thing a savegame lost.
- Guard personalities are now applied a moment after a level starts (after a savegame has been applied),
  so they use the right difficulty and aren't overwritten by the restore.
- Guard personalities: corrected field offsets for aim, sight range and field of view (the first build's
  safety check refused to change anything because the values it read were wrong).

## 1.0.0-dev.11
- **Guard personalities** (`[Gameplay] GuardPersonalities`, tables in `SCCTRemaster_Guards.ini`): every
  guard becomes faction-role-personality, e.g. `guerilla-technician-paranoid`. Faction comes from the class
  name (People's Voice = guerilla, Displace, SDF, North Korea, ...), so it follows the troops into any map;
  role from the rest of the class name (soldier, technician, officer, ...); personality (noob, lazy,
  average, paranoid, sharp-eyed, veteran) is rolled per guard, seeded from the guard's name so it never
  changes between loads. Each layer multiplies the guard's sight range, field of view, perception, search,
  aim and health, and can grant thermal vision or deafness. Difficulty only changes how common each
  personality is: good guards are rare on the lowest difficulty and make up most of Expert. Named characters
  are left alone. Savegames are safe: a guard is only adjusted once (a marker travels with the save).
- **Equal detection on every difficulty** (`[Gameplay] DetectionBaseline`, default 2): the engine made Sam
  x0.75 / x1 / x1.33 as visible and changed guards' sight limit and awareness distances by difficulty. All
  difficulties now use the chosen difficulty's values (1 lowest, 2 middle, 3 Expert; 0 = original).

## 1.0.0-dev.10
- Optic cable: now turns exactly as fast as the sticky camera (4x slower than dev.8, which was still far too
  fast with a mouse). `[Mouse] OpticCableSpeed` scales it.
- Standing vs crouched visibility, confirmed at runtime: a hardware watchpoint on `GameInfo.VisCrouchMul`
  saw no code read it during 38 s of play while standing and crouching near lights. The light meter
  difference comes from where light falls on Sam's body (his sample points are higher when standing), not
  from a stance modifier.
- Debug: `[Debug] WatchVisCrouchMul` (the watchpoint above).

## 1.0.0-dev.9
- Loading screens: the progress bar is now drawn inside the 4:3 frame too (it is drawn by its own routine,
  which still scaled to the full screen width).
- Debug: the F9 snapshot (`LogHudImages`) also logs every draw call with the game code that issued it.

## 1.0.0-dev.8
- **Loading screens at 21:9:** the loading screen background (a 640x512 image drawn in two pieces) was
  stretched to the screen width while the mission picture and text stayed in the 4:3 area, so half the
  picture frame was black and the generic loading image (when quitting a level) was stretched. The
  background now stays 4:3, like the menus (`[Display] StretchLoadingScreens = 1` restores stretching).
- **Optic cable speed:** the optic cable turned by a fixed amount per frame, so its speed grew with the frame
  rate (about 5.5x too fast at 165 FPS). It is now frame-rate independent, at the normal camera's top
  turn speed (`[Mouse] OpticCableSpeed`, 1.0; 0 = original).
- Standing vs crouched visibility: investigated, no change. The game's `VisCrouchMul` setting is never read
  by its code, and the guards' sight checks only use your stance for geometry (they aim at a lower point on
  a crouched Sam, so cover hides more of him). There is no flat stance modifier to swap.
- Debug: `LogHudImages` (press F9 to log the HUD/menu images on screen).

## 1.0.0-dev.7
- **Even diagonal speed** (`[Gameplay] EvenDiagonalSpeed`, on by default): fixes an original bug where
  holding forward plus a strafe key (W+A, W+D ...) made Sam move faster, and therefore noisier, than one
  key alone. The game sets each movement key's input separately and takes Sam's "pushing force" as the
  length of the combined vector, so two keys pushed with 1.41x the force. The pair is now scaled back to
  the strength of a single key, keeping the direction, at every mouse-wheel speed. Controller sticks are
  unchanged.
- **More mouse-wheel speed stages** (`[Gameplay] SpeedStages`, default 9): the wheel used to step through
  5 walking speeds (factor 0.2 to 1.0 in steps of 0.2). It now steps through 9 by default (steps of 0.1,
  keeping the original 5 speeds), configurable from 2 to 41.
- **DXVK bundled:** the package now ships DXVK 3.1.1 (`d3d9_dxvk.dll`, `dxvk.conf`, `DXVK-LICENSE.txt`)
  and uses it by default (`D3D9Chain = d3d9_dxvk.dll`). If DXVK can't start (no Vulkan 1.3 GPU or
  driver), the game falls back to Windows' D3D9 by itself. With DXVK, the "auto" settings let DXVK pace
  frames: `VSync = -1` means on and `FPSLimit = -1` means no cap. Vsync is no longer forced in
  `dxvk.conf`, so `VSync = 0` works with DXVK too.

## 1.0.0-dev.6
- **Unstick, take two:** the dev.5 nudge never freed Sam at the Lighthouse spot. The log showed why:
  his collision box was slightly *inside* the wall mesh, and Unreal blocks every move that starts
  overlapping something, in every direction (so nudges fail, and you can't stand up either). Now, when
  Sam is blocked, the fix first asks the engine whether his box overlaps anything solid
  (`ULevel::SinglePointCheck`, the test it uses to place spawned and teleported actors). If it does,
  he's moved to the nearest spot where the box fits (searched outwards a unit at a time, never further
  than his own radius, so never through a wall) with the engine's teleport (`ULevel::FarMoveActor`).
  The dev.5 nudge stays as a fallback for blocks without an overlap. Logged as
  `[UNSTICK] freed Sam ... was inside ...`.

## 1.0.0-dev.5
- **Unstick** (`[Gameplay] UnstickPlayer`, on by default): some spots in the original levels trap Sam
  against wall geometry so he can't walk out (for example a corridor in Lighthouse). This is a vanilla bug;
  high frame rates only make it harder to escape. When Sam has been blocked for 0.25 s while pushing
  along or away from level geometry (never when walking straight into a wall or a corner), he is nudged a
  few units with the engine's own collision-checked move and checked to see if he can move again. At most
  3 tries per spot every 5 s; every attempt is logged as `[UNSTICK]`.
- Debug: `StuckDiagnostics` logs step-up collisions while Sam is stuck; `SlowKey` (hold for 30 FPS)
  compares low and high frame rates. Both off by default.
- Frame pacing: DXVK (`D3D9Chain = d3d9_dxvk.dll`) fixed the stutter seen with the system D3D9 in
  borderless mode on the test machine.

## 1.0.0-dev.4
- Fixed the "freeze" when skipping or clicking during startup videos: those videos play in a loop that
  doesn't process window messages, so Windows "ghosted" the borderless window after 5 s (frozen picture,
  "not responding", focus stolen by the ghost, so the skip key was ignored). Window ghosting is now disabled.
- The key/click that skips a video no longer leaks into the next screen (it used to select the menu item
  under the cursor once the startup video loop ended).
- Hang watchdog: if the game stops finishing frames for 3 s, the log gets a stack trace of where it's stuck.
- Verified in game: video skip (logos, teaser, mission intro), mouse re-capture after alt-tab (menus and
  gameplay), menu cursor 1:1 with slow/diagonal movement, logic-rate classification.

## 1.0.0-dev.3
- Video skip no longer freezes: skipping now closes the video exactly like its natural end
  (`UCanvas` VideoClose) instead of seeking Bink to the last frame.
- Mouse is captured again immediately when you alt-tab back (`RecaptureOnFocus`), no click needed.
- Menu cursor: fractional movement is carried over between frames, so slow and diagonal movements work.

## 1.0.0-dev.2
- **Borderless fullscreen** (new default, `WindowMode = 2`): no display mode change, instant alt-tab,
  desktop windows no longer get rearranged. Windowed (`1`) and exclusive fullscreen (`0`) still available.
  Alt+Enter is blocked in borderless/windowed (the game can't leave exclusive fullscreen again).
- Per-monitor DPI awareness (scaled desktops no longer make the game pick a lower resolution).
- **Skippable videos**: Space, Enter, Esc, mouse click or controller A/B/Start ends logos, intro and cutscenes.
- **Menu cursor speed**: the menu cursor now moves like the Windows cursor (`MenuCursorSpeed`), instead of
  raw counts in 640x480 units. Raw input is only fed to the game while it has captured the mouse.

## 1.0.0-dev.1 (first test build)
- d3d9 proxy with deferred, pattern-verified patching (Steam 1.05 executable).
- Frame rate: configurable precise limiter replacing the 100 FPS busy-wait. VSync and refresh rate overrides.
- Logic rate: AI controllers, NPC pawns and Havok props tick at most 60 Hz (configurable).
- Widescreen/ultrawide: Hor+ FOV, HUD/text/scope scaling, corner HUD offset, resolution unlock,
  FMV aspect, high-res Bink (port of ThirteenAG's fix).
- Raw mouse input on a dedicated thread (high polling-rate fix).
- AMD thermal vision: experimental ps_1.1 → ps_2.0 shader replacement (validated on RX 9060 XT),
  GPU vendor spoofing, D3D9 chain-loading (wined3d/DXVK), F10 frame capture for diagnosis.
- EAX through bundled DSOAL without registry edits. Xidi controllers without a dinput8 proxy.
- Skip intro, safe-mode prompt removal, windowed alt-tab fix, shadow map resolution.
