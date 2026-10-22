# Port audit — 2026-10-19, updated 2026-10-21 for public release

Baseline: SnepOMatic/RenPy-Switch `ed82ccfaf0784e17216153d1ce2e678074bc6e13`,
branch `8.5.3`. No build outputs or dependency sources were present. The initial
CMake configure failed because devkitPro was absent. This audit was written
before any hardware results existed; see "Verification status" below for what
has since been verified on a real console.

## Actual version and structure

The build and patch target **Ren'Py 8.5.3.26051504**, not the README's 8.5.7.
`renpy.patch` applies to the official 8.5.3 source with zero fuzz. It does not
constitute an 8.2 port. The native module table uses the merged `renpy.pygame`
layout introduced after 8.2. The selected static Python ABI is **3.13**, with
3.13.14 named by the build scripts. Neither Python nor Ren'Py is vendored.

- `sources/main.c`: embedded Python, libnx services, SD storage bootstrap,
  generated module table, splash, external archive selection, and shutdown.
- `sources/game_picker.c`: independent SDL picker and a custom ZIP reader. The
  picker is not called by main; its splash helper is.
- `renpy.patch`: 24 files covering Python, Cython, bootstrap, archives, saves,
  graphics workarounds, optional assimp, and RNG support.
- `patches/renpy-8.5.3-runtime.patch`: maintained SD-storage, shader-cache,
  bootstrap-marker and relaunch changes; generated mechanically by
  `tools/make_runtime_patch.py` against the renpy.patch-applied tree.
- `tools/build.py`: unprivileged, fail-fast pipeline (sources/python/generate/
  native/runtime stages). `tools/package_game.py`: validating game packager.

The old shell pipeline (setup.bash, build.bash, repack_lib.sh, package_game.sh
and friends), the checked-in header inventory (`include/`), the unused root
`ffmedia.c`, `sources/video_player.c` (a disabled standalone intro player) and
the stale historical notes (BUILD_NOTES_8.5.3.md, SWITCH_PORT_MAP.md) were
removed during the 2026-10-21 cleanup; their defects are summarized below and
fixed by the replacement pipeline.

## Priorities and confirmed defects

### P0 — reproducible build and diagnostics (all fixed)

1. ~~`setup.bash` runs an unconditional system upgrade, requires apt, mutates the
   installed toolchain, copies an absent sibling Python build, and applies the
   libhydrogen patch twice~~ — removed; `tools/build.py` never touches the
   toolchain and pins every download by SHA-256 (`tools/versions.json`).
2. ~~Cython, native build and script compilation failures are swallowed with
   `|| true`~~ — every stage propagates failures; generated modules are
   validated for static `PyInit_*` symbols before linking.
3. ~~CMake mixes target headers with `/usr/include/python3.13`, overrides
   pkg-config cache variables with absolute paths, omits static transitive
   dependencies, and requests SDL_mixer/gfx~~ — CMake now discovers everything
   through switch-pkg-config; explicit `--start-group`/`--end-group` resolves
   static cycles; SDL_mixer/gfx requests dropped.
4. ~~Dependencies lack hashes/version locks; repacking can mix SDK bytecode and
   host Python; generated headers are flattened~~ — versions.json pins all
   sources; the runtime manifest records devkitPro package versions and every
   artifact hash; `lib.zip` is built from target sources only.
5. ~~No automated tests, build manifest, or staged launch/import diagnostics~~ —
   the smoke fixture, boot markers `[boot:1]`–`[boot:8]`, `runtime.json`
   manifest and the hardware test plan now exist.

### P1 — native boot, storage, correctness (partially fixed)

1. ~~NRO startup requires account services and creates/mounts NAND savedata using
   the hbloader host ProgramId~~ — fixed: saves live at
   `sdmc:/switch/renpy/data/<id>/`, keyed by a validated package id.
