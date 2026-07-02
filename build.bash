set -e

export DEVKITPRO=/opt/devkitpro

# Wipe the derived source/header trees before repopulating. A previous build that
# was interrupted (OOM, Ctrl-C, WSL shutdown) can leave correctly-SIZED but
# zero-filled files here — unflushed writes on the NTFS-backed drive. rsync's
# default size+mtime quick-check then sees a same-size file already present and
# SKIPS re-copying it, so the zeroed header (e.g. pyfreetype.h, renpygl.h) silently
# poisons every subsequent build (FT_ERRORS_H / GLuint undefined, etc.). Starting
# from empty guarantees the copies below are always real.
rm -rf source/module include/module
mkdir -p source/module include/module

# --- Cythonize Ren'Py 8.5.3 via the root setup.py -> renpy-source/tmp/gen3-static/*.c ---
# build_ext may fail to LINK against host libs, but it still generates the .c we need.
echo "Cythonizing Ren'Py 8.5.3..."
pushd renpy-source
RENPY_DEPS_INSTALL=/usr/lib/x86_64-linux-gnu:/usr:/usr/local RENPY_STATIC=1 RENPY_NO_ASSIMP=1 python3.13 setup.py build_ext --inplace -j"$(nproc)" || true
popd



GEN=renpy-source/tmp/gen3-static

# --- Gather C sources into a flat source/module/ (CMake globs these) ---
cp "$GEN"/*.c source/module/                          # cython-generated modules
cp renpy-source/src/*.c source/module/                # hand-written C (core, ffmedia, ftsupport, ...)
cp renpy-source/src/*.cc source/module/ 2>/dev/null || true   # assimpio.cc (C++)
cp renpy-source/src/pygame/*.c source/module/         # alphablit, write_jpeg/png, rotozoom, gfxprimitives
cp renpy-source/src/tinyfiledialogs/*.c source/module/
cp renpy-source/src/libhydrogen/hydrogen.c source/module/
rm -f source/module/live2dcsm.c                       # Live2D/Cubism: not built (no CUBISM SDK)
rm -f source/module/assimpio.cc source/module/renpy.gl2.assimp.c   # assimp 3D model loader: no libassimp on Switch
rm -f source/module/tinyfiledialogs.c source/module/renpy.tfd.c    # native file dialogs (termios): not on Switch
rm -f source/module/hydrogen.c     # libhydrogen built via renpy.encryption (unity build) — avoid duplicate symbols

# --- Gather headers into a flat include/module/ ---
cp "$GEN"/*.h include/module/ 2>/dev/null || true
rsync -am --include='*/' --include='*.h' --exclude='*' renpy-source/src/ include/module/
rsync -am --include='*/' --include='*.h' --exclude='*' renpy-source/renpy/ include/module/
find include/module -mindepth 2 -name '*.h' -exec mv -n -t include/module {} + 2>/dev/null || true
cp -r renpy-source/src/libhydrogen include/module/libhydrogen
find include/module -type d -empty -delete

cp sources/main.c source/main.c
cp sources/video_player.c source/video_player.c
cp sources/video_player.h source/video_player.h
cp sources/switch_stubs.c source/module/switch_stubs.c   # ENOSYS stubs for libc funcs newlib lacks

echo "== source/module contents =="
ls source/module

source /opt/devkitpro/switchvars.sh

