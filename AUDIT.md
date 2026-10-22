# Noggit3 — Code & Security Audit

**Date:** 2026-10-22
**Scope:** whole repository at `HEAD` (`1d1a161` "hardening: validate asset parsers and replace asserts with errors"), ~54.5k lines of C++ across 177 files.
**Mode:** full codebase audit (code quality + security), per `code-analysis` and `security-analysis` procedures.

---

## 1. Verdict

**Request changes.** Four findings are High severity: three provable out-of-bounds reads in the
M2/doodad parser, a use-after-free in tile unloading, a complete bypass of the Lua script sandbox,
and a silent data-corruption bug in model instance bookkeeping. Nine further findings are Medium or
lower, six of them in the scripting subsystem.

The recent hardening commit is genuinely good work — the ADT/MCNK/DBC/WMO/BLP/MPQ parsers now
validate aggressively and reject malformed input cleanly. **The M2 parser was left out of that
hardening**, and it is the one parser that still trusts the file completely. The scripting sandbox
is well *designed* but leaks through three of the Lua standard library's own escape hatches.

---

## 2. Method & tooling

No SAST tooling was present on the machine, so a toolchain was installed and the project was built
from source under it:

| Tool | Version | Use |
|---|---|---|
| cppcheck | 2.19.0 | whole-tree static analysis (`--enable=warning,style,performance,portability --inconclusive`) |
| clang-tidy | 20.1.8 (LLVM) | `clang-analyzer-*`, `bugprone-*`, `cert-*`, `performance-*` on the parser sources |
| Semgrep | 1.182.1 | custom rules targeting the file-buffer access pattern |
| GCC | 15.2.0 | `-fanalyzer` capable; used for an **ASan + UBSan** build |
| AddressSanitizer / UBSan | — | dynamic analysis of crafted malformed inputs |

The build was configured with `NOGGIT_WITH_SCRIPTING=OFF` because the vendored sol2 fork does not
compile under GCC 15 (`sol::optional<T&>` in `optional_implementation.hpp`); the scripting module was
analysed statically instead. Qt 5.15.18, Lua 5.4.8 and StormLib were staged into a user-space prefix.

**Coverage:** `src/noggit` (133 files), `src/opengl`, `src/mysql`, `src/math`, `src/util` — 366
cppcheck diagnostics, 1.2M clang-tidy diagnostics (mostly style), 1,972 Semgrep hits (29 after
deduplication and rule tuning). `src/external` was excluded.

**Dynamic proof harness:** the app's own headless `--asset-check <file>` mode was used to feed
crafted M2 files through the real loader, backed by a stub WoW installation
(`/tmp/fakewow/Data/base.MPQ`, stub `DBFilesClient/*.dbc`) so `OpenDBs()` and the client profile
detector succeed.

---

## 3. Findings

### F1 — HIGH — M2/doodad parser has no bounds validation (3 proven heap over-reads)

**Files:** `src/noggit/Model.cpp`, `src/noggit/Animated.h`

The hardening commit added `M2Array<T>` bounds-checking and `require_range()` helpers, and applied
them to DBC, ADT, MapTile, MapChunk, WMO, BLP and MPQ. The M2 parser was not included. Every read
of a file-supplied offset in `Model.cpp` goes straight through `MPQFile::getBuffer() + offset`
without a range check.

**F1a — truncated M2 → 304-byte over-read** (`Model.cpp:210`)

```cpp
memcpy(&header, f.getBuffer(), sizeof(ModelHeader));   // Model.cpp:210
memcpy(&vanilla_header, f.getBuffer(), sizeof(ModelHeaderVanilla));  // Model.cpp:218
```

`f.isEof()` only rejects a *completely empty* file. A 16-byte file passes it.

```
==46822==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x7607877e1020
READ of size 304 at 0x7607877e1020 thread T1
    #1 Model::finishLoading() src/noggit/Model.cpp:210
```

**F1b — attacker-controlled bone count → large over-read** (`Model.cpp:270-281`)

```cpp
ModelBoneDefVanilla const* vbones
  = reinterpret_cast<ModelBoneDefVanilla const*>(f.getBuffer() + header.ofsBones);  // :270
...
for (size_t i = 0; i < header.nBones; ++i)                                            // :277
  _vanilla_bone_defs.push_back(to_wotlk_block(vbones[i]));                            // :281
```

`header.nBones` comes straight from the file. A 1024-byte file declaring `nBones = 65536`:

```
==ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 120 ...
    #4 world_model_instances_storage::... Model.cpp:281
```

**F1c — attacker-controlled animation-track count → large over-read** (`Animated.h:221-246`)

