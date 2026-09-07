# Building the mod (`SM2-Archipelago.script`)

This folder is the mod's own source, under **GPL-3.0** (see `LICENSE` here).
It is a script for Marvel's Spider-Man 2 loaded by
[Overstrike](https://github.com/Tkachov/Overstrike), built on Tkachov's
[MSM2-Scripting](https://github.com/Tkachov/MSM2-Scripting) SDK (GPL-3.0),
which is why the mod carries that licence rather than the MIT licence the
rest of this repository uses.

The SDK layer is **not** redistributed here. To build:

1. From MSM2-Scripting take the script template's `game\` folder, `scan.cpp`,
   `scan.h`, `utils.cpp`, `utils.h`, `game\Native.h`, `include\MinHook.h` and
   `lib\libMinHook.x64.lib` (MinHook, BSD 2-Clause) and place them beside these
   files.
2. Open an x64 Developer Command Prompt and run:

```
cl /nologo /std:c++20 /EHa /LD /O2 /W3 /DLOGLEVEL=3 /D_CRT_SECURE_NO_WARNINGS ^
   /I. /Iinclude dllmain.cpp logging.cpp scan.cpp utils.cpp overlay.cpp ^
   game\*.cpp game\Component\*.cpp /Fe:SM2-Archipelago.dll ^
   /link lib\libMinHook.x64.lib user32.lib psapi.lib gdi32.lib shell32.lib ole32.lib
```

3. Zip `info.json` and `SM2-Archipelago.dll` together as `SM2-Archipelago.script`
   and drop it in Overstrike's `Mods Library`.

`LOGLEVEL=3` is what the release uses; `4` adds DEBUG lines to
`%LOCALAPPDATA%\SM2-Archipelago\APMod.log`.
