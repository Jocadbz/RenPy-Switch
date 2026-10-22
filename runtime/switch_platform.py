"""Horizon bootstrap and writable paths, independent of Ren'Py internals."""
import hashlib
import json
import os
import re


def _log(message):
    # Best effort only: fd 2, then the SD log file. Logging must never be able
    # to take the engine down.
    for writer in (
        lambda: os.write(2, (message + "\n").encode("utf-8", "replace")),
        lambda: open("sdmc:/renpy_switch.log", "a").write(message + "\n"),
    ):
        try:
            writer()
        except OSError:
            pass


def data_path(name):
    base = os.environ.get("RENPY_SWITCH_DATA_DIR")
    if not base:
        raise RuntimeError(
            "Storage not initialized. Report this with sdmc:/renpy_switch.log."
        )
    path = base + "/" + name
    try:
        os.makedirs(path, exist_ok=True)
    except OSError as exc:
        raise RuntimeError(
            "Cannot create %r on the SD card (%s). Insert an SD card and relaunch."
            % (path, exc)
        ) from exc
    return path


def initialize():
    archive = os.environ.get("RENPY_SWITCH_GAME_ARCHIVE")
    if archive:
        game_id = "archive-" + hashlib.sha256(archive.encode("utf-8")).hexdigest()[:24]
    else:
        with open("romfs:/Contents/switch-game.json", encoding="utf-8") as f:
            game_id = json.load(f)["id"]
    if not isinstance(game_id, str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", game_id):
        raise ValueError("Invalid packaged game id")
    data = "sdmc:/switch/renpy/data/" + game_id
    os.environ["RENPY_SWITCH_DATA_DIR"] = data
    os.environ["TMPDIR"] = data + "/cache"
    try:
        for name in ("saves", "logs", "cache"):
            os.makedirs(data + "/" + name, exist_ok=True)
        _log("[boot:3] writable data directory: " + data)
    except OSError as exc:
        # Booting read-only is fine; saving will fail later with a clear error.
        _log("[boot:3] WARNING: SD storage unavailable (%s); engine is read-only until an SD card is present" % exc)
    for name in ("encodings", "zlib", "_struct", "math", "_random", "_pickle",
                 "unicodedata", "hashlib", "threading", "ecdsa", "six"):
        __import__(name)
    entropy = os.urandom(32)
    if len(entropy) != 32:
        raise RuntimeError("os.urandom returned %d bytes" % len(entropy))
    _log("[boot:4] stdlib and dependencies imported")