```cpp
const TimestampType* timestamps = file.get<TimestampType>(ofs);   // Animated.h:231
for (size_t i = 0; i < count; ++i)                                 // count = animationBlock.nTimes
  times[j].push_back(timestamps[i]);                               // Animated.h:235
```

`MPQFile::get<T>()` validates **one** element; the loop then reads `count` elements, where `count`
is `animationBlock.nTimes` from the file. Crafted file with `nTimes = nKeys = 65536`:

```
==ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 4 ...
    #4 Animation::M2Value<...>::M2Value(...) src/noggit/Animated.h:235
    #5 Bone::Bone(...) src/noggit/Model.cpp:1787
    #10 Model::initAnimated(MPQFile const&) src/noggit/Model.cpp:1512
```

**Attack path.** A doodad `.m2` is named by the ADT's `MMDX` chunk and read from the MPQ or the
project directory. A malicious map project or MPQ therefore reaches `Model::finishLoading()` with
fully attacker-controlled headers → out-of-bounds heap reads. Impact: crash (DoS), and adjacent heap
memory is read into bone transforms, texture names and animation keys that are then rendered and
written back into saved map files (information disclosure).

**Full inventory of unchecked accesses** (Semgrep `noggit-unchecked-buffer-offset` +
`noggit-unbounded-*-memcpy`, all in the M2 parser; zero elsewhere):

```
Model.cpp:210  218  228  270  287  295  302  311  328  354  419  425  472  486
          537  552  581  593  613  640  1475  1486  1509  1519  1544  1564  1574
Model.h:270
Particle.cpp:69   (memcpy from f.getBuffer() + mta.p.colors.ofsKeys, sizeof(vector_3d)*3)
```

Representative: `Model.cpp:552` builds a C string from `f.getBuffer() + texdef[i].nameOfs` and reads
`nameLen` bytes from it — both file-supplied, neither bounded.

**Fix.** Route every M2 access through `M2Array<T>` / a bounds-checked accessor. Concretely: add a
`require_range(offset, length)` guard before each `getBuffer() + offset`, replace the fixed-size
`memcpy` at :210/:218 with a size check (`f.getSize() >= sizeof(ModelHeader)`), clamp `count` in
`Animated.h` to the number of elements actually present, and bound `nameOfs`/`nameLen` at :552.

---

### F2 — HIGH — Use-after-free: `MapTile` destroyed while its loader thread is still using it

**Files:** `src/noggit/MapTile.cpp:457-459`, `src/noggit/AsyncLoader.cpp:68-87`, `src/noggit/map_index.cpp:369-378`

`AsyncObject::finished` is the only signal other threads use. `MapTile::finishLoading()` publishes
it **before** it has finished touching the object:

```cpp
finished = true;                  // MapTile.cpp:457  <-- published here
_tile_is_being_reloaded = false;  // MapTile.cpp:458  <-- still touching `this`
_state_changed.notify_all();      // MapTile.cpp:459
```

Meanwhile the GUI thread destroys the tile as soon as it sees that flag:

```cpp
void MapIndex::unloadTile(const tile_index& tile)
{
  if (tileLoaded(tile))                      // map_index.cpp:371 — true as soon as `finished` is set
  {
    mTiles[tile.z][tile.x].tile.reset();     // map_index.cpp:376 — destroys the MapTile
  }
}
```

`unloadTiles()` is called from the render/update loop (`MapView.cpp:1855`), i.e. **on every camera
movement**, so the window is hit routinely rather than in a rare corner case.

**Consequences.**
- UAF write at `MapTile.cpp:458-459` (`this->_tile_is_being_reloaded`, `this->_state_changed`).
- UAF read at `AsyncLoader.cpp:73` (`object->filename`, a `std::string` read from freed memory) —
  reachable when the `additional_file_loading_log` setting is on.
- UAF call at `AsyncLoader.cpp:87` (`object->error_on_loading()`) on the exception path.

The codebase already contains the correct mechanism for this — `AsyncLoader::ensure_deletable()`
(`AsyncLoader.cpp:113-135`) blocks until an object is neither queued nor in flight — but it is only
used from `multimap_with_normalized_key.hpp:113`. **Tiles never call it.**

**Fix.** Call `AsyncLoader::instance->ensure_deletable(tile)` before `tile.reset()` in
`unloadTile`/`reloadTile`, and move `finished = true` out of the individual `finishLoading()`
implementations so it is set by `AsyncLoader::process()` *after* `finishLoading()` returns.

---

### F3 — HIGH — Lua sandbox bypass: `dofile`, `loadfile` and `load` are exposed

**File:** `src/noggit/scripting/script_context.cpp:31`

```cpp
open_libraries(sol::lib::base, sol::lib::table, sol::lib::string);
```

