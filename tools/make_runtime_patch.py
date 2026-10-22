#!/usr/bin/env python3
"""Generate patches/renpy-8.5.3-runtime.patch mechanically.

Hand-maintaining unified-diff hunk counts and blank-context bytes is
error-prone. This script applies each transform to the renpy.patch-applied
source tree in memory with uniqueness assertions, then emits difflib hunks
with exact byte counts. Run it after every transform change, then verify with
`python3 tools/build.py sources`.

Precondition: build/deps/renpy-8.5.3-source exists with renpy.patch applied
and no runtime patch applied (the state `prepare()` re-creates). The tree is
never modified by this script.
"""
from __future__ import annotations

import difflib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "build/deps/renpy-8.5.3-source"
OUTPUT = ROOT / "patches/renpy-8.5.3-runtime.patch"

MARKER5 = """    try:
        import switch_platform
        switch_platform._log("[boot:5] renpy bootstrap entered")
    except Exception:
        pass
"""

MARKER6 = """    try:
        import switch_platform
        switch_platform._log("[boot:6] presplash finished; importing _renpy")
    except Exception:
        pass
"""


def exact(old, new):
    return ("exact", old, new)


def regex(pattern, replacement):
    return ("regex", pattern, replacement)


TRANSFORMS = {
    "renpy.py": [
        exact(
            '''    if renpy.switch:
        # Every homebrew NRO launched from hbmenu runs under the SAME ProgramId, so
        # the mounted "save:" partition is shared across all builds. Isolate each game
        # by its config.save_directory (Ren'Py assigns a unique value per project), so
        # different games get their own save folder instead of clobbering each other.
        # (Different builds/versions of the SAME game keep the same save_directory and
        # therefore share saves, which is the desired behaviour for updates.)
        sd = save_directory
        if isinstance(sd, bytes):
            sd = sd.decode("utf-8", "replace")
        if sd:
            import re
            sub = re.sub(r"[^A-Za-z0-9._-]", "_", sd).strip("_")
            if sub:
                return "save:/Data/" + sub + "/"
        return "save:/Data/"
''',
            '''    if renpy.switch:
        # Package identity isolates games, including those without save_directory.
        # Keep token and script-backup requests separate from ordinary save slots.
        from switch_platform import data_path
        if save_directory in ("tokens", b"tokens", "backups", b"backups"):
            name = save_directory.decode() if isinstance(save_directory, bytes) else save_directory
            return data_path(name)
        return data_path("saves")
''',
        ),
        exact(
            '''    if renpy.switch:
        return "save:/Logs/"
''',
            '''    if renpy.switch:
        from switch_platform import data_path
        return data_path("logs")
''',
        ),
    ],
    "renpy/main.py": [
        exact(
            '''        renpy.config.searchpath = [ renpy.config.gamedir, renpy.config.commondir, "save://" ]''',
            '''        renpy.config.searchpath = [ renpy.config.gamedir, renpy.config.commondir ]''',
        ),
    ],
    "renpy/persistent.py": [
        exact(
            '''    elif renpy.switch:
        files = ["save://MultiPersistent"]
''',
            '''    elif renpy.switch:
        from switch_platform import data_path
        files = [data_path("multipersistent")]
''',
        ),
    ],
    "renpy/exports/restartexports.py": [
        # The switch quit block was inserted by renpy.patch; match it as a range
        # (it contains typographic punctuation) and replace it entirely.
        regex(
            r"    if renpy\.switch and not relaunch:\n(?:.*\n)*?        return\n",
            '    if renpy.switch and relaunch:\n        raise RuntimeError("Relaunch is unsupported; quit and reopen the NRO from hbmenu.")\n',
        ),
    ],
    "renpy/bootstrap.py": [
        exact(
            '''    renpy.config.renpy_base = renpy_base
''',
            '''    renpy.config.renpy_base = renpy_base

''' + MARKER5,
        ),
        exact(
            '''        else:
            renpy.display.presplash.start(basedir, gamedir)
''',
            '''        else:
            renpy.display.presplash.start(basedir, gamedir)

''' + MARKER6,
        ),
    ],
    "renpy/gl2/gl2shadercache.py": [
        exact(
            '''            fn = os.path.join(renpy.config.gamedir, renpy.loader.get_path(self.filename))
''',
            '''            if renpy.switch:
                from switch_platform import data_path
                fn = data_path("cache") + "/shaders.txt"
            else:
                fn = os.path.join(renpy.config.gamedir, renpy.loader.get_path(self.filename))
''',
        ),
        exact(
            '''            with renpy.loader.load(self.filename) as f:
''',
            '''            if renpy.switch:
                from switch_platform import data_path
                f = io.open(data_path("cache") + "/shaders.txt", "rb")
            else:
                f = renpy.loader.load(self.filename)
            with f:
''',
        ),
    ],
}


def apply(text, edits):
    for kind, *args in edits:
        if kind == "exact":
            old, new = args
            count = text.count(old)
            if count != 1:
                raise RuntimeError(f"exact transform found {count} times: {old[:60]!r}")
            if new in text:
                raise RuntimeError(f"transform already applied: {new[:60]!r}")
            text = text.replace(old, new)
        else:
            pattern, replacement = args
            matches = list(re.finditer(pattern, text))
            if len(matches) != 1:
                raise RuntimeError(f"regex transform found {len(matches)} matches: {pattern}")
            if replacement in text:
                raise RuntimeError(f"regex transform already applied: {replacement[:60]!r}")
            match = matches[0]
            text = text[: match.start()] + replacement + text[match.end():]
    return text


def main():
    sections = []
    for path, edits in TRANSFORMS.items():
        original = (SOURCE / path).read_text()
        modified = apply(original, edits)
        old = original.splitlines()
        new = modified.splitlines()
        sections.extend(difflib.unified_diff(old, new, f"a/{path}", f"b/{path}", n=3))
    # unified_diff appends lineterm only to ---/+++/@@ lines; normalize so every
    # line gets exactly one trailing newline.
    lines = [line[:-1] if line.endswith("\n") else line for line in sections]
    OUTPUT.write_text("\n".join(lines) + "\n")
    print(f"Wrote {OUTPUT} ({len(sections)} lines)")


if __name__ == "__main__":
    main()
