# Third-party notices

## Source code used in d3d9.dll

### WidescreenFixesPack (ThirteenAG) – MIT License
Widescreen/HUD/FOV logic and byte patterns in `src/widescreen.cpp` and parts of `src/misc.cpp`
are ported from `source/SplinterCellChaosTheory.WidescreenFix` (2024) and
`source/SplinterCellChaosTheory.FusionFix` (2026). https://github.com/ThirteenAG/WidescreenFixesPack

```
MIT License
Copyright (c) ThirteenAG
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
```

### Lua 5.4 – MIT License
Copyright (C) 1994-2026 Lua.org, PUC-Rio. Unmodified library sources of Lua 5.4.9 in `third_party/lua/src`, compiled
into `d3d9.dll` for mod scripting. Full text: `third_party/lua/LICENSE.txt`. https://www.lua.org

### The CLUSTERFUCK award's model – Sketchfab Standard License
A 3D model by Pitzuna (https://sketchfab.com/bonkiobonkio), used under the Sketchfab Standard License. Not in the
repository: release packages carry it converted into `System/SCCTRemaster_Award.pak` (built by
`tools/pack_plush.py`), used only by the mod. It's a fan tribute to a character that belongs to its creators, and
isn't affiliated with them.

### stb – MIT License (or public domain)
`stb_image.h` 2.30, `stb_image_write.h` 1.16 and `stb_dxt.h` 1.12 by Sean Barrett, unmodified, in `third_party/stb`;
compiled into `d3d9.dll` for texture packs (reading and writing images, DXT compression). Full text:
`third_party/stb/LICENSE.txt`. https://github.com/nothings/stb

### Dear ImGui – MIT License
Copyright (c) 2014-2026 Omar Cornut. Dear ImGui 1.92.9b (core and the DX9/Win32 backends, unmodified) in
`third_party/imgui`, compiled into `d3d9.dll` for the in-game level editor. Full text: `third_party/imgui/LICENSE.txt`.
https://github.com/ocornut/imgui

### MinHook – BSD 2-Clause License
Copyright (C) 2009-2017 Tsuda Kageyu. Full text: `third_party/minhook/LICENSE.txt`.
https://github.com/TsudaKageyu/minhook

## Bundled binaries (unmodified)

| File | Project | License | Source |
|---|---|---|---|
| `dsound.dll` | DSOAL (DirectSound to OpenAL, based on Wine DirectSound) | LGPL-2.1 | https://github.com/kcat/dsoal |
| `dsoal-aldrv.dll` | OpenAL Soft 1.25.2 | LGPL-2.0 | https://github.com/kcat/openal-soft |
| `Xidi.32.dll` | Xidi (Samuel Grossman) | BSD-3-Clause | https://github.com/samuelgr/Xidi |
| `d3d9_dxvk.dll` | DXVK 3.1.1 (Philip Rebohle and contributors), 32-bit `d3d9.dll` renamed | zlib | https://github.com/doitsujin/dxvk |

DSOAL, OpenAL Soft and Xidi are taken from ThirteenAG's SplinterCellChaosTheory.WidescreenFix package.
DXVK is the official 3.1.1 release build; its license is included as `DXVK-LICENSE.txt`, and
`dxvk.conf` is our own configuration file for it. The LGPL
components are dynamically loaded libraries; their source code is available at the links above.
`Xidi.ini` and `JoysticksDefault.ini` (Xidi controller profiles) come from the same package.