`sol::lib::base` maps to `luaL_requiref(L, "base", luaopen_base, 1)`
(`sol/state_view.hpp:135-138` in the vendored sol2). Lua's base library contains **`dofile`,
`loadfile` and `load`**. Verified against the exact Lua version this project links (5.4.8):

```
GLOBAL           PRESENT?
dofile           YES  <-- present
loadfile         YES  <-- present
load             YES  <-- present
os               no
io               no
package          no
debug            no
```

`dofile("/etc/passwd")` reaches the file with no sandbox check and no prompt:

```
dofile('/etc/passwd') error: /etc/passwd:1: function arguments expected near ':'
```

The deliberate read policy in `script_filesystem.cpp:56-58` ("Scripts may only read inside the
scripts tree … and the open WoW project") is therefore not enforceable — `read_file` is sandboxed
but `dofile`/`loadfile`/`load` are not, and neither is anything reached through them.

**Impact.** Any `.lua` file anywhere on disk can be opened and executed with the full noggit script
API. Lua's syntax-error message quotes the offending token, so fragments of arbitrary files also
leak into the script log.

**Fix.** After `open_libraries`, remove the escape hatches:

```cpp
for (auto const* n : {"dofile", "loadfile", "load", "loadstring"})
  set_function(n, sol::lua_nil);
```

Better: build an explicit `_ENV` table containing only the intended functions and install it as the
state's environment, so new base-library functions cannot leak in later.

---

### F4 — HIGH — Model/WMO instance UID mismatch corrupts instance bookkeeping

**File:** `src/noggit/world_model_instances_storage.cpp:31` and `:77`

```cpp
std::uint32_t world_model_instances_storage::add_model_instance(ModelInstance instance, bool from_reloading)
{
  std::uint32_t uid = instance.uid;      // :18  original uid
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    uid_after = unsafe_add_model_instance_no_world_upd(std::move(instance));  // :23
  }
  if (from_reloading || uid_after != uid)
    _world->updateTilesModel(&_m2s.at(uid_after), model_update::add);        // :28  correct
  return instance.uid;                    // :31  <-- ORIGINAL uid, not uid_after
}
```

`unsafe_add_model_instance_no_world_upd` assigns a **new** GUID and stores the instance under it
when the requested uid collides with a *different* instance (`:54-58`):

```cpp
_uid_duplicates_found = true;
instance.uid = _world->mapIndex.newGUID();
return unsafe_add_model_instance_no_world_upd(std::move(instance));
```

The move constructor copies the scalar (`uid (other.uid)`, `Model.h:47`; `mUniqueID (other.mUniqueID)`,
`WMOInstance.h`), so the moved-from `instance` still holds the **original** uid — and that is what
the function returns. `add_wmo_instance` (`:77`) has the identical defect.

**Consequences.**
- `World::addModel` (`World.cpp:2582-2583`) and `World::addWMO` (`World.cpp:2606-2607`) do
  `get_model_instance(uid).value()` with the stale uid → they return the *pre-existing* instance
  that owns that uid, and push it into `_models_by_filename`. If the stale uid belongs to no model
  instance, `.value()` throws `std::bad_optional_access`.
- `MapTile::add_model(uid)` (`MapTile.cpp:1204`) records the wrong uid in the tile's uid list, so
  when the tile unloads, `remove_models_if_needed` → `unload_instance_and_remove_from_selection_if_necessary(uid)`
  (`world_model_instances_storage.cpp:275-293`) **erases the legitimate instance** that owns the
  stale uid and decrements its `_instance_count_per_uid`, while the newly added instance is
  orphaned and never unloaded.

This is silent data corruption on a normal path (any duplicate uid), not an edge case.

**Fix.** Return `uid_after`, not the moved-from object's field:

```cpp
return uid_after;
```

---

### F5 — MEDIUM — `require()` path traversal executes arbitrary `.lua` files

**File:** `src/noggit/scripting/script_context.cpp:137-154`, `:169-172`

```cpp
std::string script_context::module_to_file(std::string const& mod)
{
  return (std::filesystem::path("scripts") / std::filesystem::path(mod + ".lua")).string();
}
```

`mod` is a plain string supplied by the running script and is never validated. `operator/` with an
absolute right-hand side *replaces* the left-hand side (C++17 `[fs.path.append]`), and `..`
components are honoured:

```
require("util")                ->  scripts/util.lua
require("../../../../tmp/evil")->  scripts/../../../../tmp/evil.lua   (resolves to /tmp/evil.lua)
require("/tmp/absolute")       ->  /tmp/absolute.lua
```

`execute_file` then calls sol2's `script_file`, which opens the path directly with
`luaL_loadfilex` — **no containment check**, unlike `read_file`.

Worse, the loaded module's *parent directory* becomes a new read root, because
`current_script_dir()` is derived from the raw module path and `is_readable_path` treats it as an
allow-listed root (`script_filesystem.cpp:63-65`). A script can therefore plant a module in a
directory it wants to read and that directory becomes readable for the module.

**Fix.** Validate module names in `require()` — reject absolute paths, `..` components and anything
that does not resolve under `scripts/` — and derive `current_script_dir()` from a validated
canonical path rather than the raw module string.

---

### F6 — MEDIUM — `load_png` bypasses the read sandbox

**File:** `src/noggit/scripting/script_image.cpp:182-184`, `:33-36`

```cpp
state->set_function("load_png", [state](std::string const& path) {
  return std::make_shared<image>(state, path);     // :182
});

image::image(script_context * ctx, std::string const& path)
: script_object(ctx)
{
  unsigned error = lodepng::decode(_image, _width, _height, path);   // :36 — raw path
```

The script-supplied path goes straight to `lodepng::decode`. `get_readable_path()`
(`script_filesystem.cpp:76`) is never called, so the file is read from anywhere on disk with no
prompt and no allow-list. Exfiltration is limited to files that decode as PNG, but the policy check
itself is simply absent.

**Fix.** Route through `get_readable_path("load_png", ctx, path)` exactly as `read_file` does.

---

### F7 — MEDIUM — Read-root fail-open: an empty project path makes the whole filesystem readable

**Files:** `src/noggit/settings.hpp:14-22`, `:37-40`; `src/noggit/scripting/script_filesystem.cpp:34-46`, `:63-65`

```cpp
QString project_folder = values->value("project/path", "./").toString();
if (!(project_folder.endsWith('\\') || project_folder.endsWith('/')))
{
  project_folder += "/";       // "" becomes "/"
}
```

`is_readable_path` compares roots **component-wise**:

```cpp
bool path_is_within (fs::path const& candidate, fs::path const& root)
{
  ...
  for (; root_it != root.end(); ++root_it, ++candidate_it)
    if (candidate_it == candidate.end() || *candidate_it != *root_it) return false;
  return true;
}
```

If `project/path` is ever empty, the root becomes `/`, whose single component matches the first
component of *every* absolute path → `read_file` can read the entire filesystem.

Separately, the default `"./"` is **not** the WoW project. `application.cpp:297` intends to default
it to the game directory, but the settings constructor always pre-seeds the key, so that default is
dead code and the effective root is the **current working directory** — the noggit executable's
directory (`application.cpp:78-98`). For a git-clone install this is the user's home directory, so
the "read root" covers `~/.ssh`, `~/.bashrc` and so on.

**Fix.** Do not pre-seed `project/path` in the settings constructor; resolve it to the game
directory at startup as `application.cpp:297` intends; and make `is_readable_path` fail closed when
a root is empty or `/`.

---

### F8 — MEDIUM — Write consent is process-global, not per-script

**File:** `src/noggit/scripting/script_filesystem.cpp:101-103`

```cpp
namespace {
  std::set<std::filesystem::path> allowed_files;
}
```

A file-scope static in an anonymous namespace → one instance for the whole process, shared by
every `script_context` and every script, and never cleared. `get_writable_path` short-circuits on a
hit (`:112-115`). A path the user approved for one script is writable **silently** by any other
script afterwards — including one dropped into `scripts/` later, since the scripting tool
rebuilds its context on "Reload Scripts" and on brush-event exceptions
(`scripting_tool.cpp:47`, `:170-174`, `:248`) without clearing the set.

**Fix.** Move `allowed_files` into `script_context` (or key it by script identity), and clear it in
`doReload()`.

---

### F9 — MEDIUM — `get_writable_path()` does not anchor relative paths, unlike the read path

**File:** `src/noggit/scripting/script_filesystem.cpp:107` vs `:79`

```cpp
// write path (:107)                              // read path (:79)
auto canonical = fs::weakly_canonical(fs::path(path));   fs::path const canonical (fs::weakly_canonical (fs::absolute (fs::path (path)), ec));
```

The author's own comment at `:51-53` notes that `absolute` anchors relative paths at cwd — the
write path does not do it. Consequences: the consent dialog can display a *relative* path, so the
user cannot see where the file will actually land (this is the only control on writes); and the
approval key stored in `allowed_files` may be relative, so `"out.txt"` and `"/abs/cwd/out.txt"`
are two different keys for the same file.

**Fix.** Use `fs::weakly_canonical(fs::absolute(fs::path(path)), ec)` and show the absolute path in
the prompt.

---

### F10 — LOW — DBC string table entries are not guaranteed NUL-terminated

**File:** `src/noggit/DBCFile.h:44-50`, `:144-150`

```cpp
const char *getString(size_t field) const
{
  size_t stringOffset = getUInt(field);
  file.require_string_offset(stringOffset);              // offset < stringSize
  return file.stringTable.data() + stringOffset;         // no NUL guarantee
}
```

`require_string_offset` only bounds the *offset*. `stringTable` is a `std::vector<char>` resized to
`stringSize`; if the final string runs to the end of the table without a NUL byte, every
`std::string` constructed from the returned `const char*` (e.g. `AreaDB::getAreaName`,
`DBC.cpp:47-52`) reads past the allocation.

**Fix.** Require a NUL terminator within the table, e.g. `memchr(stringTable.data() + offset, '\0',
stringSize - offset) != nullptr`, and return a bounded `std::string_view`.

---

### F11 — LOW — `MPQFile::existsOnDisk()` is an unnormalised existence oracle

**File:** `src/noggit/MPQ.cpp:267-275`, `src/noggit/scripting/script_global.cpp:30`, `:56`

```cpp
bool MPQFile::existsOnDisk (std::string const& filename)
{
  ...
  return std::filesystem::exists (getDiskPath (filename));   // :274 — raw filename
}
```

Unlike the `MPQFile` constructor (`:186-195`), this path never passes through
`normalized_filename` + `is_safe_normalized_filename`. `getDiskPath` is
`project_path / filename`, so `add_m2("../../../../etc/shadow", …)` yields a yes/no answer for a
path outside the project directory. Content read is still blocked by the constructor's validation,
so this leaks **existence only**.

**Fix.** Normalise and validate in `existsOnDisk` as well.

---

### F12 — LOW — `MPQFile::get<T>()` returns a possibly unaligned pointer

**File:** `src/noggit/MPQ.h`

```cpp
template<typename T> const T* get(size_t offset) const
{
  require_range(offset, sizeof(T));
  return reinterpret_cast<T const*>(buffer.data() + offset);
}
```

The range is checked but the *alignment* is not. `buffer` is a `std::vector<char>`, so `data()` is
aligned for `max_align_t`, but `offset` need not be a multiple of `alignof(T)`. Used at
`Animated.h:221-222` with `animationBlock.ofsTimes` / `ofsKeys`, which are file-supplied. The read
is UB and would trap on a strict-alignment architecture; it is also the shape that ASan's
`-fsanitize=alignment` flags.

**Fix.** Check `offset % alignof(T) == 0` in `get<T>()` (or `memcpy` into a local).

---

### F13 — LOW — MCIN chunk offsets are read without checking the read result

**File:** `src/noggit/MapTile.cpp:132-136`

```cpp
for (int i = 0; i < 256; ++i)
{
  theFile.read(&lMCNKOffsets[i], 4);   // :134 — return value ignored
  theFile.seekRelative(0xC);
}
```

`lMCNKOffsets` is uninitialised, and `read()` returns 0 once `eof` is latched. This is currently
masked: the following `seekRelative(0xC)` throws `std::out_of_range` before the offsets are used
(`eof` can only be latched at `pointer == buffer.size()`, at which point the seek must fail). The
result is that a truncated ADT is rejected, but by accident rather than by design — and the code
style differs from the explicit checks used everywhere else in the same function.

**Fix.** Check the return value and throw, matching the surrounding code.

---

### F14 — LOW — `liquid_layer::operator=(const&)` omits `_liquid_id`

**File:** `src/noggit/liquid_layer.cpp:325-...`

The copy constructor copies `_liquid_id` (`:264`) and the move assignment swaps it (`:293`), but
the copy assignment does not copy it — it then calls `changeLiquidID(_liquid_id)`, which recomputes
the derived type fields from the *old* id while the geometry came from the source object. The
result is a layer with one object's flags and another's liquid type.

The operator currently has **no call sites** in the tree, so this is latent rather than live.

**Fix.** Copy `_liquid_id` (and `_need_data_update`, which the move assignment swaps but the copy
assignment ignores).

---

### F15 — INFORMATIONAL — Throwing from a destructor under `NOGGIT_THROW_ON_OPENGL_ERRORS`

**File:** `src/opengl/context.cpp:182`

`~verify_context_and_check_for_gl_errors()` throws when the macro is defined. A destructor is
implicitly `noexcept`, so this would call `std::terminate`. The macro is **not defined anywhere** in
the tree, so the path is currently dead — cppcheck reports it only because it does not evaluate
preprocessor conditionals.

**Fix.** If the check is ever enabled, throw from a non-destructor scope or mark the destructor
`noexcept(false)` deliberately.

---

### F16 — LOW — `AsyncLoader::setup()` leaks its instance

**File:** `src/noggit/AsyncLoader.cpp:12-17`

```cpp
instance = new AsyncLoader(std::max(1, threads));
```

`instance` is a raw pointer that is never deleted, although `AsyncLoader` has a destructor. The
loader threads are therefore still running when static destructors execute at process exit.

**Fix.** Store the instance in a `std::unique_ptr` and shut it down in its destructor.

---

## 4. Checked and found sound

These were examined specifically and **passed**; they are recorded so the next reviewer does not
re-audit them.

- **MPQ path traversal.** `is_safe_normalized_filename()` (`MPQ.cpp:471-494`) was extracted verbatim
  from the source and exercised against 13 adversarial inputs — `../` traversal, backslash
  traversal, absolute paths, UNC-style paths, drive letters, NTFS alternate data streams, and the
  260-character cap. **All verdicts were as expected**; no traversal bypass found.
- **`read_file` symlink escape.** `canonicalize()` uses `weakly_canonical(fs::absolute(p))`, which
  resolves symlinks for the existing prefix, and `read_file` requires the target to exist, so the
  final component is resolved too. The path that is opened is the resolved path.
- **Read-root prefix confusion.** `path_is_within` compares path *components*, not string
  prefixes, so `/proj` does not match `/proj2`.
- **MySQL layer.** All query construction goes through `mysql_real_escape_string` /
  `mysql_real_escape_string_quote` with the connection handle; no string-concatenated SQL was found.
  Credentials are read from `settings.ini` and written back with restrictive permissions.
- **ADT / MCNK / DBC / WMO / BLP parsers.** Every chunk read validates its declared size against
  the remaining file, and several parsers apply explicit plausibility caps (e.g. `MapTile.cpp`
  rejects an `MCNK` offset table larger than 4096 bytes). Semgrep found **zero** unchecked
  buffer-offset accesses in these files.
- **DBC size caps.** `DBCFile::open` bounds the record count to 10M, the string table to 32 MiB and
  the record data to 64 MiB, and checks `20 + dataBytes + stringSize <= fileSize`.
- **AsyncLoader exception handling.** `AsyncLoader::process` wraps `finishLoading()` in
  `try/catch(...)`, so a malformed asset produces a logged error rather than `std::terminate`.
- **`AsyncObject::finished`** is a `std::atomic<bool>`, so the flag itself is not a data race.
- **Script sandbox: no shell, process or network primitives.** `os`, `io`, `package` and `debug`
  are not opened; no `QProcess`, `popen`, `system()` or socket code exists under
  `src/noggit/scripting/`.
- **Script write path.** `write_file`/`append_file` require an explicit user confirmation unless
  `allow_scripts_write_any_file` is set; `mkdirs` only creates the parent of the *approved
  canonical* path, so it cannot create directories outside the approved area.

---

## 5. Coverage gaps & recommendations

- **The M2 parser has no tests.** The whole test suite is three files under `test/math/`
  (`matrix_4x4`, `vector_2d`, `trig`) — no parser, no MPQ, no scripting test. CI
  (`.github/workflows/cmake.yml`) builds on three toolchains but **never runs `ctest`**, and runs no
  sanitizer or SAST job.
- Recommended, in priority order:
  1. Add a **fuzz/regression corpus** of malformed assets (`.m2`, `.adt`, `.dbc`, `.wmo`) run under
     ASan+UBSan in CI. F1 is exactly the class of bug such a harness finds immediately.
  2. Add a **CI job that runs the test suite** — it currently builds only.
  3. Enable a **clang-tidy** CI job with `clang-analyzer-*,bugprone-*,cert-*` (the
     `cppcoreguidelines-*` family is too noisy on this codebase to be useful as a gate).
  4. Add a **sandbox regression test** asserting, from Lua, that `read_file("/etc/passwd")`,
     `load_png("/etc/…")`, `require("/etc/x")` and `dofile`/`loadfile` all fail.

---

## 6. Finding index

| # | Severity | Confidence | Area | Title |
|---|---|---|---|---|
| F1 | High | High (3× ASan) | `Model.cpp`, `Animated.h` | M2 parser has no bounds validation |
| F2 | High | High (traced) | `MapTile.cpp`, `AsyncLoader.cpp`, `map_index.cpp` | Use-after-free destroying a tile mid-load |
| F3 | High | High (verified) | `script_context.cpp` | `dofile`/`loadfile`/`load` bypass the Lua sandbox |
| F4 | High | High (traced) | `world_model_instances_storage.cpp` | Instance UID mismatch corrupts bookkeeping |
| F5 | Medium | High (verified) | `script_context.cpp` | `require()` path traversal |
| F6 | Medium | High (traced) | `script_image.cpp` | `load_png` bypasses the read sandbox |
| F7 | Medium | High (traced) | `settings.hpp`, `script_filesystem.cpp` | Read root fails open to `/`; default root is cwd |
| F8 | Medium | High (traced) | `script_filesystem.cpp` | Write consent is process-global |
| F9 | Medium | High (traced) | `script_filesystem.cpp` | Write path is not anchored at cwd |
| F10 | Low | Medium | `DBCFile.h` | DBC strings not guaranteed NUL-terminated |
| F11 | Low | High (traced) | `MPQ.cpp` | `existsOnDisk` existence oracle |
| F12 | Low | High (traced) | `MPQ.h` | `get<T>()` may return an unaligned pointer |
| F13 | Low | High (traced) | `MapTile.cpp` | MCIN offsets read without checking the result |
| F14 | Low | High (traced) | `liquid_layer.cpp` | Copy assignment omits `_liquid_id` (no call sites) |
| F15 | Informational | High | `context.cpp` | Destructor throws under an undefined macro |
| F16 | Low | High (traced) | `AsyncLoader.cpp` | Loader instance leaked; threads outlive static destruction |

---

---

## 7. Remediation applied

All 16 findings were subsequently fixed in the working tree (22 files, +379/-87). No commit was made.

| # | Fix | Files |
|---|---|---|
| F1 | Added `MPQFile::get_array<T>(offset, count)` (bounds-checked) and `MPQFile::read_array<T>()` (memcpy-based, alignment-safe). Every M2 array read now goes through them; the two header `memcpy`s are guarded by a size check; the animation track counts are clamped to the data actually present; the view/skin section validates its offsets and its triangle/submesh indices; the bone index and the particle/ribbon bone indices are range-checked; the texture name offset and length are range-checked. | `MPQ.h`, `Model.cpp`, `Model.h`, `Animated.h`, `Particle.cpp` |
| F2 | Added `MapIndex::destroyTile()`, which calls `AsyncLoader::ensure_deletable()` before destroying a tile, and used it in `unloadTile()`/`reloadTile()`. `finished` is now published after the tile is last touched. | `map_index.cpp/.hpp`, `MapTile.cpp` |
| F3 | `dofile`, `loadfile`, `load` and `loadstring` are removed from the Lua state right after `open_libraries()`. | `script_context.cpp` |
| F4 | `add_model_instance()` and `add_wmo_instance()` now return `uid_after` instead of the moved-from object's field. | `world_model_instances_storage.cpp` |
| F5 | `module_to_file()` rejects module names containing a backslash or a colon and requires the result to resolve inside `scripts/`. | `script_context.cpp`, `script_filesystem.cpp/.hpp` |
| F6 | `load_png` now resolves its path through `get_readable_path()` before handing it to lodepng. | `script_image.cpp` |
| F7 | The sandbox ignores a root that resolves to the filesystem root, and an empty project path is treated as "no root". `project/path` is no longer pre-seeded with `"./"`; it defaults to the detected WoW installation, and a legacy `"./"` value is migrated to it. | `script_filesystem.cpp`, `settings.hpp`, `application.cpp` |
| F8 | The write-consent set moved from a process-global static into `script_context`, so consent is per script context. | `script_context.hpp`, `script_filesystem.cpp` |
| F9 | `get_writable_path()` anchors the path with `fs::absolute()` before resolving it, so the prompt shows the real destination. | `script_filesystem.cpp` |
| F10 | `require_string_offset()` now also requires a NUL terminator inside the string table. | `DBCFile.h` |
| F11 | `existsOnDisk()` normalises and validates the filename before probing it. | `MPQ.cpp` |
| F12 | `MPQFile::get<T>()` rejects unaligned offsets; `M2Array` reads through `read_array()` so element alignment cannot matter. | `MPQ.h`, `Model.h` |
| F13 | The MCIN offset table now checks the result of each read. | `MapTile.cpp` |
| F14 | The `liquid_layer` copy assignment copies `_liquid_id` and `_need_data_update` like the copy constructor and the move assignment do. | `liquid_layer.cpp` |
| F15 | The OpenGL error check was moved out of the destructor into `check_for_gl_errors()`, so the destructor cannot throw. | `context.cpp` |
| F16 | The loader instance is owned and released through `AsyncLoader::shutdown()`, which `main()` calls once the application is torn down. | `AsyncLoader.cpp/.h`, `application.cpp` |

### Verification

- **The three original ASan proofs now pass.** `truncated.m2`, `badbones.m2` and `badanim.m2`
  produced `heap-buffer-overflow READ` reports before the fix; they now fail the load cleanly
  with **zero** sanitizer reports.
- **A 60-file mutation corpus** (random header byte flips and randomised count fields over a
  plausible vanilla M2) was run against an ASan+UBSan build of the **unmodified** code and
  against the fixed code:

  | Build | Result on the same 60 files |
  |---|---|
  | original (`HEAD`, ASan+UBSan) | **56 sanitizer reports** — 52 `heap-buffer-overflow`, 4 other |
  | fixed | **0** — 60/60 clean, no report, no crash, no hang |
- **Semgrep** re-run over the tree: the unchecked-buffer-access class dropped from **29 hits to
  5**, and all 5 remaining are false positives (each is preceded by an explicit `require_range`
  / size check that the rule cannot see).
- **cppcheck** re-run over the tree: 366 -> 363 diagnostics, no new findings introduced.
- **The Lua sandbox** was verified against the project's own Lua 5.4.8: `dofile`, `loadfile`,
  `load` and `loadstring` are absent from the state, and a script calling `loadfile('/etc/passwd')`
  now fails with "attempt to call a nil value".
- **The `require()` containment** was exercised against 8 module names; traversal, absolute paths
  and backslash forms are all rejected, ordinary module names still resolve. No shipped script
  uses `require`, `load`, `dofile` or `loadfile`, so nothing in the tree depends on the removed
  behaviour.
- **The project path default** was verified end to end: on first run `project/path` is now the
  detected WoW installation instead of the install directory, and an explicitly configured value
  is left untouched.
- Both build configurations (with and without the scripting module) compile clean, and the
  application still starts.

### Linux build verification

The whole point of a Windows-targeted C++/Qt codebase is that the fixes have to hold on the
platform you actually build on. Every configuration below was built from a clean tree.

| Configuration | Compiler | Flags | Result |
|---|---|---|---|
| CI-equivalent, scripting **ON** | GCC 15.2 | `-Wdev -Wdeprecated --warn-uninitialized`, `RelWithDebInfo` | builds clean |
| CI default, scripting **OFF** | GCC 15.2 | same | builds clean |
| ASan + UBSan | GCC 15.2 | `Debug` + `-fsanitize=address,undefined` | builds clean, 60/60 corpus clean |
| CI's other compiler leg | Clang 20.1.8 | `RelWithDebInfo`, scripting **ON** | builds clean |

- **No new warnings.** The fixed tree and the unmodified tree produce the *same* warning count
  under the CI's exact flags: **154 vs 154**. The only warning in a file this change touches is
  the pre-existing `std::iterator` deprecation at `map_index.hpp:48`, which this change does not
  modify.
- **The fix holds in optimised builds, not just Debug.** The `RelWithDebInfo` clang binary rejects
  the malformed M2 cleanly (exit 1), while the original code still reports
  `heap-buffer-overflow ... Model.cpp:210` for the same input.
- **Three files relied on transitive includes** for what the fix added (`<stdexcept>` in
  `Particle.cpp` and `Model.cpp`, `<filesystem>` in `script_image.cpp`). These are now explicit, so
  the build does not depend on libstdc++ and the MSVC standard library agreeing on what a header
  pulls in transitively.

### Platform support

noggit3 was **never Windows-only**. The CI matrix builds `ubuntu-24.04` with both `g++-14` and
`clang++-18` alongside `windows-2019` with MSVC 2019, and the build system has a
`cmake/linux_postfind.cmake` for the Linux side. The README heading "Building (Windows — the
known-good configuration for this fork)" describes the recipe this fork's author validated, not a
platform restriction.

