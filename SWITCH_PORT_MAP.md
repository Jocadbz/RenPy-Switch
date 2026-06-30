# Ren'Py → Nintendo Switch Port Map

A change-by-change map of what the RenpySwitch fork's `renpy.patch` does, why, and
where to re-apply it. The patch was authored against **Ren'Py ~8.1 / 7.6.3 (mid-2023)**
and will not apply cleanly to newer trees (8.3.x, 8.5.x). Rather than force the stale
diff, use this document to re-insert the port by hand at the equivalent locations in
your target source.

> Scope: this describes the *Python/`.rpy` side* of the port plus the C touch-points.
> The heavy lifting — the libnx entry point and the `_otrhlibnx` native module — lives
> in the fork's `sources/main.c`, not in Ren'Py, and is summarized in section 2.

---

## Mental model (read this first)

- **One flag gates everything.** The port adds `renpy.switch` (a bool). Almost every
  change is an `if renpy.switch:` / `or renpy.switch` branch. Add the flag first, then
  the rest are local conditionals.
- **Switch OS is "Horizon".** Detection is `sys.platform == 'horizon'`. In pure-Python
  spots that can't import `renpy` yet (e.g. `renpy.py`), the code checks
  `sys.platform == 'horizon'` directly.
- **Two filesystem mounts.** `romfs:` is read-only (ships the game + engine);
  `save:` is the only writable location (saves, logs, persistent, cache).
- **A native bridge does the OS-specific work.** The `_otrhlibnx` C module exposes a
  handful of Horizon calls. Without it, the lifecycle and save code below cannot run.

---

## 1. The platform flag — do this first

**File:** `renpy/__init__.py`

| Intent | Change |
|---|---|
| Declare the flag | Add `switch = False` alongside `android`, `ios`, `emscripten` |
| Detect Horizon | In the `sys.platform` ladder, add `elif sys.platform == 'horizon': switch = True` |
| Treat as mobile | `mobile = android or ios or emscripten or switch` |
| Never "official" | Force `official = False` |
| Skip encryption import | Wrap `import renpy.encryption` in `if not switch:` |

**8.5.3 note:** the flag declarations, the platform-detection ladder, and the `mobile =`
line all still exist in `__init__.py` and are easy to locate. Re-evaluate the
`renpy.encryption` skip — it pairs with the fork's `switch_enc.h` / `encryption.patch`
(libhydrogen), so only keep it if you reproduce that libhydrogen change.

---

## 2. The native bridge — `_otrhlibnx` (the real dependency)

This is a C extension provided by the fork's native side, **not** by Ren'Py. The Python
patch only *calls* it. If you port nothing else at the C level, you port this.

