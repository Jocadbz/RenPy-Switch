#!/usr/bin/env bash
#
# package_game.sh — wrap a Ren'Py game/ folder into a Switch homebrew .nro,
# REUSING the already-compiled engine (build/renpy-switch.elf). This does NOT
# rebuild Python/Cython/the engine — it only swaps the embedded game, the title
# (NACP), and the icon, then re-runs elf2nro. Seconds, not the full build.
#
# Prerequisite: run a full `sudo bash build.bash` ONCE first, which produces
#   build/renpy-switch.elf  and  raw/switch/romfs/Contents/{renpy,renpy.py,lib.zip}
# Those engine/runtime pieces are game-independent and are reused as-is here.
#
# ---------------------------------------------------------------------------
# Usage:
#   bash package_game.sh <game-dir> [options]
#
#   <game-dir>   Path to a Ren'Py project's game/ folder (or a project dir that
#                CONTAINS a game/ subfolder — either works).
#
# Options:
#   --name "Title"     Title shown on the Switch home menu. Default (in order):
#                      "name" from game.json in the project folder (the parent of
#                      game/), else config.name scraped from the .rpy files, else the
#                      project folder name, else "Ren'Py Game".
#   --version "1.0"    Version string. Default (in order): "version" from game.json in
#                      the project folder, else scraped config.version, else "1.0".
#   --icon  PATH       (see below) — a splash.png in the project folder is also used
#                      as the boot splash automatically if present.
#   --author "Name"    Publisher/author. Default: "RenpySwitch".
#   --icon  PATH       Icon source image (any size/format ImageMagick or PIL reads).
#                      Default (in order): icon.png in the project folder (alongside
#                      game.json / splash.png), else <game>/gui/window_icon.png, else
#                      the engine default icon. Auto-resized to a 256x256 JPEG.
#   --out   PATH       Output .nro path. Default: ./<sanitized-title>.nro
#   --no-compile       Skip the SDK .rpy->.rpyc compile (embed the game as-is;
#                      use this if the game already ships compiled .rpyc).
#   -h | --help        Show this help.
#
# Examples:
#   bash package_game.sh ~/mygame/game
#   bash package_game.sh ~/mygame --name "My Game" --version 1.2 --icon ~/art/logo.png
#   bash package_game.sh ~/dist/MyGame-1.0-pc/game --out ~/MyGame.nro
# ---------------------------------------------------------------------------
set -e
cd "$(dirname "$0")"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
NACPTOOL="$DEVKITPRO/tools/bin/nacptool"
ELF2NRO="$DEVKITPRO/tools/bin/elf2nro"
DEFAULT_ICON="../PyNX_python3.13.0_horizonOS_switch/icon.jpg"
ELF="build/renpy-switch.elf"
ROMFS="raw/switch/romfs"

die() { echo "ERROR: $*" >&2; exit 1; }
usage() { sed -n '2,/^set -e$/p' "$0" | sed 's/^# \{0,1\}//; /^set -e$/d'; exit "${1:-0}"; }

# ---- parse args ----
GAME_IN=""; NAME_OVERRIDE=""; VER_OVERRIDE=""; AUTHOR="RenpySwitch"
ICON_OVERRIDE=""; OUT=""; DO_COMPILE=1
while [ $# -gt 0 ]; do
    case "$1" in
        --name)     NAME_OVERRIDE="$2"; shift 2;;
        --version)  VER_OVERRIDE="$2";  shift 2;;
        --author)   AUTHOR="$2";        shift 2;;
        --icon)     ICON_OVERRIDE="$2"; shift 2;;
        --out)      OUT="$2";           shift 2;;
        --no-compile) DO_COMPILE=0;     shift;;
        -h|--help)  usage 0;;
        -*)         die "unknown option: $1  (try --help)";;
        *)          [ -z "$GAME_IN" ] && GAME_IN="$1" || die "unexpected extra arg: $1"; shift;;
    esac
done
[ -n "$GAME_IN" ] || usage 1

# ---- resolve the game/ dir (accept either a game/ dir or a project dir) ----
if   [ -d "$GAME_IN/game" ]; then GAME_DIR="$GAME_IN/game"
elif [ -d "$GAME_IN" ];      then GAME_DIR="$GAME_IN"
else die "'$GAME_IN' is not a directory"; fi
GAME_ABS="$(cd "$GAME_DIR" && pwd)"
PROJECT_ABS="$(dirname "$GAME_ABS")"

# ---- prerequisites (the reused engine/runtime from build.bash) ----
[ -f "$ELF" ]               || die "$ELF not found — run 'sudo bash build.bash' once first."
[ -f "$ROMFS/Contents/lib.zip" ] || die "$ROMFS/Contents/lib.zip not found — run build.bash once first."
[ -d "$ROMFS/Contents/renpy" ]   || die "$ROMFS/Contents/renpy not found — run build.bash once first."
[ -f "$ROMFS/Contents/renpy.py" ] || die "$ROMFS/Contents/renpy.py not found — run build.bash once first."