Two gaps are worth knowing about, both pre-existing:

1. **`NOGGIT_WITH_SCRIPTING` defaults to `OFF`** and CI never enables it, so **the scripting
   module is not built by CI at all** — six of these fixes (F3, F5, F6, F7, F8, F9) live in code
   that CI never compiles. Covering it needs `liblua5.4-dev` in the CI image and
   `-DNOGGIT_WITH_SCRIPTING=ON`.
2. **The vendored sol2 fork does not compile on current toolchains.** `sol::optional<T&>::emplace`
   calls a `construct` member that the reference specialisation does not define, which breaks both
   GCC 15 and Clang 20. CI does not notice, because CI neither enables scripting nor uses those
   compiler versions. Fixing it properly means patching the sol2 dependency (one line: bind the
   reference instead of calling `construct`).

### New observation (not part of the original 16)

`noggit_terminate_handler()` (`application.cpp:577`) shows a **modal `QMessageBox`** when
`std::terminate` is reached. In the headless modes (`--asset-check`, `--adt-roundtrip`,
`--extract`) a failing file therefore turns into a process that blocks forever on an undismissable
dialog instead of exiting with an error code. This is pre-existing and was confirmed on the
unmodified code; it made an early version of the fuzz harness look like a hang. Worth fixing
separately: the handler should not open a modal dialog when running headless.