| Function | Purpose | Called from |
|---|---|---|
| `commitsave()` | Flush save data to Switch storage (Horizon requires an explicit commit) | `__init__.reload_all`, `bootstrap.py`, `savelocation.py` |
| `restartprogram()` | Relaunch the title (Switch can't `fork`/`exec`; "reload" = restart) | `__init__.reload_all`, `bootstrap.py` |
| `startboost()` | Enter CPU/GPU boost mode (faster loading) | `bootstrap.py` startup |
| `disableboost()` | Leave boost mode once the window is up | `display/core.py` |

**8.5.3 note:** unchanged — this is your own module, version-independent. Get it building
and importable (`import _otrhlibnx`) before wiring the calls below.

---

## 3. Filesystem & save-path rerouting

The single largest functional area. Everything writable goes to `save:`, everything
shipped is read from `romfs:`.

| File | Intent | Mapping |
|---|---|---|
| `renpy.py` | Hardcode engine paths when `sys.platform == 'horizon'` | game → `romfs:/Contents/game`; common → `romfs:/Contents/renpy/common`; base → `romfs:/Contents`; save dir → `save:/Data/` |
| `loader.py` | Route cache + searchpath | `cache/*` → `save://cache`; honor `romfs:`-prefixed searchpath entries (don't re-prefix with basedir) |
| `main.py` | Restrict searchpath | `config.searchpath = [ gamedir, "save://" ]` on switch |
| `bootstrap.py` | Log location | `config.logdir = "save:/Logs/"` |
| `persistent.py` | Multipersistent | `["save://MultiPersistent"]` |
| `savelocation.py` | Save root + commit | location → `FileLocation("save:/Data/")`; call `_otrhlibnx.commitsave()` after writes; disable the background scan thread (`quit()` early-returns); fake mtimes (read-only romfs has none) |
| `loadsave.py` | (see §7 autosave) | don't start the autosave thread |
| `log.py` | Logging sink | write to `save://Logs/log.log`; treat switch like `macapp` (logging mostly off) |
| `script.py` | No `.rpyc` writes | skip writing compiled `.rpyc` (romfs is read-only) — same branch as `macapp` |

**8.5.3 note:** the function shapes (`path_to_saves`, `path_to_logdir`,
`FileLocation`, `Loader.transfn`/searchpath builders) are stable in spirit but signatures
and helper names have drifted. Find by function name, not line number. Verify the
`save://` / `romfs:` VFS prefixes are still recognized by your cross-compiled
`pygame_sdl2` RWops layer.

---

## 4. App lifecycle — restart / quit / reload

Switch can't spawn subprocesses or exit the process the normal way.

| File | Change |
|---|---|
| `renpy/__init__.py` (`reload_all`) | On switch: `commitsave()` + `restartprogram()` then `return` (no in-process reload) |
| `bootstrap.py` | On `UtterRestart`/`Quit`/`relaunch`: `commitsave()` + `restartprogram()`; final `finally:` ends with `os._exit(0)` (no `subprocess.Popen` relaunch) |
| `display/core.py` | Don't call `sys.exit(0)` on switch during shutdown |
| `main.py` | After `--compile`-style early exits, skip `sys.exit(0)` on switch |

**8.5.3 note:** the `try/except UtterRestartException/QuitException` block in
`bootstrap.py` is the trickiest to rebase — it has been refactored across versions.
Re-create the *intent* (commit, restart, hard-exit) rather than transplanting the hunk.

---

## 5. Rendering

Forces the legacy GL/GLES path at a fixed handheld/docked resolution.

| File | Change |
|---|---|
| `config.py` | `gl2 = True if not renpy.switch else False` — use the old `gl` renderer, not gl2 |
| `display/core.py` | renderer list = `["gles"]` on switch; no `"sw"` software fallback |
| `gl/gldraw.pyx` | force fullscreen + 1920×1080; recreate GL context like android/ios; set `redraw_period = 1.0` |
| `gl2/gl2draw.pyx` | add a switch branch: `RESIZABLE`, `gles = True` |
| `display/presplash.py` | add `start_switch(gamedir)` — load `presplash.png/.jpg` at 1920×1080 via pygame and flip once |

**8.5.3 note — highest-risk area.** The renderer changed substantially after 8.1: gl2 /
the model-based renderer is now the default path and the old `gl` renderer may be
deprecated or gone. Forcing `gl2 = False` may no longer be viable. Plan to test the
**gl2/gles** path on Switch instead, and treat the `gldraw.pyx` hunks as guidance, not a
recipe. Confirm what renderers your devkitPro GLES/EGL portlibs actually support.

---

## 6. Input — controller mapping

| File | Change |
|---|---|
| `renpy/common/00keymap.rpy` | Remap `config.pad_bindings` for a controller-only device: A=confirm/rollback, B=dismiss/activate, X=toggle skip, Y=hide windows, L/R triggers=dismiss/activate, L/R-stick click=quick save / quick load. Add `quick_save`/`quick_load` keymap entries + `K_F5`/`K_F6` and the `QuickSave()`/`QuickLoad()` actions |
| `display/behavior.py` | On switch, return `ev.text` directly for text-input events (drives the Switch on-screen software keyboard) |

**8.5.3 note:** `00keymap.rpy` is data-ish and rebases cleanly; just merge the binding
dict against the current defaults. Confirm the `pad_*` event names still match.

---

## 7. Features disabled on Switch

| Feature | File(s) | Change |
|---|---|---|
| Autosave | `config.py`, `loadsave.py` | `has_autosave=False`, `autosave_on_choice=False`, `autosave_on_input=False`, `save_on_mobile_background=False`; don't start the autosave thread |
| Self-updater | `renpy/common/00updater.rpy` | drop `urllib`/`future.utils`, use `requests` + builtin `exec` |
| Cloud sync | `renpy/common/00sync.rpy` | `upload()`/`download()` early-return on switch |
| Audio teardown | `renpy/audio/audio.py` | `quit()` early-returns (don't tear down the audio subsystem) |

**8.5.3 note:** these are small, low-risk conditionals. Autosave defaults you may prefer
to set per-game in `options.rpy` rather than editing engine `config.py`.

---

## 8. Text shaping — HarfBuzz → FreeType

HarfBuzz wasn't built for the Switch toolchain, so shaping falls back to FreeType.

| File | Change |
|---|---|
| `renpy/common/00style.rpy` | `shaper "freetype"` (instead of `"harfbuzz" if not PY2 else "freetype"`) |
| `renpy/text/__init__.py` | don't `from . import hbfont` |
| `module/setup.py` | remove the `hb.h`/harfbuzz `include(...)` and the `renpy.text.hbfont` cython module |

**8.5.3 note — verify.** If you can cross-compile HarfBuzz with devkitPro, you can skip
this whole section and keep proper shaping. Newer Ren'Py also leans more on HarfBuzz, so
removing `hbfont` may have wider knock-on effects than it did in 8.1. Try building
HarfBuzz first.

---

## 9. Native C — `module/ffmedia.c`

Removes the `USE_POSIX_MEMALIGN` code path, always using `SDL_calloc`/`SDL_free` instead
of `posix_memalign`/`free` (newlib/Switch alignment behavior).

**8.5.3 note:** the fork overwrites `module/ffmedia.c` wholesale with its own copy
(`cp -rf ffmedia.c renpy-source/module` in `build.bash`), so this patch hunk is
effectively dead — maintain the standalone `ffmedia.c` instead.

---

## 10. Build-environment busywork — mostly skip on a modern tree

A large share of the patch's line count is *not* Switch logic. It exists because of the
old cross-compiled Python 3.9 / Cython 0.29 environment:

- **`from typing import ...` removed** from ~20 modules (`ast.py`, `character.py`,
  `config.py`, `easy.py`, `game.py`, `python.py`, `screen.py`, `transform.py`,
  `lint.py`, `loader.py`, etc.).
- **`future.utils` → `six`** for `raise_`/`reraise` (`audio.py`, `execution.py`,
  `loadsave.py`).
- **`compat/__init__.py`:** hardcode `PY2 = False`; add a manual `surrogateescape` codec
  handler (the Switch Python build lacked the builtin); skip the `Popen` subclass on
  `horizon`.

**8.5.3 note:** re-evaluate, don't copy. A current Python 3.11+/Cython 3 build almost
certainly does *not* need the `typing` removals or `future`→`six` swaps, and Ren'Py has
moved on from `future`/PY2 anyway. The only piece worth checking is whether your
cross-compiled Python provides the `surrogateescape` error handler; if not, port that one
codec shim.

---

## Suggested rebase order

1. **Add the `renpy.switch` flag** (§1) — nothing else compiles meaningfully without it.
2. **Build `_otrhlibnx`** and get the app to *boot to a black screen* importing it (§2).
3. **Filesystem paths** (§3) — get saves/logs/persistent landing in `save:`.
4. **Lifecycle** (§4) — clean start/restart/quit without crashing on exit.
5. **Rendering** (§5) — get a frame on screen; expect to debug the gl2/gles choice.
6. **Input** (§6) — controller + on-screen keyboard.
7. **Disable features** (§7) and **text shaping** (§8) as needed.
8. Skip §10 unless a specific build error points you back to it.

Test after each step rather than porting everything then booting once.

---

## Verification checklist

- `grep -rn "renpy.switch" renpy/` — every branch you intended is present.
- `import _otrhlibnx` succeeds on device.
- Saves, persistent, and logs appear under `save:/…`; nothing tries to write to `romfs:`.
- Clean exit and restart (no hang, no `sys.exit`/subprocess path taken).
- A frame renders; resolution is correct docked and handheld.
- Controller navigation + software keyboard work.
- Save → restart → load round-trips correctly.

---

## Caveats

This map is derived from the fork's 2023-era `renpy.patch` (Ren'Py ~8.1 / 7.6.3). The
**rendering** (§5) and **text shaping** (§8) areas changed the most in later releases and
are the likeliest to need a different approach rather than a direct re-apply. Treat exact
file locations as starting points — find code by function name, and confirm each
insertion point still exists in 8.5.3 before adding the conditional.
