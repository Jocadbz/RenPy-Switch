# Building and packaging

Reproducible, fail-fast pipeline for Ren'Py 8.5.3 on Switch. All stages run
unprivileged as your normal user; nothing mutates the devkitPro installation.
Every command fails the build instead of swallowing errors.

## Prerequisites (Arch Linux)

```fish
sudo pacman -S --needed devkit-env devkitA64 devkita64-cmake switch-dev \
  switch-mesa switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-ffmpeg \
  switch-freetype switch-harfbuzz switch-libfribidi switch-zlib switch-bzip2 \
  rsync cmake patch python
# Host Python 3.13 must match the pinned target Python:
uv python install 3.13.14
```

Verify the toolchain:

```fish
python3 tools/build.py doctor
```

## Pipeline

```
python3 tools/build.py all          # prepare -> python -> generate -> native -> runtime
```

Stages (also runnable individually):

- `sources`  download+verify pinned Ren'Py 8.5.3 and apply `renpy.patch` +
  `patches/renpy-8.5.3-runtime.patch` (dry-run first, zero fuzz)
- `python`   cross-build pinned CPython 3.13.14 as a static library
  (`build/python/install`) using only `setup.py`-style targeted edits
- `generate` run upstream's own `setup.py generate` Cython pass, then validate
  that every declared extension produced a static `PyInit_*` symbol and emit
  `build/generated/{modules.cmake,module_declarations.h,module_entries.h}`
- `native`   configure CMake with the devkitPro toolchain, compile the engine
  and the generated module sources, link `build/native/renpy-switch.elf`
- `runtime`  stage `build/runtime/romfs` (stdlib + Ren'Py Python in a
  source-only `lib.zip`, licenses, switch bootstrap) and write `runtime.json`

`doctor` checks the compiler, pkg-config modules and host tools. Dependencies
are pinned by SHA-256 in `tools/versions.json`; the devkitPro package set that
produced the build is recorded in `runtime.json`.

## Packaging a game

```
python3 tools/package_game.py /path/to/extracted-game --inspect   # report only
python3 tools/package_game.py /path/to/extracted-game \
    --output build/MyGame --id my-game-id --name "My Game" \
    --author "…" --version "1.0.0" --icon icon.png
```

- The source directory is never modified; packaging copies into a temp tree.
- Validation rejects symlinks, oversized files, malformed RPA headers and
  missing script content; it warns about native extensions, Live2D, assimp,
  process spawning, TLS and file dialogs.
- Engine version evidence is collected from `cache/build_info.json`,
  `renpy/vc_version.py` and `log.txt` when present; missing or non-8.5.3
  evidence is reported instead of guessed.
- The result contains the `.nro` (validated header/NACP/RomFS bounds),
  `package.json` (hashes, inventory, warnings), licenses and this test plan.

## Hardware

See `docs/HARDWARE_TEST.md`. Nothing here counts as hardware-verified until a
run against a real console is recorded. Host builds only prove compilation.
