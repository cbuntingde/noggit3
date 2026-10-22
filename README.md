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
- **The crash handler pops a modal dialog.** On an unhandled exception noggit opens a `QMessageBox`, so the headless command modes (`--adt-roundtrip`, `--asset-check`, `--extract`) can end up blocked on a dialog instead of exiting with an error code. Known, pre-existing, and not yet fixed.
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

## Robustness / security

The parser and scripting layers were audited end-to-end (static analysis, sanitizer builds, and
fuzzing the asset readers with malformed input). The findings, the reasoning and the
verification evidence are recorded in [AUDIT.md](AUDIT.md). The user-visible consequences of the
fixes that landed:

- **Malformed assets no longer read out of bounds.** The M2 model parser — and the animated,
  particle and liquid readers it pulls in — now validates every offset and count against the
  actual file size before using it. A truncated or corrupted `.M2` now produces a clean error
  instead of a heap over-read, so a broken doodad in a map can no longer crash the editor.
- **Map tiles are no longer freed while still in use.** Unloading a tile waits for the background
  loader to drop its last reference before the tile memory is released.
- **Adding a model instance returns the right UID.** `add_model_instance` / `add_wmo_instance`
  now hand back the identifier the caller needs, so instance bookkeeping no longer goes stale
  after an add.
- **The Lua sandbox is actually a sandbox.** `dofile`, `loadfile`, `load` and `loadstring` are
  removed from the scripting environment, so a script can no longer read arbitrary files through
  the interpreter itself.
- **Script file access is contained.** `require()` is restricted to the `scripts/` directory of
  the active project, `load_png` goes through the same readable-path check as every other read,
  and the readable root must exist — when it does not, reads fail closed instead of silently
  falling back to an unrestricted root.
- **Script writes need explicit consent.** Writes are limited to the files a script has
  declared, and write paths are anchored inside the project directory instead of resolving
  against an arbitrary working directory.

One related note for `settings.ini`: `project/path` is no longer seeded with a default. When the
key is missing (or still holds the legacy `"./"` value) the application points it at the detected
WoW installation, which is what the script sandbox uses as its root. An explicitly configured
value is left untouched.

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
path=C:/path/to/your/1.12.1/client
game_path=C:/path/to/your/1.12.1/client
```

`project/path` may be omitted — it then defaults to the detected client directory. The legacy
value `./program/` still works.

See the [Verifying](#verifying) section below for the round-trip and smoke checks used for this fork. The install directory must contain `Qt5Core.dll`, `Qt5Gui.dll`, `Qt5Widgets.dll`, `Qt5OpenGL.dll` and the `platforms/qwindows.dll` plugin next to `noggit.exe`.

### Building on Linux

Linux builds work and are exercised in CI: `.github/workflows/cmake.yml` builds `ubuntu-24.04`
with both `g++-14` and `clang++-18`. This fork is developed on Windows, so treat Linux as
"supported and verified to build" rather than as the primary development target.

```bash
# dependencies, e.g. on Ubuntu 24.04:
#   sudo apt install build-essential cmake qtbase5-dev libgl1-mesa-dev libbz2-dev
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/Qt5
cmake --build build --parallel
```

That is the default configuration and it needs no Lua. **Scripting is off by default**
(`NOGGIT_WITH_SCRIPTING=OFF`) and CI never turns it on.

To build with the scripting layer you additionally need Lua 5.4 development files, and CMake
will pull sol2, nlohmann/json, lodepng and FastNoise2 over the network:

```bash
#   sudo apt install liblua5.4-dev
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/Qt5 \
  -DLUA_INCLUDE_DIR=/usr/include/lua5.4 \
  -DLUA_LIBRARIES=/usr/lib/x86_64-linux-gnu/liblua5.4.so \
  -DNOGGIT_WITH_SCRIPTING=ON
cmake --build build --parallel
```

> **sol2 caveat:** the pinned sol2 revision (`b9c83d5`) does not compile with recent GCC (15+),
> because `sol::optional<T&>` calls a `construct` member that no longer exists. Use GCC 14 /
> Clang 18 (what CI uses), or apply the one-line upstream fix in `sol/optional.hpp` — bind the
> reference directly instead of calling `construct`. The repository deliberately does not patch
> the fetched dependency.

## Verifying

- Launch and auto-enter a map: set `NOGGIT_ENTER_MAP=0` (map id) before starting `noggit.exe`.
- Headless ADT round-trip (no window, no GL context needed): `noggit.exe --adt-roundtrip 0` (optionally `--adt-roundtrip-tile <x>,<z>`). The report is written to `log.txt`.
- Headless asset load check (no window, no GL context needed): `noggit.exe --asset-check <file.M2>` loads a model file through the real loader and reports whether it parsed. Useful for checking a broken doodad by hand.
- `log.txt` (cwd-relative) flushes per line — an abruptly ending log marks a crash point.

## Credits & license

Based on [noggit3](https://github.com/wowdev/noggit3) by the Noggit developers (StormLib by Ladislav Zezula, and others — see the upstream README).

This software is open source, licensed under **GPL3** — see the [COPYING](COPYING) file.
