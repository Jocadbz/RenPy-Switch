#!/usr/bin/env bash
# make_renpy_patch.sh — regenerate renpy.patch as the diff between pristine
# Ren'Py 8.5.3 source and the hand-ported renpy-source tree (our Switch
# conditionals). tools/build.py applies the result with `patch -p1` to its
# freshly downloaded, SHA-256-verified source.
#
#     bash make_renpy_patch.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORTED="renpy-source"
TB="$HERE/renpy-8.5.3-source.tar.bz2"
cd "$HERE"

[ -d "$PORTED" ] || { echo "ERROR: $PORTED not found next to this script"; exit 1; }

echo "==> extracting pristine 8.5.3 source"
rm -rf .renpy-pristine && mkdir .renpy-pristine
if [ -f "$TB" ]; then
  tar -xf "$TB" -C .renpy-pristine --strip-components=1
else
  echo "   ($TB not present; downloading)"
  curl -L -o /tmp/renpy-8.5.3-source.tar.bz2 https://www.renpy.org/dl/8.5.3/renpy-8.5.3-source.tar.bz2
  tar -xf /tmp/renpy-8.5.3-source.tar.bz2 -C .renpy-pristine --strip-components=1
fi

echo "==> diffing pristine -> hand-ported"
# diff exits 1 when differences exist (expected), so guard against set -e.
diff -Naur \
  -x '__pycache__' -x '*.pyc' -x '*.pyo' -x '.git' -x '*.orig' -x '*.rej' \
  -x 'tmp' -x 'build' -x '*.so' \
  .renpy-pristine "$PORTED" > renpy.patch || true

rm -rf .renpy-pristine
echo "==> wrote renpy.patch  ($(grep -c '^+++ ' renpy.patch) files, $(wc -l < renpy.patch) lines)"
echo ""
echo "Files in the patch (expect ~10 .py files):"
grep '^+++ ' renpy.patch | sed 's#^+++ [^/]*/##; s/\t.*//'
