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
