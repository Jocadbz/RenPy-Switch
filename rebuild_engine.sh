#!/usr/bin/env bash
#
# Recompile the Ren'Py engine after a Cython (.pyx) change: re-cythonize (only
# changed files), recompile/relink the ELF, then repack lib.zip + the NRO.
# Use this for renpy/**/*.pyx edits. (For pure-.py edits, repack_lib.sh is enough;
# for libpython changes, rebuild_libpython.sh.)
#
#   bash rebuild_engine.sh
#
set -e
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
JOBS="${JOBS:-$(nproc)}"

echo "==> [1/3] Cythonizing changed .pyx -> renpy-source/tmp/gen3-static/*.c"
( cd renpy-source && RENPY_DEPS_INSTALL=/usr/lib/x86_64-linux-gnu:/usr:/usr/local \
    RENPY_STATIC=1 RENPY_NO_ASSIMP=1 python3.13 setup.py build_ext --inplace -j"$JOBS" || true )

echo "==> [2/3] Gathering generated C + syncing hand-written sources"
cp renpy-source/tmp/gen3-static/*.c source/module/ 2>/dev/null || true
cp sources/main.c          source/main.c
cp sources/game_picker.c   source/game_picker.c
cp sources/game_picker.h   source/game_picker.h
cp sources/switch_stubs.c  source/module/switch_stubs.c

echo "    relinking ELF (-j$JOBS)"
rm -f build/renpy-switch.elf
# Force the hand-written sources to RECOMPILE, not just relink. Over the mounted
# drive, make's mtime comparison can wrongly decide main.o is up-to-date and relink
# stale object code (symptom: source edits don't take effect on device). Deleting the
# objects guarantees a fresh compile.
find build -name 'main.o' -delete 2>/dev/null || true
find build -name 'switch_stubs.o' -delete 2>/dev/null || true
( cd build && make -j"$JOBS" 2>&1 | tee /tmp/renpy_link.log | tail -8 )
grep -oE "undefined reference to .*" /tmp/renpy_link.log | sort -u || true
if [ ! -f build/renpy-switch.elf ]; then
    echo "!! ELF not produced — see errors above. Aborting."; exit 1
fi

echo "==> [3/3] Repacking lib.zip + NRO"
bash repack_lib.sh

echo "==> engine rebuilt."
