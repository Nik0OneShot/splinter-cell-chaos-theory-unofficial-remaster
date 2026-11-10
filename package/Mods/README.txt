SPLINTER CELL CHAOS THEORY - UNOFFICIAL REMASTER: MODS FOLDER
===============================================================

Every folder in here is a mod. The remaster loads them when the game starts.


INSTALLING A MOD
----------------
Extract it so that its folder sits directly in this Mods folder:

    Splintercell Chaos Theory\Mods\SomeMod\mod.ini
    Splintercell Chaos Theory\Mods\SomeMod\Data\...

To switch a mod off, set Enabled = 0 in its mod.ini, or put an underscore in
front of its folder name (_SomeMod). To remove it, delete its folder. Mods never
change the game's own files.


MAKING A MOD
------------
1. Copy the _Example folder and rename the copy (without the underscore).
2. Fill in mod.ini.
3. Put your files in the mod's System\ and Data\ folders, laid out exactly like
   the game's own System\ and Data\ folders:

   - A file with the same path as a game file replaces it. For example,
         Mods\MyMod\Data\Videos\back_menu_PC.bik
     plays instead of the game's main menu background video.
   - A file the game doesn't have is added, as if it had shipped with the game.
     For example, a new package in Data\Maps\ or Data\StaticMeshes\ is found by
     the engine like the shipped ones.

   .ini, .log, .exe, .dll and .asi files can't be replaced (the ini files hold
   your settings).

When two mods replace the same file, the mod with the higher Priority in its
mod.ini wins. With equal priorities, the folder name later in the alphabet wins.

The log (System\SCCTRemaster.log) lists the mods it loaded and, with
LogModFiles = 1, every file a mod provides the first time the game opens it
([MODS] lines). [Mods] Mods = 0 in System\SCCTRemaster.ini turns all mods off.

SCRIPTS
-------
A mod's Scripts folder holds Lua scripts, which run when the game starts and
can react to the game (a level starting, every frame, key presses) and change
it (any property of Sam, the guards, the level, ...). See SCRIPTING.txt.

TEXTURES
--------
A mod's Textures folder holds replacement textures: any texture in the game, at
any resolution. See TEXTURES.txt for how to dump the game's textures and make
replacements.

THE LEVEL EDITOR
----------------
Press ~ while playing to open the in-game level editor. The world freezes and
you fly a free camera: hold the right mouse button and use WASD (Q/E down/up,
Shift faster, mouse wheel for speed). Click an actor to select it; drag the
selected actor to move it (Shift: up/down); F focuses on it, Ctrl+D duplicates
it (guards too), Del deletes it (with the gun and gear it carries). The Actors
tab lists every actor, the Inspector shows and edits any property, and the Tools
tab has the camera, grid snapping, actor names, freezing the world, saving, the
UI size and the mod tools. Ctrl+S saves your edits as a level file of the mod
folder named in the Tools tab (MyLevel at first): Mods\MyLevel\Levels\<map>.txt,
applied every time that mission starts (see LEVELS.txt). Press ~ again to close
the editor.

LEVELS
------
A mod's Levels folder holds level changes: text files that add actors to a map,
change the ones in it or remove them, applied when the mission starts. See
LEVELS.txt.