rm -rf build
mkdir build
pushd build
cmake ..
# A full parallel build of the heavy C++/Cython units can exhaust RAM under high -j;
# the killed compiler leaves no .nso and no "error:" line. If the parallel build
# fails, retry serially (low memory) to finish the remaining objects + link.
make -j"$(nproc)" || { echo "== parallel make failed (likely OOM) — retrying serially =="; make; } || true
echo "===== PyInit symbols ====="
nm CMakeFiles/renpy-switch.dir/source/module/*.o | grep PyInit || true
echo "===== PyInit symbols ====="
popd

mkdir -p ./raw/switch/exefs

#mkdir -p ./raw/switch/exefs2
#cp -r /usr/local/lib/python3.9/dist-packages ./raw/switch/exefs2

mv ./build/renpy-switch.nso ./raw/switch/exefs/main

# --- Stage the Python tree + game, then compile .rpy -> .rpyc with the desktop SDK ---
# GAME_DIR: a Ren'Py project's game/ dir to embed (default: the bundled the_question demo).
GAME_DIR="${GAME_DIR:-renpy-source/the_question/game}"

# SKIP_GAME=1 -> build only the reusable engine (build/renpy-switch.elf + the romfs
# engine pieces renpy/, renpy.py, lib.zip) without embedding a playable game or
# producing the the_question.nro demo. Wrap real games afterwards with package_game.sh.
# NOTE: the demo game is still COMPILED internally below even when SKIP_GAME=1 —
# that same compile generates the renpy/common .rpyc that every package_game.sh output
# reuses, and the device romfs is read-only so common cannot be compiled there. Only
# the shipping of the game (into the romfs) and the demo NRO are skipped.
SKIP_GAME="${SKIP_GAME:-0}"

rm -rf renpy_clear
mkdir -p renpy_clear/game
cp ./renpy_sdk/*/renpy.sh ./renpy_clear/renpy.sh
cp -r ./renpy_sdk/*/lib ./renpy_clear/lib
cp -r ./renpy-source/renpy ./renpy_clear/renpy
cp ./renpy-source/renpy.py ./renpy_clear/renpy.py
cp -r "$GAME_DIR"/. ./renpy_clear/game/
rm -rf ./renpy_clear/lib/*mac*

pushd renpy_clear
./renpy.sh --compile . compile || true
find ./renpy/ -regex ".*\.\(pxd\|pyx\|rpym\|pxi\)" -delete
popd



# --- Assemble romfs/Contents (clean first so stale files don't linger) ---
rm -rf ./raw/switch/romfs/Contents/renpy ./raw/switch/romfs/Contents/game ./raw/switch/romfs/Contents/renpy.py
mkdir -p ./raw/switch/romfs/Contents/renpy
cp -r ./renpy_clear/renpy/common ./raw/switch/romfs/Contents/renpy/
# sdk-fonts (SourceHanSansLite CJK, Vazirmatn RTL) live outside renpy/common but are
# needed at runtime (e.g. the preferences screen). Fold them into common (on the
# font searchpath) so load_face() can find them.
cp ./renpy-source/sdk-fonts/*.ttf ./raw/switch/romfs/Contents/renpy/common/ 2>/dev/null || true
cp ./renpy_clear/renpy.py ./raw/switch/romfs/Contents/
if [ "$SKIP_GAME" = 1 ]; then
    echo "SKIP_GAME=1: not embedding a game into the romfs (engine-only build)."
else
    cp -r ./renpy_clear/game ./raw/switch/romfs/Contents/game
fi
# Optional boot splash shown by main.c during init — drop art at sources/splash.png.
[ -f sources/splash.png ] && cp sources/splash.png ./raw/switch/romfs/Contents/splash.png || true

# --- lib.zip = FULL CPython 3.13 stdlib + Ren'Py python package ---
# PyNX's python.zip ships only a subset of the stdlib (it omits packages like re/),
# so we package the complete Lib tree that libpython3.13.a was built from instead.
FULLLIB="${FULLLIB:-../PyNX_python3.13.0_horizonOS_switch/python_build/Python-3.13.14/Lib}"
rm -rf ./raw/lib
mkdir -p ./raw/lib
rsync -a \
    --exclude 'test' --exclude 'tests' --exclude '__pycache__' \
    --exclude 'idlelib' --exclude 'tkinter' --exclude 'turtledemo' \
    --exclude 'lib2to3' --exclude 'ensurepip' --exclude 'site-packages' \
    "$FULLLIB"/ ./raw/lib/                            # full 3.13 stdlib (.py)
cp -r ./renpy_clear/renpy ./raw/lib/renpy            # Ren'Py python package
rm -rf ./raw/lib/renpy/common                         # common ships unpacked in Contents/renpy
# Third-party pure-Python deps Ren'Py needs (e.g. ecdsa for renpy.savetoken). The SDK
# ships these only as 3.12 .pyc, which our 3.13 interpreter can't load, so fetch the
# SOURCE with pip (--no-compile keeps it .py; pulls in six etc.).
python3 -m pip install --target ./raw/lib --no-compile --upgrade ecdsa || \
    pip3 install --target ./raw/lib --no-compile --upgrade ecdsa
rm -rf ./raw/lib/*.dist-info ./raw/lib/bin             # strip pip metadata/scripts
# Switch: _socket has no gethostname() (no hostname concept), so games doing
# `from socket import gethostname` crash. Append a guarded stub to socket.py.
if [ -f ./raw/lib/socket.py ] && ! grep -q RENPY_SWITCH_GETHOSTNAME ./raw/lib/socket.py; then
cat >> ./raw/lib/socket.py <<'PYEOF'

# RENPY_SWITCH_GETHOSTNAME: Horizon _socket omits gethostname(); provide a stub.
try:
    gethostname
except NameError:
    def gethostname():
        return "switch"
PYEOF
fi
# Switch runs Python 3.13, where random.randrange() rejects non-integer (float) args;
# many older Ren'Py games pass floats. Append a leniency shim to random.py (idempotent).
if [ -f ./raw/lib/random.py ] && ! grep -q RENPY_SWITCH_RANDRANGE ./raw/lib/random.py; then
cat >> ./raw/lib/random.py <<'PYEOF'

# RENPY_SWITCH_RANDRANGE: coerce non-integer randrange() args to int (pre-3.11 behaviour).
try:
    _rr_orig_switch = Random.randrange
    def _rr_switch(self, start, stop=None, step=_ONE, _orig=_rr_orig_switch):
        if not isinstance(start, int):
            start = int(start)
        if stop is not None and not isinstance(stop, int):
            stop = int(stop)
        if step is not _ONE and not isinstance(step, int):
            step = int(step)
        return _orig(self, start, stop, step)
    Random.randrange = _rr_switch
    randrange = _inst.randrange
    randint = _inst.randint
except Exception:
    pass
PYEOF
fi
# Pre-compile .py -> .pyc (sourceless) so the device loads bytecode without re-parsing
# every module each boot. Any 3.13.x emits magic-compatible .pyc for the device 3.13.14.
# Delete a .py only once its .pyc exists (failed compiles keep their source).
python3.13 -m compileall -b -q -j0 ./raw/lib || true
find ./raw/lib -name '*.py' | while read -r f; do [ -f "${f%.py}.pyc" ] && rm -f "$f"; done
# STORED (no compression) so zipimport needs no zlib during interpreter init.
( cd ./raw/lib && 7z a -tzip -mx0 ../switch/romfs/Contents/lib.zip ./* >/dev/null )
rm -rf ./raw/lib

echo "== packaged homebrew layout (raw/switch) =="
find ./raw/switch -maxdepth 3 | sort

# --- Bundle a homebrew NRO (embeds romfs) for hbmenu / Ryujinx ---
if [ "$SKIP_GAME" = 1 ]; then
    # Loader NRO: engine + runtime, NO embedded game. It runs whatever archive is
    # named in sdmc:/renpy/game.txt (see the selection bridge in main.c), reading the
    # game's files on the fly. build/renpy-switch.elf + the romfs engine pieces are
    # reused; only game/ is absent.
    echo "SKIP_GAME=1: building loader NRO (no embedded game)."
    LOADER_TITLE="${TITLE:-RenPy Loader}"
    LOADER_AUTHOR="${AUTHOR:-RenpySwitch}"
    LOADER_ICON="${ICON:-../PyNX_python3.13.0_horizonOS_switch/icon.jpg}"
    $DEVKITPRO/tools/bin/nacptool --create "$LOADER_TITLE" "$LOADER_AUTHOR" "8.5.3" ./raw/app.nacp
    LOADER_NRO_ICON=""; [ -f "$LOADER_ICON" ] && LOADER_NRO_ICON="--icon=$LOADER_ICON"
    $DEVKITPRO/tools/bin/elf2nro ./build/renpy-switch.elf ./raw/loader.nro \
        --nacp=./raw/app.nacp $LOADER_NRO_ICON --romfsdir=./raw/switch/romfs
    echo "== built ./raw/loader.nro =="
    ls -la ./raw/loader.nro
else
    TITLE="${TITLE:-The Question}"
    AUTHOR="${AUTHOR:-RenpySwitch}"
    ICON="${ICON:-../PyNX_python3.13.0_horizonOS_switch/icon.jpg}"
    $DEVKITPRO/tools/bin/nacptool --create "$TITLE" "$AUTHOR" "8.5.3" ./raw/app.nacp
    NRO_ICON=""; [ -f "$ICON" ] && NRO_ICON="--icon=$ICON"
    $DEVKITPRO/tools/bin/elf2nro ./build/renpy-switch.elf ./raw/the_question.nro \
        --nacp=./raw/app.nacp $NRO_ICON --romfsdir=./raw/switch/romfs
    echo "== built ./raw/the_question.nro =="
    ls -la ./raw/the_question.nro
fi