# ---- scratch staging dir (assembled fresh each run, erased on exit) ----
# Everything that goes into the .nro is assembled here rather than in-place in
# $ROMFS, so per-game assets (game/, splash.png, icon, nacp) from a previously
# packaged game can never leak into this one. $ROMFS stays the pristine build.bash
# output. The trap wipes the scratch dir on any exit (success, error, or Ctrl-C).
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/pkg_game.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
STAGE_ROMFS="$STAGE/romfs"

# ---- resolve name/version ----
# Preferred source: game.json in the PROJECT folder (the parent of game/), giving
# name + version. Falls back to scraping the project's .rpy, then the folder name.
# Explicit --name/--version still win.
GAME_JSON="$PROJECT_ABS/game.json"

# json_get <field: name|version> — reads game.json, tolerant of a few key spellings.
json_get() {
    [ -f "$GAME_JSON" ] || return 0
    python3 - "$GAME_JSON" "$1" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception:
    raise SystemExit
if not isinstance(d, dict):
    raise SystemExit
alt = {"name": ("name", "title", "game_name"),
       "version": ("version", "ver", "game_version")}[sys.argv[2]]
for k in alt:
    v = d.get(k)
    if isinstance(v, (str, int, float)) and str(v).strip():
        print(str(v).strip())
        break
PY
}

# scrape <dotted key, e.g. config.name> from .rpy:  define config.name = _("X") | "X"
scrape() {
    local k="${1//./\\.}"
    grep -rhE "define[[:space:]]+${k}[[:space:]]*=" "$GAME_ABS" --include='*.rpy' 2>/dev/null \
        | head -1 \
        | sed -E "s/.*=[[:space:]]*_?\(?[[:space:]]*\"([^\"]*)\".*/\1/"
}

[ -f "$GAME_JSON" ] && echo "game.json: found ($GAME_JSON)"

TITLE="$NAME_OVERRIDE"
[ -z "$TITLE" ] && TITLE="$(json_get name)"
[ -z "$TITLE" ] && TITLE="$(scrape config.name)"
[ -z "$TITLE" ] && TITLE="$(basename "$PROJECT_ABS")"
[ -z "$TITLE" ] && TITLE="Ren'Py Game"
VERSION="$VER_OVERRIDE"
[ -z "$VERSION" ] && VERSION="$(json_get version)"
[ -z "$VERSION" ] && VERSION="$(scrape config.version)"
[ -z "$VERSION" ] && VERSION="1.0"

# ---- choose + build the icon (256x256 JPEG, required by elf2nro) ----
make_icon() {  # $1=src  $2=dst(.jpg) ; returns 0 on success, 1 if nothing worked
    local src="$1" dst="$2" py
    # 1) ImageMagick (v7 'magick' or v6 'convert')
    if command -v magick >/dev/null 2>&1; then
        magick "$src" -background black -alpha remove -alpha off \
            -resize 256x256^ -gravity center -extent 256x256 "$dst" 2>/dev/null && return 0
    fi
    if command -v convert >/dev/null 2>&1; then
        convert "$src" -background black -alpha remove -alpha off \
            -resize 256x256^ -gravity center -extent 256x256 "$dst" 2>/dev/null && return 0
    fi
    # 2) Python + Pillow, under whichever interpreter has it
    for py in python3 python3.13 python; do
        command -v "$py" >/dev/null 2>&1 || continue
        "$py" -c 'import PIL' >/dev/null 2>&1 || continue
        "$py" - "$src" "$dst" <<'PY' && return 0
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGB")
w, h = im.size
s = max(256 / w, 256 / h)              # cover-fit, then centre-crop to a square
im = im.resize((round(w * s), round(h * s)))
w, h = im.size
l, t = (w - 256) // 2, (h - 256) // 2
im.crop((l, t, l + 256, t + 256)).save(dst, "JPEG", quality=90)
PY
    done
    # 3) ffmpeg (cover-fit + centre-crop)
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -y -loglevel error -i "$src" \
            -vf "scale=256:256:force_original_aspect_ratio=increase,crop=256:256" \
            "$dst" </dev/null 2>/dev/null && return 0
    fi
    # 4) Last resort: source is already a JPEG -> embed as-is (not resized)
    case "$(printf '%s' "$src" | tr '[:upper:]' '[:lower:]')" in
        *.jpg|*.jpeg) cp -f "$src" "$dst" && return 0 ;;
    esac
    return 1
}

