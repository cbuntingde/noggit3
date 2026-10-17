# noggit3 — WoW 1.12.1 map editor fork

**Status: work in progress — not finished, not yet ready for general use.**

This is a fork of [wowdev/noggit3](https://github.com/wowdev/noggit3) being converted into a map editor for **World of Warcraft 1.12.1 (vanilla)** with working MCLQ water, and (planned) AI-assisted map generation from a text prompt.

**This fork is modified for WoW 1.12.1 only — TBC and WotLK are no longer supported.**

The original upstream README is preserved as [README.upstream.md](README.upstream.md).

## Scope

- **WoW 1.12.1 (vanilla) only. TBC and WotLK are no longer supported**: not tested, not maintained, no effort will be made to keep them working, and bugs reported for them will not be fixed. When a choice would cost vanilla work to keep TBC/WotLK working, vanilla wins.
- TBC/WotLK-era code paths remain in the tree as dormant dead weight (noggit's core is WotLK-shaped; vanilla rides the same infrastructure) but are never verified, maintained or debugged.
- Tested against a vanilla 1.12.1 client with a **flat `Data\` layout** (`base.MPQ`, `dbc.MPQ`, `terrain.MPQ`, ... — no `enUS`-style locale subdirectories, `patch-*.MPQ` expanded at load).

## What works today (verified on a 1.12.1 client)

- **Client profile detection**: recognizes a 1.12.1 install from its `Data\` layout, drives the MPQ archive list. Only WoW 1.12.1 is supported — the "Client Version" setting still lists other versions, but they are unsupported leftovers.
- **Opening maps**: 1.12.1 ADTs load, terrain renders correctly, and you can fly around (tested over Westfall at a steady 59–60 FPS).
- **MCLQ water renders.** A freeze/hang that occurred whenever liquid entered the view (vanilla `LiquidType.dbc` has no texture names, and the WotLK-shaped read produced garbage that triggered an unbounded loop) is fixed: each liquid kind resolves to its fixed 1.12.1 texture set.
- **MCLQ save path exists** (export toggle + path in settings, vanilla nibble alpha format).
- **Headless ADT round-trip tool** for regression testing: `noggit.exe --adt-roundtrip <mapID>` — currently round-trips all 687 Azeroth tiles cleanly.

## Known issues / still to do

- **M2 doodads partially fail to load** (vanilla M2 format support is not in yet) — some world props (haystacks, kobold cave clutter, ...) are missing and log `could not be loaded`. Planned for the vanilla-assets phase.
- **Location/area names resolve incorrectly** (vanilla DBC layouts are not in yet) — e.g. "Tirisfal Glades" may be shown while you are over Westfall.
- **Water rendering is rough** — the sea surface looks dark and flat at distance; MCLQ completeness (tile flags, min/max, liquid bits on save, vanilla UI) is still being worked through.
- **AI map generation is not implemented yet** (design approved: multi-provider — OpenAI/OpenRouter/Ollama/LM Studio/Groq, Anthropic, Gemini — returning a validated JSON map spec rendered deterministically; auto-backup of touched ADTs before apply).
- No undo system. Back up the ADTs you edit before bulk changes.

## Roadmap

| Phase | Content | Status |
|---|---|---|
| P0 | Toolchain, CMake build, runnable install dir | done |
| P1 | Client profile (detection, per-profile MPQ list, flat-layout paths, MCLQ export settings) | done |
| P2 | Vanilla ADT load/save audit, headless round-trip tool, liquid-render freeze fix | done |
| P3 | MCLQ water completeness (tile flags, min/max, liquid bits on save, vanilla UI/render) | in progress |
| P4 | Vanilla assets (DBC layouts, M2, WMO/MLIQ) | planned |
| P5 | AI provider core (adapters + UI) | planned |
| P6 | AI generation/apply (map spec, deterministic terrain generation, auto-backup) | planned |

## Building (Windows — the known-good configuration for this fork)

- **Qt 5.15.2 msvc2019_64** (qtbase-only archive is enough; no windeployqt)
- **Visual Studio 2022** (v143 toolset) + its bundled CMake
- Configure (from the repository root):

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64 \
  -DCMAKE_INSTALL_PREFIX=C:/chris-dev/noggit3/build/install
```

- Build: `cmake --build build --config RelWithDebInfo --parallel`
- Install: `cmake --install build --config RelWithDebInfo` → stage Qt DLLs (`Qt5Core/Gui/Widgets/OpenGL.dll` + `platforms/qwindows.dll`) into the install dir.
- The exe must run from a directory containing `settings.ini` (it is read cwd-relative). Minimal settings:

```ini
[project]
path=./program/
game_path=C:/path/to/your/1.12.1/client
```

See the [Verifying](#verifying) section below for the round-trip and smoke checks used for this fork. The install directory must contain `Qt5Core.dll`, `Qt5Gui.dll`, `Qt5Widgets.dll`, `Qt5OpenGL.dll` and the `platforms/qwindows.dll` plugin next to `noggit.exe`.

## Verifying

- Launch and auto-enter a map: set `NOGGIT_ENTER_MAP=0` (map id) before starting `noggit.exe`.
- Headless ADT round-trip (no window, no GL context needed): `noggit.exe --adt-roundtrip 0` (optionally `--adt-roundtrip-tile <x>,<z>`). The report is written to `log.txt`.
- `log.txt` (cwd-relative) flushes per line — an abruptly ending log marks a crash point.

## Credits & license

Based on [noggit3](https://github.com/wowdev/noggit3) by the Noggit developers (StormLib by Ladislav Zezula, and others — see the upstream README).

This software is open source, licensed under **GPL3** — see the [COPYING](COPYING) file.
