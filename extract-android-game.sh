#!/usr/bin/env bash
#
# extract-xgame.sh
#
#   1. Checks an APK contains /assets/x-game/
#   2. Extracts that folder into a directory named after the APK
#        (mygame.apk  ->  ./mygame/)
#   3. Renames every file & folder to strip the leading "x-" prefix
#        (x-game -> game, x-foo.png -> foo.png, ...)
#   4. Copies game/gui/main_menu.* (any image format) to ./<name>/splash.png,
#      resized to 1920x1080.
#   5. Reads assets/x-android.json (the file just above x-game) and writes
#      ./<name>/game.json (parent dir, alongside game/) with { "name","version" }.
#
# Tolerates unzip "warning" exits (code 1), e.g. the harmless
# "mismatching local filename ... continuing with central filename version"
# message seen on archives that store a name twice (Windows/UTF-8 quirk).
#
# Usage:  ./extract-xgame.sh path/to/mygame.apk
#
set -euo pipefail

# ---- args ------------------------------------------------------------------
if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <file.apk>" >&2
  exit 1
fi

apk="$1"
src="assets/x-game/"

[[ -f "$apk" ]] || { echo "Error: '$apk' not found." >&2; exit 1; }
command -v unzip >/dev/null 2>&1 || {
  echo "Error: 'unzip' is required.  Install it with: sudo apt install unzip" >&2
  exit 1
}
command -v python3 >/dev/null 2>&1 || {
  echo "Error: 'python3' is required to parse JSON.  Install it with: sudo apt install python3" >&2
  exit 1
}

# unzip exit codes: 0 = ok, 1 = completed with warnings (fine), >=2 = real error.
die_on_unzip_error() {           # $1 = exit code, $2 = context
  if (( $1 > 1 )); then
    echo "Error: unzip failed while $2 (exit $1). Is '$apk' a valid APK/zip?" >&2
    exit 1
  fi
}

# Resize an image to exactly 1920x1080 and save as PNG. Prefers ImageMagick,
# falls back to Python Pillow. Returns non-zero (with guidance) if neither works.
resize_1080p() {                 # $1 = src image, $2 = dst .png
  local s="$1" d="$2"
  if command -v magick >/dev/null 2>&1; then
    magick "$s" -resize '1920x1080!' "$d"
  elif command -v convert >/dev/null 2>&1; then
    convert "$s" -resize '1920x1080!' "$d"
  elif python3 -c 'import PIL' >/dev/null 2>&1; then
    python3 - "$s" "$d" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1])
im = im.resize((1920, 1080))
im.save(sys.argv[2], "PNG")
PY
  else
    echo "Warning: need ImageMagick or Python Pillow to make splash.png; skipped." >&2
    echo "         Install one:  sudo apt install imagemagick   (or)   pip install pillow" >&2
    return 1
  fi
}

# ---- 1. verify the folder exists inside the APK ----------------------------
# Capture the listing first. Piping unzip straight into `grep -q` can make
# pipefail report a false failure, because grep closes the pipe as soon as it
# finds a match and unzip then dies with SIGPIPE on a large archive.
set +e
listing="$(unzip -Z1 "$apk" 2>/dev/null)"
rc=$?
set -e
die_on_unzip_error "$rc" "listing the archive"

if ! grep -q "^${src}" <<<"$listing"; then
  echo "Error: '$apk' does not contain /${src}" >&2
  exit 1
fi

# ---- 2. work out the output directory from the APK name --------------------
out="$(basename "$apk")"
out="${out%.*}"                       # strip the .apk extension

if [[ -e "$out" ]]; then
  echo "Error: '$out' already exists. Move/remove it or run elsewhere." >&2
  exit 1
fi

# ---- extract only assets/x-game/* -----------------------------------------
mkdir -p "$out"
set +e
unzip -qo "$apk" "${src}*" -d "$out"
rc=$?
set -e
die_on_unzip_error "$rc" "extracting"

# flatten:  out/assets/x-game/...  ->  out/x-game/...
mv "$out/assets/x-game" "$out/x-game"
rm -rf "$out/assets"

# ---- 3. strip "x-" prefix from every file & folder (deepest first) ---------
renamed=0
while IFS= read -r -d '' path; do
  dir="$(dirname "$path")"
  name="$(basename "$path")"
  dest="$dir/${name#x-}"
  if [[ -e "$dest" ]]; then
    echo "Warning: skipping '$path' (target '$dest' already exists)." >&2
    continue
  fi
  mv "$path" "$dest"
  renamed=$((renamed + 1))
done < <(find "$out" -depth -name 'x-*' -print0)

# ---- 4. make splash.png (in parent dir) from game/gui/main_menu.* ----------
# main_menu may be any image format (.png, .webp, .jpg, ...). Prefer common
# formats in order, then fall back to any image named main_menu under a gui/.
menu_src=""
for ext in png webp jpg jpeg bmp gif tif tiff; do
  if [[ -f "$out/game/gui/main_menu.$ext" ]]; then
    menu_src="$out/game/gui/main_menu.$ext"
    break
  fi
done
if [[ -z "$menu_src" ]]; then
  menu_src="$(find "$out/game" -type f -ipath '*/gui/main_menu.*' \
      \( -iname '*.png'  -o -iname '*.webp' -o -iname '*.jpg' -o -iname '*.jpeg' \
      -o -iname '*.bmp'  -o -iname '*.gif'  -o -iname '*.tif' -o -iname '*.tiff' \) \
      | sort | head -1 || true)"
fi

if [[ -z "$menu_src" || ! -f "$menu_src" ]]; then
  echo "Warning: game/gui/main_menu.* not found; skipped creating splash.png." >&2
elif resize_1080p "$menu_src" "$out/splash.png"; then
  echo "Wrote '$out/splash.png' (1920x1080) from '$(basename "$menu_src")'."
fi

# ---- 5. build game.json (in parent dir) from x-android.json ----------------
# Locate x-android.json (expected at assets/x-android.json, i.e. just above
# x-game). Fall back to any x-android.json entry if it lives elsewhere.
json_member=""
if grep -qx "assets/x-android.json" <<<"$listing"; then
  json_member="assets/x-android.json"
else
  json_member="$(grep -m1 -E '(^|/)x-android\.json$' <<<"$listing" || true)"
fi

if [[ -z "$json_member" ]]; then
  echo "Warning: x-android.json not found in the APK; skipped creating game.json." >&2
else
  tmp="$(mktemp)"
  set +e
  unzip -p "$apk" "$json_member" > "$tmp"
  rc=$?
  set -e
  die_on_unzip_error "$rc" "reading $json_member"

  python3 - "$tmp" "$out/game.json" <<'PY'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src, encoding="utf-8") as f:
    data = json.load(f)
result = {"name": data.get("name"), "version": data.get("version")}
missing = [k for k, v in result.items() if v is None]
with open(dst, "w", encoding="utf-8") as f:
    json.dump(result, f, indent=2, ensure_ascii=False)
    f.write("\n")
if missing:
    sys.stderr.write("Warning: key(s) not found in x-android.json: %s\n" % ", ".join(missing))
PY
  rm -f "$tmp"
  echo "Wrote '$out/game.json'."
fi

echo "Done. Extracted to '$out/' and stripped the 'x-' prefix from $renamed item(s)."
