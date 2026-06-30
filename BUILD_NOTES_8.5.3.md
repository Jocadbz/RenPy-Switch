# RenpySwitch → Ren'Py 8.5.3 build notes

Record of everything done to take the RenpySwitch fork (built for ~Ren'Py 8.1/8.3,
Python 3.9, SDL2, old `module/` layout) to a **building Ren'Py 8.5.3 + CPython 3.13.14
`.nso`** on a current devkitPro/devkitA64 (GCC 16) toolchain.

## Status
- ✅ Engine + all cython/C modules + libpython3.13 **compile and link** → `renpy-switch.nso`.
- ⬜ Packaging into a bootable homebrew (romfs/lib.zip), runtime Switch conditionals, on-device test.

## Target versions
| Component | Version | Notes |
|---|---|---|
| Ren'Py | 8.5.3 | source layout reorged vs the fork's assumptions (see below) |
| CPython (Switch) | 3.13.14 | built via PyNX (see `PyNX_.../BUILD_NOTES_3.13.14.md`); SDL2 still (8.5.3 isn't SDL3 yet) |
| Cython (host) | ≥3.0.11 (3.2.5 used) | 0.29.x can't target 3.12/3.13 |
| FFmpeg | 7.1 (devkitPro `switch-ffmpeg`) | prebuilt; building 6.0 from source ICEs GCC 16 |
| pygame_sdl2 | merged into `renpy.pygame` | no separate download as of 8.5 |

## 8.5.3 layout changes the fork didn't expect
- `pygame_sdl2` merged into the engine as `renpy.pygame.*` (no separate project/download).
- C sources moved `module/` → **`src/`** (incl. `src/libhydrogen`, `src/pygame`).
- Single **root `setup.py`** (no `module/setup.py`); cython output goes to `tmp/gen3-static/`.
- Legacy `gl` renderer removed (only `gl2`); `font`/`mixer`/`mixer_music` gone.

## setup.bash changes
- Host Python 3.9 → **3.13** (`python3.13-venv`, not `-distutils`); Cython `0.29.37` → `>=3.0.11`; dropped `typing`.
- `get-pip` + `pip --upgrade pip` made non-fatal (`|| true`) — Debian's pip can't self-uninstall.
- `libgl1-mesa-glx` → `libgl1` (Ubuntu 24.04 rename).
- Replaced the from-source FFmpeg build with `dkp-pacman -S switch-ffmpeg` (GCC 16 ICEs on FFmpeg 6.0 `vp9recon.c`/`indeo3.c`).
- Dropped the `pygame_sdl2` download/extract (merged); `PYGAME_SDL2_VER` 2.28.5 → 2.1.0 was moot.
- libhydrogen + `switch_enc.h` + `encryption.patch` repointed `module/` → `src/libhydrogen/impl`.
- `renpy.patch` now regenerated from the hand-ported tree by `make_renpy_patch.sh` and applied; gen cleanup → `tmp/gen3*`.
- Installs the PyNX `libpython3.13.a` + `nxpy3.13.14/include` into `portlibs/switch` (via `$NXPY`).

## build.bash changes
- Cythonize via **root `setup.py build_ext`** (was `module/setup.py`); gather `tmp/gen3-static/*.c` + `src/**/*.c|cc` into a flat `source/module/`.
- Excluded modules with no Switch backend: `assimp` (no libassimp), `tinyfiledialogs`/`renpy.tfd` (termios), `live2dcsm` (no Cubism).
- `hydrogen.c` not compiled standalone (libhydrogen is unity-built via `renpy.encryption`) — avoids duplicate symbols.
- Added `sources/switch_stubs.c`: ENOSYS stubs for `fdatasync`/`fchdir`/`chroot`/`setgroups`/`pause` (CPython posix/pthread) and `posix_memalign` (ffmedia) — newlib lacks them, Ren'Py never calls them on Switch.

## CMakeLists.txt changes
- `python3.9` → `python3.13`, `libpython3.9.a` → `libpython3.13.a`.
- Source list replaced with `file(GLOB ...)` over `source/module/*.c|cc`.
- Added defines `-D_GNU_SOURCE -DSSIZE_MAX=__LONG_MAX__` (newlib doesn't define `SSIZE_MAX`).
- Added include paths: flat `include/module` + libhydrogen subdirs + the original `renpy-source/src`, `renpy-source`, `renpy-source/tmp/gen3-static`, and portlibs `harfbuzz`/`freetype2`/`SDL2` (path-qualified includes like `pygame/x.h` resolve against the real tree).

## sources/main.c changes
- Rebuilt the `builtins[]` `_inittab`: `pygame_sdl2.*` → `renpy.pygame.*` with the **full** cython init names (`PyInit_renpy_pygame_color`, not `PyInit_color`); dropped `font`/`mixer`/`mixer_music` and the `renpy.gl.*` renderer; `_renpybidi` → `renpy.text.bidi`; added `renpy.astsupport`, `renpy.cslots`, `renpy.gl2.gl2uniform`, `renpy.text.hbfont`.

## Ren'Py source conditionals (hand-ported, applied cleanly via renpy.patch)
The `renpy.switch` flag + `if renpy.switch:` branches across `renpy/__init__.py`, `renpy.py`,
`config.py`, `main.py`, `loader.py`, `loadsave.py`, `log.py`, `persistent.py`,
`savelocation.py`, `script.py`. See `SWITCH_PORT_MAP.md`. Runtime-only conditionals
(`bootstrap.py`, `core.py`, input, presplash, rendering) are still TODO — they don't block
compiling but are needed to boot/render.

## Reproduce
```bash
sudo bash setup_wsl.sh                 # once (WSL env: toolchain, python3.13, devkitPro)
cd RenpySwitch
bash make_renpy_patch.sh               # regen renpy.patch from the hand-ported tree
sudo bash setup.bash                   # downloads + libpython install + patches
sudo bash build.bash                   # cythonize + cross-compile + link -> .nso  (+ package)
```
Iterate the compile/link without re-gathering: `cd build && cmake .. && make`.
