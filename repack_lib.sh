#!/usr/bin/env bash
#
# Repackage lib.zip (full stdlib + patched renpy package + vendored deps) and
# rebuild the NRO. Use after editing anything under renpy-source/renpy or the
# stdlib. Does NOT recompile the ELF (run a normal `make` for C changes).
#
#   bash repack_lib.sh
#
set -e
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

FULLLIB=../PyNX_python3.13.0_horizonOS_switch/python_build/Python-3.13.14/Lib

rm -rf raw/lib && mkdir -p raw/lib

# full CPython 3.13 stdlib
rsync -a \
    --exclude 'test' --exclude 'tests' --exclude '__pycache__' \
    --exclude 'idlelib' --exclude 'tkinter' --exclude 'turtledemo' \
    --exclude 'lib2to3' --exclude 'ensurepip' --exclude 'site-packages' \
    "$FULLLIB"/ raw/lib/

# Ren'Py engine package from the patched source (minus cython sources + common)
rsync -a \
    --exclude 'common' --exclude '__pycache__' \
    --exclude '*.pyx' --exclude '*.pxd' --exclude '*.pxi' --exclude '*.rpym' \
    renpy-source/renpy/ raw/lib/renpy/

# vendored pure-Python deps (ecdsa etc.) as SOURCE — SDK ships only 3.12 .pyc.
# Fetch once into switch_deps/ (needs network), then reuse it on every repack.
DEPS=switch_deps
if [ ! -d "$DEPS/ecdsa" ]; then
    echo "Fetching vendored deps (ecdsa) into $DEPS/ (one-time, needs network)..."
    rm -rf "$DEPS"; mkdir -p "$DEPS"
    python3 -m pip install --target "$DEPS" --no-compile --upgrade ecdsa \
        || pip3 install --target "$DEPS" --no-compile --upgrade ecdsa
    rm -rf "$DEPS"/*.dist-info "$DEPS"/bin
fi
cp -r "$DEPS"/. raw/lib/

# Switch: the platform _socket C module has no gethostname() (Horizon has no hostname
# concept), so games doing `from socket import gethostname` / socket.gethostname()
# crash on import. Append a guarded stub to the bundled socket.py (idempotent).
if [ -f raw/lib/socket.py ] && ! grep -q RENPY_SWITCH_GETHOSTNAME raw/lib/socket.py; then
cat >> raw/lib/socket.py <<'PYEOF'

# RENPY_SWITCH_GETHOSTNAME: Horizon _socket omits gethostname(); provide a stub.
try:
    gethostname
except NameError:
    def gethostname():
        return "switch"
PYEOF
fi

# Switch runs Python 3.13, where random.randrange() rejects non-integer (float) args;
# many older Ren'Py games pass floats (e.g. float render sizes). Append a leniency
# shim to random.py that int()-coerces the arguments (idempotent).
if [ -f raw/lib/random.py ] && ! grep -q RENPY_SWITCH_RANDRANGE raw/lib/random.py; then
cat >> raw/lib/random.py <<'PYEOF'

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

# Pre-compile .py -> .pyc (sourceless) so the on-device interpreter loads bytecode
# directly instead of re-parsing every module each boot. Any CPython 3.13.x emits
# magic-compatible bytecode for the device's 3.13.14 (magic is frozen per minor).
# Only delete a .py once its .pyc exists, so a module that fails to compile keeps
# its source as a fallback rather than disappearing.
echo "Pre-compiling lib to .pyc with $(python3.13 -V 2>&1)..."
python3.13 -m compileall -b -q -j0 raw/lib || true
find raw/lib -name '*.py' | while read -r f; do [ -f "${f%.py}.pyc" ] && rm -f "$f"; done
echo "  .py left (failed/none): $(find raw/lib -name '*.py' | wc -l);  .pyc: $(find raw/lib -name '*.pyc' | wc -l)"

# STORED zip (no compression) so zipimport needs no zlib at interpreter init
rm -f raw/switch/romfs/Contents/lib.zip
( cd raw/lib && 7z a -tzip -mx0 ../switch/romfs/Contents/lib.zip ./* >/dev/null )
rm -rf raw/lib

# Keep the deployed launcher in sync with the patched source. path_to_saves (the
# Switch per-game save path) and other launcher logic live in renpy.py, which is NOT
# part of lib.zip — build.bash is otherwise the only place that copies it.
cp renpy-source/renpy.py raw/switch/romfs/Contents/renpy.py

$DEVKITPRO/tools/bin/elf2nro build/renpy-switch.elf the_question.nro \
    --nacp=the_question.nacp \
    --icon=../PyNX_python3.13.0_horizonOS_switch/icon.jpg \
    --romfsdir=raw/switch/romfs

echo "== repacked the_question.nro =="