ICON_SRC="$ICON_OVERRIDE"
# Preferred default: icon.png in the PROJECT folder (alongside game.json / splash.png).
[ -z "$ICON_SRC" ] && [ -f "$PROJECT_ABS/icon.png" ] && ICON_SRC="$PROJECT_ABS/icon.png"
# Fallback: the game's built-in window icon.
[ -z "$ICON_SRC" ] && [ -f "$GAME_ABS/gui/window_icon.png" ] && ICON_SRC="$GAME_ABS/gui/window_icon.png"
NRO_ICON_ARG=""
if [ -n "$ICON_SRC" ]; then
    [ -f "$ICON_SRC" ] || die "icon not found: $ICON_SRC"
    if make_icon "$ICON_SRC" "$STAGE/icon.jpg"; then
        NRO_ICON_ARG="--icon=$STAGE/icon.jpg"
        echo "icon  : $ICON_SRC -> 256x256 jpg"
    else
        echo "WARN  : couldn't make a 256x256 JPEG from '$ICON_SRC'."
        echo "        Install one converter, then re-run, e.g.:"
        echo "          sudo apt-get install -y imagemagick    # provides 'convert'"
        echo "          # or:  pip install Pillow               # for python3/python3.13"
        echo "          # or:  sudo apt-get install -y ffmpeg"
        echo "        (Or pass a ready 256x256 .jpg via --icon.) Using the default icon for now."
    fi
fi
if [ -z "$NRO_ICON_ARG" ] && [ -f "$DEFAULT_ICON" ]; then
    NRO_ICON_ARG="--icon=$DEFAULT_ICON"
    echo "icon  : (default) $DEFAULT_ICON"
fi

# ---- output path ----
if [ -z "$OUT" ]; then
    slug="$(printf '%s' "$TITLE" | tr -cd '[:alnum:]._ -' | tr ' ' '_')"
    [ -z "$slug" ] && slug="renpy_game"
    OUT="./$slug.nro"
fi

echo "title : $TITLE"
echo "ver   : $VERSION"
echo "author: $AUTHOR"
echo "game  : $GAME_ABS"
echo "out   : $OUT"
echo

# ---- (optional) compile .rpy -> .rpyc with the desktop SDK ----
# Mirrors build.bash: stage the SDK lib + engine python package + game, then
# `renpy.sh --compile`. renpy_clear is reused across runs; only game/ is refreshed.
if [ "$DO_COMPILE" = 1 ]; then
    if [ ! -x renpy_clear/renpy.sh ] || [ ! -d renpy_clear/renpy ] || [ ! -d renpy_clear/lib ]; then
        echo "Staging renpy_clear (SDK + engine) for compilation..."
        rm -rf renpy_clear; mkdir -p renpy_clear
        cp  ./renpy_sdk/*/renpy.sh renpy_clear/renpy.sh
        cp -r ./renpy_sdk/*/lib    renpy_clear/lib
        cp -r ./renpy-source/renpy renpy_clear/renpy
        cp  ./renpy-source/renpy.py renpy_clear/renpy.py
        rm -rf renpy_clear/lib/*mac*
    fi
    echo "Compiling game scripts (.rpy -> .rpyc)..."
    rm -rf renpy_clear/game; mkdir -p renpy_clear/game
    cp -r "$GAME_ABS"/. renpy_clear/game/
    ( cd renpy_clear && ./renpy.sh --compile . compile || true )
    STAGED_GAME="renpy_clear/game"
else
    STAGED_GAME="$GAME_ABS"
fi

# ---- assemble a fresh romfs in the scratch dir ----
# Reused, game-independent engine/runtime pieces are copied from the pristine
# $ROMFS (build.bash output); per-game pieces (game/, splash) are layered on top
# here, so nothing from a previously packaged game can linger in this .nro.
echo "Assembling romfs in scratch dir..."
mkdir -p "$STAGE_ROMFS/Contents"
cp -r "$ROMFS/Contents/renpy"    "$STAGE_ROMFS/Contents/renpy"
cp    "$ROMFS/Contents/renpy.py" "$STAGE_ROMFS/Contents/renpy.py"
cp    "$ROMFS/Contents/lib.zip"  "$STAGE_ROMFS/Contents/lib.zip"
cp -r "$STAGED_GAME"             "$STAGE_ROMFS/Contents/game"

# Boot splash (read by the C splash module as Contents/splash.png). Priority: a
# splash.png in the project folder (parent of game/), else the build default at
# sources/splash.png. Absent both, the game simply ships without a boot splash.
if [ -f "$PROJECT_ABS/splash.png" ]; then
    cp -f "$PROJECT_ABS/splash.png" "$STAGE_ROMFS/Contents/splash.png"
    echo "splash: using $PROJECT_ABS/splash.png"
elif [ -f sources/splash.png ]; then
    cp -f sources/splash.png "$STAGE_ROMFS/Contents/splash.png"
    echo "splash: using default sources/splash.png"
fi

# ---- NACP (title/author/version) + NRO ----
[ -x "$NACPTOOL" ] || die "$NACPTOOL missing — is devkitPro installed at $DEVKITPRO?"
[ -x "$ELF2NRO" ]  || die "$ELF2NRO missing — is devkitPro installed at $DEVKITPRO?"
"$NACPTOOL" --create "$TITLE" "$AUTHOR" "$VERSION" "$STAGE/app.nacp"
"$ELF2NRO" "$ELF" "$OUT" --nacp="$STAGE/app.nacp" $NRO_ICON_ARG --romfsdir="$STAGE_ROMFS"

echo
echo "== built: $OUT =="
ls -la "$OUT"
