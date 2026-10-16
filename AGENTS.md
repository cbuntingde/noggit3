# AGENTS.md — noggit3 fork: pure WoW 1.12.1 map editor

This fork (origin `github.com/cbuntingde/noggit3`, tracks `wowdev/noggit3`, branch `default`) is being converted to a **WoW 1.12.1 map editor with working MCLQ water**, plus **multi-provider AI map generation** (prompt → validated JSON map spec → deterministic terrain generation).

**Scope: 1.12.1 ONLY.** No TBC/WotLK support effort, ever. WotLK-era code paths remain in the tree as dormant dead weight (noggit's core is WotLK-shaped; vanilla rides the same infrastructure) — never a compatibility constraint, never verified, never debugged. Vanilla wins every toss-up.

## Phase plan & status (updated 2026-10-16)

| Phase | Content | Status |
|---|---|---|
| P0 | Qt/VS2022 toolchain, cmake build, runnable install dir | done |
| P1 | Client profile: vanilla detection from Data layout, per-profile MPQ list, flat-layout path validation, "Client Version" setting, profile-driven MCLQ export default/path | done |
| P2 | Vanilla ADT load/save audit + headless round-trip test + save tooling. Headline bug (opening a map "crashed"/froze) root-caused to a liquid texture-loop spin and fixed. | done |
| P3 | MCLQ water completeness (tile flags, min/max, liquid bits on save, vanilla UI/render) | done |
| P4 | Vanilla assets (DBC layouts, M2 v256, WMO v17/MLIQ) | next |
| P5 | AI provider core (adapters + UI + mock tests) | planned |
| P6 | AI generation/apply (map spec schema, FastNoise2, auto-backup) | planned |

### P2 findings (fixed 2026-10-16, verified on SoloCraft)

- **The freeze was an infinite loop, not a crash**: `liquid_render::ubo_data` enumerates a liquid's texture frames with `for (;;) { replace("%d", i); if (!exists) break; }`. On vanilla, `LiquidType.dbc` was read with WotLK-shaped accessors (asserts compiled out in RelWithDebInfo) → garbage/empty filename → `exists("")` stat'ed the project dir = always true → unbounded `vector<string>` growth at 100% one core; window froze whenever a liquid entered the view (clicking or simply turning toward water).
- **Fixes**: `liquid_render.cpp` vanilla branch resolves fixed texture sets per liquid id (still=`XTextures\river\lake_a.%d.blp`, ocean=`XTextures\ocean\ocean_h.%d.blp`, magma=`XTextures\lava\lava.%d.blp`, slime=`XTextures\slime\slime.%d.blp`; MCLQ ids 1/2/3/4/15 and WMO ids 13/14/15/19/20/21), and the enumeration loop now also breaks when `replace()` is a no-op. `MPQFile::exists("")` returns false. `AsyncLoader` catch-path now releases the failed object's queue slot (previously leaked → `wait_queue_empty()` at world unload/exit hung after any failed load — verified live: clean exit with ~40 failed M2 loads).
- **Verification tooling** (all in `build/scratch/`, gitignored): `hang_probe.ps1` (per-thread CPU+RIP via dbghelp, no cdb available), `send_keys.ps1` (keybd_event driver), `click.ps1`, `gui_shot3.ps1` (window capture; note: the older `gui_shot2.ps1` ALT-tap activates the Qt menu bar and subsequent arrows navigate menus — don't combine it with synthetic input).
- **Carryover**: sea surface renders very dark/flat at distance (P3 liquid render polish); location names resolve wrong (`Tirisfal Glades` while over Westfall — P4 AreaTable DBC); ~40 M2 doodad load failures per coast flight (M2 v256 — P4).

### P3 findings (done 2026-10-16, verified on SoloCraft)

- **MCLQ save is unconditional** — the `use_mclq_liquids_export` / `mclq_liquids_path` settings are gone (code, UI, settings.ini); vanilla save always writes MCLQ.
- **Headline result: byte-identical round-trip.** Full Azeroth (687 tiles): **92,173 / 92,173 liquid chunks byte-identical** (92,186 layers), 0 diffs in size/min-max/heights/depth-flow bytes/tile bits/flow data/mcnk flags; census of the first save == census of the original (type1=5419715, type4=234937, type6=4183, type7=241069 tiles, 5580616 fishable, 4683878 fatigue, 3534 flow vertices, 5998 flow layers).
- **Fixes that got it there** (all in `liquid_layer.{hpp,cpp}`, `MapChunk.cpp`):
  - `changeLiquidID` hardcodes the vanilla liquid id ↔ mclq type pairing (1→river/mclq4, 2→ocean/mclq1, 3+15→magma/mclq6, 4→slime/mclq3) before the DBC fallback: the WotLK-shaped `DBC.h` accessors misread vanilla `LiquidType.dbc`, which made every layer save as "river".
  - `liquid_layer` keeps the raw MCLQ fields the editor does not model (`_from_mclq`: per-tile covered/fishable/fatigue/flag_0x10/flag_0x20 masks, per-vertex depth + flow_0/flow_1 bytes, `n_flowvs`/`flowvs`) and `to_mclq` re-emits them for untouched data.
  - Loaded `_minimum`/`_maximum` are kept verbatim (no recompute from vertices); MCNK flags are kept verbatim (vanilla sets e.g. `do_not_fix_alpha_map`, clearing it rewrote untouched chunks).
  - The tile loader's render hack (`MapTile.cpp` → `update_underground_vertices_depth`) zeroes depth on below-terrain vertices for lod rendering; `to_mclq` restores the raw byte for vertices the hack zeroed that were not edited since (`_render_zeroed_depth` flag).
- **Metrics**: `adt_roundtrip.cpp` now reports liquid fidelity (per-region byte compares, flag-bit histogram) and liquid census (tile type histogram, attribute counts); non-identical tiles keep `.orig/.first/saved` dumps for inspection.

## Build (Windows, this machine)

- **Qt 5.15.2 msvc2019_64** (qtbase-only archive) at `C:/Qt/5.15.2/msvc2019_64` — no windeployqt; deploy Qt DLLs manually into the install dir.
- **cmake** (VS2022-bundled, 3.31.6): `"/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"`
- Configure (build tree already exists at `build/`): generator `"Visual Studio 17 2022"` `-A x64`, `-DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64`, `-DCMAKE_INSTALL_PREFIX=C:/chris-dev/noggit3/build/install`. Scripting OFF (default); StormLib via FetchContent (its FetchContent_Populate deprecation warning is expected).
- Build: `cmake --build C:/chris-dev/noggit3/build --config RelWithDebInfo --parallel 12` → exe at `build/bin/RelWithDebInfo/noggit.exe`
- Stage runnable dir: `cmake --install C:/chris-dev/noggit3/build --config RelWithDebInfo` → `build/install` (needs Qt5Core/Gui/Widgets/OpenGL.dll + `platforms/qwindows.dll`; Qt5Network.dll joins in P5).
- `build/` and `aqtinstall.log` are .gitignored. The exe must be run from a directory containing `settings.ini` (cwd-relative).

## Test client (ground truth for vanilla)

`C:\chris-dev\SoloCraft 1.12.1` — vanilla 1.12.1 client with **flat** `Data\`: base.MPQ, dbc.MPQ, fonts.MPQ, interface.MPQ, misc.MPQ, model.MPQ, sound.MPQ, speech.MPQ, terrain.MPQ, texture.MPQ, wmo.MPQ, backup.MPQ, patch.MPQ, patch-2.MPQ. No locale subdirectories (2.x+ only); `patch-{number}.MPQ` is a wildcard expanded at load.

## Smoke verification recipe (after every GUI-affecting change)

1. Build + install (above).
2. Seed `build/install/settings.ini`:
   ```ini
   [project]
   path=./program/
   game_path=C:/chris-dev/SoloCraft 1.12.1
   ```
3. From `build/install`: launch `./noggit.exe`, wait 10–60 s (MPQ listfiles take ~8 s; world/tile loading follows), then `taskkill //F //IM noggit.exe`. To auto-enter a map headlessly, set `NOGGIT_ENTER_MAP=<mapID>` (e.g. `NOGGIT_ENTER_MAP=0` → Azeroth, Westfall tile 28-28, after a 2 s timer).
4. Read `log.txt` (cwd-relative, flushed per line). Healthy start expects:
   - `Client profile: vanilla_1_12_1 (detected from game path)`
   - `Using flat vanilla data layout (no locale subdirectories).`
   - all archives opened, DBCs opened, GL context created, `Loading world "Azeroth"`.
   - Expected-on-vanilla noise until P4: `[Error] ... .m2 could not be loaded` (~40 per coast flight) and `Texture N has a lenght of 0` debug lines — these are M2 v256 format failures, benign.
5. Headless ADT round-trip regression: `./noggit.exe --adt-roundtrip 0` (add `--adt-roundtrip-tile x,z` for one tile). Expect final log lines `adt_roundtrip: 687/687 tiles round-tripped`, a liquid fidelity line reading `92173 chunks / 92186 layers compared, 92173 byte-identical` (all other counters 0), and `adt_roundtrip: done with exit code 0`. **The report goes to `log.txt`, not stdout** — stdout is empty. Output ADTs land in `build/install/adt_roundtrip/` (never commit); tiles that are not byte-identical keep `.orig/.first/saved` dumps.

## Crash triage

- `log.txt` flushes per line → an abruptly ending log marks the crash point.
- Windows Event Viewer → Windows Logs → Application, provider "Application Error" records noggit crashes. `0xe06d7363` = unhandled C++ exception (faulting module KERNELBASE.dll is the raiser, not the fault).
- No `%LOCALAPPDATA%\CrashDumps` entries are produced by noggit crashes.

## Architecture notes for the vanilla work

- **Client profile**: `src/noggit/client_profile.{hpp,cpp}`. Values `vanilla_1_12_1` (default) / `wotlk_3_3_5a` (dormant). Detection: `Data/base.MPQ` → vanilla; `Data/common.MPQ` or locale dir → wotlk. Stored as `project/client_version` in settings.ini; drives the startup MPQ archive list and the MCLQ export default/path. Wired into `application.cpp` (flat-layout path validation, startup detection + log, `loadMPQs`).
- **Settings**: `NoggitSettings` macro → `noggit::settings` singleton (`settings.ini` in cwd; keys e.g. `project/game_path`, `project/path`, `project/client_version`). UI in `src/noggit/ui/SettingsPanel.cpp`.
- **Existing vanilla groundwork to build on, not rewrite**: MCLQ read/save in `src/noggit/MapChunk.cpp` (save always writes MCLQ on vanilla), 2048-byte 4-bit nibble alpha write, MTXF disabled.
- **Format landmines**: MVER must be 18 (vanilla is too — the assert in `MapTile.cpp` is fine). MCNK header 0x80 in both versions; vanilla water = MCLQ at MCNK `ofsLiquid`/`sizeLiquid` (0x60/0x64); MH2O is WotLK-only. MCAL: vanilla 4-bit nibble (2048 B) vs WotLK 4096/RLE. The typed accessors in `DBC.h` are WotLK-shaped — vanilla layouts differ (P4).
- **No undo system.** Any bulk edit (especially AI-generated terrain) must back up the ADTs it touches before writing.

## AI design (approved, for P5/P6)

- Providers: OpenAI-compatible (OpenAI/OpenRouter/Ollama/LM Studio/Groq), Anthropic, Gemini. HTTP via Qt Network (`QNetworkAccessManager`), JSON via `QJsonDocument` — zero new third-party dependencies. API keys stored in settings.ini (plaintext, accepted).
- AI returns a **validated JSON "map spec"** (noise octaves, water level/type, texture bands, rivers/lakes/roads); noggit renders it deterministically (FastNoise2). Auto-backup of touched ADTs before apply.

## Workflow conventions

- One phase per user go-ahead (the user launches phases with one-word directives like "next"). End each phase with a checkpoint report: what changed, verified with which commands/evidence, what remains unverified. Then stop.
- The user dogfoods `build/install/noggit.exe` between phases and reports symptoms in plain language — treat those reports as first-class bug input (post-P1 report: "crashed when I clicked on a map" → P2 headline).
- Commit/push directly to `default` when asked (no PRs). Keep scratch artifacts (build output, logs, `aqtinstall.log`) out of commits.
- `git push` can hang on this machine (GCM): use `GIT_TERMINAL_PROMPT=0 git -c credential.helper= -c credential.helper='!gh auth git-credential' push origin default`.
