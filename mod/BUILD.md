# Building the mod (`SM2-Archipelago.script`)

This folder holds the mod's own source, licensed **GPL-3.0** (`LICENSE` here).
The rest of the repository is MIT; the mod is not, because it is a script for
[Overstrike](https://github.com/Tkachov/Overstrike)'s `scripts_proxy`, part of
Tkachov's [MSM2-Scripting](https://github.com/Tkachov/MSM2-Scripting) (GPL-3.0,
no exception for scripts), and its layout and engine headers descend from
hbgda's [SM2ScriptTemplate](https://github.com/hbgda/SM2ScriptTemplate).

The template's files are **not redistributed here** (it publishes no licence).
To build:

1. Get SM2ScriptTemplate and copy these from it, beside the files in this
   folder: the `game\` folder, `scan.cpp`, `scan.h`, `utils.cpp`, `utils.h`,
   `logging.h`, `include\MinHook.h` and `lib\libMinHook.x64.lib` (MinHook is
   BSD 2-Clause).
2. Apply this folder's edits to those headers:

```
git apply SM2ScriptTemplate.patch        (or: patch -p1 < SM2ScriptTemplate.patch)
```

3. From an x64 Developer Command Prompt:

```
cl /nologo /std:c++20 /EHa /LD /O2 /W3 /DLOGLEVEL=4 /D_CRT_SECURE_NO_WARNINGS ^
   /I. /Iinclude dllmain.cpp logging.cpp scan.cpp utils.cpp overlay.cpp ^
   game\*.cpp game\Component\*.cpp /Fe:SM2-Archipelago.dll ^
   /link lib\libMinHook.x64.lib user32.lib psapi.lib gdi32.lib shell32.lib ole32.lib
```

4. Zip `info.json` and `SM2-Archipelago.dll` together as `SM2-Archipelago.script`
   and drop it in Overstrike's `Mods Library`.

Released builds use `LOGLEVEL=4`, which writes the DEBUG lines to
`C:\ProgramData\SM2-Archipelago\APMod.log` that a bug report needs; `3` drops
them. The `.inc` files are included by `dllmain.cpp` in a fixed order; they are
not compiled separately.
