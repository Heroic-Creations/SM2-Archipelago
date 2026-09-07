# Licensing and third-party notices

This project is two things under two licences.

## The client, the Archipelago world, the docs — MIT

`client\`, `spiderman2.apworld`, the YAML templates, the starter save's
packaging and this documentation are **MIT** — see `LICENSE`.

## The game mod (`SM2-Archipelago.script`) — GPL-3.0

The mod is built on **[MSM2-Scripting](https://github.com/Tkachov/MSM2-Scripting)**
by Tkachov (the `MSM2-Script-SDK` runtime it links against and the header
layer it is compiled with), which is **GPL-3.0** with no exception for scripts.
The mod is therefore a derivative work and is itself **GPL-3.0**; its own
source is in `mod\` with the licence text at `mod\LICENSE`, and `mod\BUILD.md`
says where the SDK layer comes from. The SDK headers are not redistributed here.

## Notices

- **MSM2-Scripting** — Tkachov — GPL-3.0. SDK and script template for
  Marvel's Spider-Man 2 scripts; loaded by Overstrike's `scripts_proxy`.
- **SM2ScriptTemplate** — hbgda — no licence file published. The project
  layout (`dllmain.cpp`, `hooking.h`, `scan`, `utils`, `logging`, the `game\`
  headers) descends from this template, as does MSM2-Scripting itself.
- **MinHook** — Tsuda Kageyu — BSD 2-Clause. API hooking.
- **Overstrike** — Tkachov — GPL-3.0. The mod manager that installs the
  `.script`; not linked, not redistributed here.
- **Archipelago** — LLCoolDave, Berserker66, CaitSith2, LegendaryLinux and
  contributors — MIT. The world is written against its `worlds.AutoWorld` API;
  no Archipelago code is redistributed here.
- **CRC-64/ECMA-182** — the hash the engine keys assets by. `client\crc64.py`
  generates the standard table from the polynomial; nothing is copied.
- **ALERT** — Tkachov — GPL-3.0 — used as a research tool for the save and
  config formats during development; no ALERT code is included.

Marvel's Spider-Man 2 is the property of Insomniac Games / Sony Interactive
Entertainment / Marvel. This project is an unofficial fan modification and
ships no game assets. The starter save is player progress data only.
