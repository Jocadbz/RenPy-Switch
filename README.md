# Ren'Py for Nintendo Switch

Native [Ren'Py](https://www.renpy.org/) engine for the Nintendo Switch.

## Credits

**All credit for the Switch port belongs to
[SnepOMatic](https://github.com/SnepOMatic) and the
[SnepOMatic/RenPy-Switch](https://github.com/SnepOMatic/RenPy-Switch)
project.** Everything that makes Ren'Py run on the Switch is their work:

- the engine platform patch (`renpy.patch`),
- the native launcher (`sources/main.c`),
- the splash screen and game picker (`sources/game_picker.c`),
- the libnx adaptations, POSIX shims and storage handling,
- the years of device-specific debugging (Mesa/nouveau behavior, heap
  sizing, reload crashes) reflected in that code.

This repository does **not** re-implement any of that. It only makes the
existing port easier to build and use, by:

- pinning all downloads with SHA-256 hashes,
- building the static CPython in-tree (replacing the external PyNX
  dependency),
- providing a fail-fast, unprivileged build script (`tools/build.py`)
  instead of the machine-specific setup,
- adding a validating game packager (`tools/package_game.py`),
- documenting and recording what has actually been verified on hardware.

If you're looking for the port itself, it lives upstream — this is a
simpler way to build and package it.

## Status

Hardware-verified on a real console (application mode): engine boot, splash,
GL2 renderer (nouveau), controller input, interactive dialogue, save → quit →
relaunch → load, frame-sequence animation galleries, audio, and on-screen
keyboard input. Also verified: a real Ren'Py 7.4.8 title packaged unmodified,
including its 985 MB RPA-3.0 archive, saves and gallery animations.

Not yet verified on hardware: codec video playback (`webm`/`mp4`
`movie_cutscene`), sleep/resume, applet-mode (album) relaunch, and games
authored for Ren'Py 8.2.x. See [docs/AUDIT.md](docs/AUDIT.md) for the full,
honest accounting of what is and is not verified.

## Requirements

- Arch Linux (or any distro with the same devkitPro packages) + any shell
- devkitPro: `devkit-env devkitA64 devkita64-cmake switch-dev switch-mesa
  switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-ffmpeg switch-freetype
  switch-harfbuzz switch-libfribidi switch-zlib switch-bzip2 rsync`
- host Python 3.13 (matching the pinned target Python), e.g. `uv python install 3.13.14`
- `cmake`, `patch`

Check the toolchain with:

```
python3 tools/build.py doctor
```

## Build

```
python3 tools/build.py all
```

This downloads pinned sources (SHA-256 verified), applies the patches
(`renpy.patch` + `patches/renpy-8.5.3-runtime.patch`), cross-builds static
CPython, runs upstream's Cython generation, compiles the engine, and stages
the runtime. Individual stages: `doctor sources python generate native runtime`.

## Package a game

```
python3 tools/package_game.py /path/to/extracted-game --inspect
python3 tools/package_game.py /path/to/extracted-game \
    --output build/MyGame --id my-game --name "My Game" \
    --author "…" --version "1.0.0" --icon icon.png
```

The source game is never modified. Validation detects engine versions from
shipped evidence, checks RPA headers, and warns about native extensions,
Live2D, assimp, subprocesses, TLS and file dialogs. Each package is a
self-contained `.nro` (engine included) plus `package.json`, licenses and the
hardware test plan. See [docs/BUILD.md](docs/BUILD.md).

## Deploy

Copy the `.nro` to `sdmc:/switch/` and launch in **application mode**
(title override / forwarder). Saves live in `sdmc:/switch/renpy/data/<id>/`;
logs in `sdmc:/renpy_switch.log`. See [docs/HARDWARE_TEST.md](docs/HARDWARE_TEST.md).

## Documentation

- [docs/BUILD.md](docs/BUILD.md) — full pipeline and packaging reference
- [docs/HARDWARE_TEST.md](docs/HARDWARE_TEST.md) — deployment, smoke test, crash reporting
- [docs/AUDIT.md](docs/AUDIT.md) — audit findings, fixed defects, verification status

## Licensing

The engine is Ren'Py (MIT and per-component licenses); CPython is PSF-licensed;
FFmpeg and other dependencies have their own terms. Packaged NROs ship the
corresponding license texts. **This repository has no license grant for its own
port files yet** — the original repo did not carry one, and none is invented
here. Choose and add one before redistributing.