2. ~~Pre-initialization errors call Python APIs before Python exists;
   PyStatus/allocation checks missing; `lib.zip` appended twice~~ — fixed:
   preflight file checks, PyStatus checks on every config call, single append.
3. ~~Hash randomization is disabled despite an entropy implementation;
   libhydrogen uses a time-seeded xorshift~~ — entropy now comes from libnx
   randomGet (`_getentropy_r`), and hash randomization is enabled.
4. Cleanup and shutdown paths remain delicate (SystemExit treated as success,
   applet-mode `_exit`); verified working on hardware in application mode,
   but applet-mode lifecycle is still untested.
5. ~~`posix_memalign` accepts invalid alignment; entropy shim ignores the
   256-byte getentropy limit~~ — fixed; remaining broad POSIX stubs are listed
   in `sources/switch_stubs.c` and return ENOSYS.
6. Transition preferences are forcibly disabled. glFinish and disabled vsync are
   global workarounds retained until a device trace identifies which are still
   needed. (MESA_NO_ERROR is no longer set.)

### P2 — broad game compatibility

- Save isolation is now per-package-id, covering games without
  `config.save_directory`. Token/backup requests get their own directories.
- ZIP fallback fully decompresses entries into memory (including large RPAs).
  The native ZIP reader lacks ZIP64/CRC and robust length/method validation.
  (Used only by the external-archive/splash path, not by packaged games.)
- Autosave is disabled; sleep/resume, Joy-Con reconnect, touch coordinates and
  applet-mode relaunch remain untested. Codec video playback (webm/mp4) is
  untested — no verified game exercises it. Audio and on-screen keyboard input
  have been exercised on hardware.
- Third-party native extensions, Live2D, assimp, subprocesses, updater/networking
  and system-dependent Python packages need explicit capability reporting.
  `tools/package_game.py` now reports these as packaging warnings.
- ~~Packaging does not detect engine versions, validate RPA headers or native
  dependencies, and ignores script-compile errors~~ — fixed: the packager
  detects engine versions from shipped evidence, validates RPA headers, warns
  on unsupported features, and never modifies the source game.

## Comparison and reuse

- jvrcruzGAMES/RenpySwitch `7824039a7d0353b7bfa79ddfcecbf30a375d3d62`:
  targets 8.3.4, separate pygame_sdl2 and Python 3.14 (NXPython). Its cached
  downloads/venv and separation of modules/runner are useful ideas. It still
  ignores some generation failures and mixes SDK stdlib with another Python ABI.
  Its PFS0 `.nsp` assembly is not evidence of an installable title package.
- SnepOMatic/PyNX `da1553324505fd7af04a87f5c1b5b06f519959a8`:
  useful evidence for newlib configuration, libnx pthreads, static module setup,
  and subprocess restrictions. It also copies stale CPython internals and edits
  posixmodule with broad substitutions. Prefer small patches to pinned CPython.
- Upstream 8.5.3 already provides `setup.py generate`: Cython generation does not
  need a deliberately failing host build. Reuse its style generator, static init
  symbol naming, FFmpeg backend, SDL bindings, gl2 renderer and HarfBuzz shaping.
- Official 8.2.3.24061702 source was downloaded for comparison. Its `module/` and
  separate pygame_sdl2 layout require a distinct build/patch profile. Testing
  8.2-authored games on 8.5.3 is different from building the 8.2 engine.

## Shortest baseline

Pin 8.5.3 and 3.13.14; build a complete static Python without machine-specific
installation; use upstream Cython generation and target-only dependency discovery;
link ELF and package NRO with a source-only stdlib and minimal dialogue fixture.
Add staged import diagnostics and SD storage. Retain existing graphics adaptations
until a device trace identifies which are necessary. Test 8.2 content separately.

Current installed target packages: devkitA64 r30 / GCC 16.1.0, newlib
4.6.0.20260123-4, libnx 4.12.0, SDL2 2.28.5, Mesa 20.1.0, FFmpeg 7.1,
HarfBuzz 10.0.1, FreeType 2.13.3, FriBidi 1.0.12. Record exact package releases
in build artifacts; a rolling package install alone is not a reproducible lock.

