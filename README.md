# Ren'Py for Nintendo Switch

A simpler way to build and package games with
[SnepOMatic's Ren'Py Switch port](https://github.com/SnepOMatic/RenPy-Switch).

## Credits

**All credit for the Switch port goes to
[SnepOMatic](https://github.com/SnepOMatic) and the upstream contributors.**
Their work provides the engine patches, native launcher, splash screen,
game picker and Switch-specific code that this fork builds on. The upstream
project was initially based on
[greg6821-debug/RenpySwitch](https://github.com/greg6821-debug/RenpySwitch).

Our part is making that work easier to set up. The build scripts download
the required sources, check their hashes, build Python and the engine, and
prepare games for packaging. We've also cleaned up the setup and documented
our hardware testing. Thanks to SnepOMatic for making the port possible.

## Status

The current build uses **Ren'Py 8.5.3 and CPython 3.13.14**. It has booted,
displayed the splash screen and reached dialogue on a real Switch in
application mode. Saves, frame-based animation galleries and the on-screen
keyboard have also been reported working in a tested game.

Audio, codec video playback, sleep/resume, applet mode and games made with
Ren'Py 8.2.x still need hardware testing. Packaging a game successfully
doesn't guarantee it will run. See [docs/AUDIT.md](docs/AUDIT.md) for more
details.

## Requirements

- Arch Linux with devkitPro installed. Other Linux distributions may work
  with equivalent packages.
- devkitA64, libnx and the Switch libraries listed in
  [docs/BUILD.md](docs/BUILD.md#prerequisites-arch-linux).
- Host Python 3.13. You can install the matching version with
  `uv python install 3.13.14`.
- `cmake`, `make`, `patch` and `rsync`.

## Build

Once the dependencies are installed, check your setup and build:

```fish
python3 tools/build.py doctor
python3 tools/build.py all
```

The build downloads the sources, applies the patches, compiles Python and
the engine, and prepares the runtime under `build/`. It stops if a step
fails. You can also run each stage separately: `sources`, `python`,
`generate`, `native` or `runtime`.

## Package a game

Start with an extracted game directory. To inspect it without building a
package:

```fish
python3 tools/package_game.py /path/to/extracted-game --inspect
```

Then package it:

```fish
python3 tools/package_game.py /path/to/extracted-game \
    --output build/MyGame --id my-game --name "My Game" \
    --author "Game Author" --version "1.0.0" --icon icon.png
```

The output directory must not already exist. `--icon` is optional. The
packager leaves the original game untouched, checks its files and reports
potential compatibility problems. The output includes a self-contained
`.nro`, package metadata, collected license notices and a hardware test plan.
See [docs/BUILD.md](docs/BUILD.md) for the details.

## Run it on your Switch

Copy the `.nro` to `sdmc:/switch/` and launch it in **application mode**
using title override or a forwarder.

Saves are stored in `sdmc:/switch/renpy/data/<id>/`, using the ID you chose
when packaging. Keep that ID the same when updating a game. The main log is
`sdmc:/renpy_switch.log`.

See [docs/HARDWARE_TEST.md](docs/HARDWARE_TEST.md) for testing and crash
reporting instructions.

## Documentation

- [Build guide](docs/BUILD.md): setup, build stages and packaging.
- [Hardware testing](docs/HARDWARE_TEST.md): installation, smoke tests and logs.
- [Project audit](docs/AUDIT.md): technical notes, changes and known limitations.

## Licensing

Ren'Py, CPython, FFmpeg and the other dependencies have their own licenses.

THIS REPOSITORY HAS NO LICENSE GRANT FOR ITS OWN PORT FILES AND IT IS NOT RESPONSIBLE FOR COPYRIGHT INFRINGEMENT.

## Original README

The [upstream README](https://github.com/SnepOMatic/RenPy-Switch/blob/ed82ccfaf0784e17216153d1ce2e678074bc6e13/README.md), quoted in full:

```text
## Description

Ren'Py 8.5.7 for Switch.
Initially based on https://github.com/greg6821-debug/RenpySwitch

Requires https://github.com/SnepOMatic/PyNX_python3_horizonOS_switch

Works with https://github.com/SnepOMatic/engine-picker

## Note!

This is a personal project and therefore carries no warrantee of
 - it working for you.
 - being based on any best practices.
 - being attributed correctly.
```
