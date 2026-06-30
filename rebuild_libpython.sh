#!/usr/bin/env bash
#
# Rebuild libpython3.13.a WITH the full CPython stdlib C extensions as static
# builtins (PyNX's core-only build omitted _struct, math, zlib, _random, ...),
# reinstall it, then relink the RenpySwitch ELF + repackage the NRO.
#
# Run from the RenpySwitch directory:  bash rebuild_libpython.sh
#
set -e
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITA64=${DEVKITA64:-$DEVKITPRO/devkitA64}
JOBS="${JOBS:-$(nproc)}"   # parallel build jobs; override with JOBS=N
# CPython's own Makefile invokes the cross-compiler by bare name (aarch64-none-elf-gcc),
# so the devkitA64 + tools bins must be on PATH (the devkitPro-rules Makefiles add this
# automatically, but CPython's does not).
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"

PYBUILD=../PyNX_python3.13.0_horizonOS_switch/python_build
PYDIR="$PYBUILD/Python-3.13.14"

echo "==> [1/5] Enabling full stdlib as STATIC builtins (Setup.local)"
cp "$PYDIR/Modules/Setup.stdlib" "$PYDIR/Modules/Setup.local"
sed -i 's/^\*shared\*/*static*/' "$PYDIR/Modules/Setup.local"
# pyexpat needs the bundled libexpat, which PyNX never built (no xmlparse.o); drop it
# and _elementtree (which depends on pyexpat). Ren'Py doesn't need XML to boot.
sed -i 's/^pyexpat /#pyexpat /'           "$PYDIR/Modules/Setup.local"
sed -i 's/^_elementtree /#_elementtree /' "$PYDIR/Modules/Setup.local"
# _decimal needs bundled libmpdec (never built); decimal.py falls back to _pydecimal.
sed -i 's/^_decimal /#_decimal /' "$PYDIR/Modules/Setup.local"
# _sha2 links a prebuilt libHacl_Hash_SHA2.a (never built). Compile the HACL source
# inline like _md5/_sha1/_sha3 do, so hashlib.sha256/sha512 work.
sed -i 's#^_sha2 .*#_sha2 sha2module.c -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_SHA2.c -D_BSD_SOURCE -D_DEFAULT_SOURCE#' "$PYDIR/Modules/Setup.local"

echo "==> [2/5] Regenerating config.c + rebuilding libpython3.13.a (parallel -j$JOBS)"
( cd "$PYDIR" \
    && rm -f libpython3.13.a Modules/config.c \
    && make -j"$JOBS" LIBRARY="libpython3.13.a" LDLIBRARY="libpython3.13.a" libpython3.13.a )

echo "==> new PyInit_* now in libpython3.13.a:"
nm "$PYDIR/libpython3.13.a" 2>/dev/null | grep -oE "T PyInit_[A-Za-z0-9_]+" | sed 's/T //' | sort -u | tr '\n' ' '; echo

echo "==> [3/5] Installing libpython into portlibs + PyNX outputs"
# /opt/devkitpro is root-owned, so this copy needs sudo.
sudo cp "$PYDIR/libpython3.13.a" "$DEVKITPRO/portlibs/switch/lib/libpython3.13.a"
cp "$PYDIR/libpython3.13.a" "$PYBUILD/libpython3.13.a"
[ -d "$PYBUILD/nxpy3.13.14/lib" ] && cp "$PYDIR/libpython3.13.a" "$PYBUILD/nxpy3.13.14/lib/libpython3.13.a" || true

echo "==> [4/5] Relinking RenpySwitch ELF (force relink against the new archive)"
# Sync the hand-written sources into the build tree first (build.bash does this).
cp sources/main.c source/main.c
cp sources/switch_stubs.c source/module/switch_stubs.c
rm -f build/renpy-switch.elf
( cd build && make -j"$JOBS" 2>&1 | tee /tmp/renpy_link.log | tail -8 )
echo "----- all undefined references (empty = link clean): -----"
grep -oE "undefined reference to .*" /tmp/renpy_link.log | sort -u || true
echo "-----------------------------------------------------------"

if [ ! -f build/renpy-switch.elf ]; then
    echo "!! ELF not produced — fix the errors above before packaging. Aborting."
    exit 1
fi

echo "==> [5/5] Repackaging the NRO"
$DEVKITPRO/tools/bin/elf2nro build/renpy-switch.elf the_question.nro \
    --nacp=the_question.nacp \
    --icon=../PyNX_python3.13.0_horizonOS_switch/icon.jpg \
    --romfsdir=raw/switch/romfs

echo "==> DONE: the_question.nro rebuilt with the full stdlib."
