"""Shared build utilities. No privileged installation or shell evaluation."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
CACHE = ROOT / "downloads"
VERSIONS = json.loads((ROOT / "tools/versions.json").read_text())


def run(args, *, cwd=ROOT, env=None):
    args = [str(a) for a in args]
    print("+ " + shlex.join(args), flush=True)
    subprocess.run(args, cwd=cwd, env=env, check=True)


def digest(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def fetch(component):
    spec = VERSIONS[component]
    CACHE.mkdir(exist_ok=True)
    dest = CACHE / spec["url"].rsplit("/", 1)[1]
    if not dest.exists():
        temporary = dest.with_suffix(dest.suffix + ".part")
        print(f"Downloading {spec['url']}", flush=True)
        try:
            with urllib.request.urlopen(spec["url"], timeout=60) as src, temporary.open("wb") as out:
                shutil.copyfileobj(src, out)
            if digest(temporary) != spec["sha256"]:
                raise RuntimeError(f"SHA-256 mismatch: {spec['url']}")
            temporary.replace(dest)
        finally:
            temporary.unlink(missing_ok=True)
    if digest(dest) != spec["sha256"]:
        raise RuntimeError(f"SHA-256 mismatch: {dest}; remove this cached download and retry")
    return dest


def extract(component, parent):
    archive = fetch(component)
    parent = Path(parent)
    parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tf:
        roots = {Path(m.name).parts[0] for m in tf.getmembers() if m.name}
        if len(roots) != 1:
            raise RuntimeError(f"Expected one source root in {archive}")
        root = parent / roots.pop()
        if root.exists():
            return root
        tf.extractall(parent, filter="data")
    return root


def target_env():
    env = os.environ.copy()
    dkp = Path(env.get("DEVKITPRO", "/opt/devkitpro")).resolve()
    a64 = Path(env.get("DEVKITA64", dkp / "devkitA64")).resolve()
    for path in (a64 / "bin/aarch64-none-elf-gcc", dkp / "libnx/include/switch.h",
                 dkp / "cmake/Switch.cmake", dkp / "tools/bin/elf2nro"):
        if not path.is_file():
            raise RuntimeError(f"Missing {path}. Install devkitPro switch-dev; see docs/BUILD.md")
    env.update(DEVKITPRO=str(dkp), DEVKITA64=str(a64))
    env["PATH"] = os.pathsep.join((str(a64 / "bin"), str(dkp / "tools/bin"), env["PATH"]))
    # Never leak host headers/libraries into target feature detection.
    for key in ("C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "CPATH", "LIBRARY_PATH", "PYTHONPATH"):
        env.pop(key, None)
    env["PKG_CONFIG"] = str(dkp / "portlibs/switch/bin/aarch64-none-elf-pkg-config")
    env["PKG_CONFIG_PATH"] = ""
    env["PKG_CONFIG_LIBDIR"] = str(dkp / "portlibs/switch/lib/pkgconfig")
    return env