## Verification status (2026-10-19/20, this session)

Verified by execution on this host — not on Switch hardware:

- `renpy.patch` applies to pinned 8.5.3 source with zero fuzz; a second
  maintained patch (`patches/renpy-8.5.3-runtime.patch`) applies after it.
- CPython 3.13.14 cross-builds to a static `libpython3.13.a` for
  aarch64-none-elf (devkitA64). Required source edits: configure accepts
  aarch64-none-elf; newlib's 64-bit clockid_t in timemodule.c; horizon
  excluded from subprocess fork paths. Feature detection comes from
  config.site + configure, not vendored CPython files.
- Upstream `setup.py generate` (Cython) runs to completion with exception
  propagation; all 60 declared extensions were validated to contain static
  `PyInit_*` symbols, and `build/generated/` drives the inittab and CMake
  source list. `renpy.tfd` (desktop dialogs) is skipped.
- The engine links as `renpy-switch.elf` using only target headers/libraries
  discovered through switch-pkg-config; no host Python or archive overrides.
- `build.py` stages a source-only `lib.zip` (stdlib + renpy + ecdsa/six),
  disk-side `renpy/common`, splash, licenses and a deterministic runtime
  manifest that records devkitPro package versions.
- Packaging validates the fixture, reports unknown engine version for games
  without evidence, and rejects malformed inputs.

Still unverified (requires Switch hardware or further work):

- **Hardware boot — VERIFIED 2026-10-21** (user log, real console,
  application mode via title-override, `renpy-switch-smoke.nro`
  sha256 `a80d1338…`): full `[boot:1]`–`[boot:8]` chain, splash, gl2 renderer
  (nouveau NV120, GL 4.3 compatibility, Mesa 20.1.0-rc3, 1920x1080@60),
  Switch Controller detected, interactive dialogue reached multiple times,
  and clean quit via svcExitProcess → HOME.
- **Real game — VERIFIED 2026-10-21** (user report, real console):
  an unmodified commercial visual novel built with **Ren'Py 7.4.8**
  (985 MB RPA-3.0 archive, 3112 entries, 28 scripts), packaged with the
  pipeline above. Boots, dialogue runs, **saves work**
  (save → quit → relaunch → load), the in-game animation galleries
  (ATL frame-sequence "movies" over JPGs — no codec movies are used by
  this game) play, and on-screen **keyboard input works**. This also
  validates the 7.4.8 → 8.5.3 rpyc migration path end to end.
- Found on hardware and fixed host-side: shader cache wrote
  `romfs:/Contents/game/romfs:/Contents/game/cache/shaders.txt` at quit
  (posixpath misreads `romfs:` prefixes as relative; joined with gamedir;
  target read-only). Now routed to `sdmc:/switch/renpy/data/<id>/cache`.
- A single data-abort (0x101) was observed once in
  `__pyx_pymod_exec_uguu` on the previous NRO; it has not reproduced on two
  later boots of the rebuilt NRO. Cause not established — if it recurs,
  collect `sdmc:/atmosphere/crash_reports/` and the extended dump.
- Not yet verified: codec video playback (webm/mp4 `movie_cutscene` /
  `Movie` — no tested game uses them), audio/music, suspend/resume,
  applet-mode relaunch, MultiPersistent, games authored for 8.2.x.
- `renpy.patch` content was not re-audited line-by-line after this session's
  bootstrap changes; graphics workarounds are retained until a device trace
  shows which are still needed.

## Licensing

Preserve upstream source notices and ship dependency licenses. The repository has
no top-level license grant for all original port files; do not invent one.
Ren'Py includes MIT-licensed engine code and components with other licenses;
FFmpeg build options affect redistribution obligations. Record provenance and
retain source/patches alongside binaries before publishing a release.
