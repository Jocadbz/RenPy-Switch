#!/usr/bin/env python3
"""Unprivileged, fail-fast Ren'Py Switch build driver (Python 3.11+)."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

from common import BUILD, ROOT, VERSIONS, digest, extract, run, target_env


def host_python():
    configured = os.environ.get("HOST_PYTHON")
    if configured:
        return configured
    found = shutil.which("python3.13")
    if found:
        return found
    if shutil.which("uv"):
        return subprocess.check_output(["uv", "python", "find", "3.13.14"], text=True).strip()
    raise RuntimeError("Need host Python 3.13. Set HOST_PYTHON, or run: uv python install 3.13.14")


def venv():
    python = ROOT / ".venv/bin/python"
    requirements = ROOT / "tools/requirements-build.txt"
    stamp = ROOT / ".venv/requirements.sha256"
    if not python.exists():
        if shutil.which("uv"):
            run(["uv", "venv", "--python", host_python(), ROOT / ".venv"])
        else:
            run([host_python(), "-m", "venv", ROOT / ".venv"])
    key = digest(requirements)
    if not stamp.exists() or stamp.read_text() != key:
        if shutil.which("uv"):
            run(["uv", "pip", "install", "--python", python, "-r", requirements])
        else:
            run([python, "-m", "pip", "install", "-r", requirements])
        stamp.write_text(key)
    return python


def prepare():
    source = extract("renpy", BUILD / "deps")
    specs = ((ROOT / "renpy.patch", ".switch-patch.sha256"),
             (ROOT / "patches/renpy-8.5.3-runtime.patch", ".runtime-patch.sha256"))
    current = {name: digest(patch) for patch, name in specs}
    stamps = {name: source / name for _, name in specs}
    if not all(stamp.exists() and stamp.read_text() == current[name] for name, stamp in stamps.items()):
        # Patch content evolved: rebuild the patched tree from pristine sources.
        shutil.rmtree(source)
        source = extract("renpy", BUILD / "deps")
        for patch, name in specs:
            run(["patch", "--dry-run", "--batch", "--forward", "--fuzz=0", "-p1", "-i", patch], cwd=source)
            run(["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", patch], cwd=source)
            (source / name).write_text(current[name])
    # Keep the RNG backend in a single maintained file (renpy.patch also contains
    # the original backend for historical source provenance).
    shutil.copy2(ROOT / "switch_enc.h", source / "src/libhydrogen/impl/random/switch_enc.h")
    return source


def doctor(env):
    run(["aarch64-none-elf-gcc", "--version"], env=env)
    run([env["PKG_CONFIG"], "--modversion", "sdl2", "SDL2_image", "SDL2_ttf", "egl", "glesv2",
         "libavformat", "libavcodec", "libavutil", "libswresample", "libswscale", "harfbuzz", "freetype2", "fribidi", "zlib"], env=env)
    for tool in ("cmake", "make", "patch"):
        if not shutil.which(tool):
            raise RuntimeError(f"Missing host tool: {tool}")
    print("Host Python:", host_python())


def runtime(source):
    python = venv()
    stdlib = BUILD / "deps/Python-3.13.14/Lib"
    if not (stdlib / "encodings/__init__.py").is_file():
        raise RuntimeError("Missing target stdlib; run the python stage first")
    root = BUILD / "runtime"
    contents = root / "romfs/Contents"
    contents.mkdir(parents=True, exist_ok=True)
    package = contents / "renpy"
    if package.exists():
        shutil.rmtree(package)
    shutil.copytree(source / "renpy/common", package / "common")
    for font in (source / "sdk-fonts").glob("*.ttf"):
        shutil.copy2(font, package / "common" / font.name)
    shutil.copy2(source / "renpy.py", contents / "renpy.py")
    shutil.copy2(ROOT / "runtime/switch_platform.py", contents / "switch_platform.py")
    shutil.copy2(ROOT / "sources/splash.png", contents / "splash.png")
    site = Path(subprocess.check_output([python, "-c", "import sysconfig; print(sysconfig.get_path('purelib'))"], text=True).strip())
    members = {}

    def add_tree(path, prefix="", exclude_common=False, skip=frozenset()):
        for file in sorted(path.rglob("*")):
            rel = file.relative_to(path)
            if not file.is_file() or skip.intersection(rel.parts):
                continue
            if exclude_common and rel.parts[0] == "common":
                continue
            if file.suffix in {".pyc", ".pyo", ".so", ".pyx", ".pxi", ".pxd"}:
                continue
            members[prefix + rel.as_posix()] = file

    # The stdlib's test suites are skipped; Ren'Py's own renpy/test package is
    # required (imported during import_all) and must be kept.
    add_tree(stdlib, skip={"test", "tests", "__pycache__", "idlelib", "tkinter", "turtledemo", "ensurepip", "site-packages"})
    add_tree(source / "renpy", "renpy/", exclude_common=True, skip={"__pycache__", "site-packages"})
    add_tree(site / "ecdsa", "ecdsa/", skip={"__pycache__"})
    members["six.py"] = site / "six.py"
    # The bootstrap must be importable even if the on-disk path finder cannot
    # list the RomFS root: ship it inside lib.zip (zip imports are proven by the
    # stdlib) in addition to the loose romfs:/Contents copy.
    members["switch_platform.py"] = ROOT / "runtime/switch_platform.py"
    # Source-only zip avoids mixing SDK/host bytecode with the target ABI. Stored
    # entries allow encodings to import before zlib has initialized.
    temporary = contents / "lib.zip.tmp"
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED) as zf:
        for name, file in sorted(members.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            zf.writestr(info, file.read_bytes())
    temporary.replace(contents / "lib.zip")
    # Fail the build (not the console) if core modules are missing from the zip.
    # renpy/common is deliberately disk-only (Ren'Py's rpy loader, not zipimport).
    required = {"encodings/__init__.py", "renpy/__init__.py", "renpy/test/__init__.py",
                "renpy/gl2/__init__.py", "switch_platform.py", "six.py", "ecdsa/__init__.py"}
    present = set(members)
    missing = required - present
    if missing:
        raise RuntimeError(f"lib.zip is missing required modules: {sorted(missing)}")
    licenses = contents / "licenses"
    licenses.mkdir(exist_ok=True)
    shutil.copy2(stdlib.parent / "LICENSE", licenses / "Python.txt")
    for path in source.glob("LICENSE*"):
        if path.is_file():
            shutil.copy2(path, licenses / ("RenPy-" + path.name))
    for dist in (site / "ecdsa-0.19.1.dist-info", site / "six-1.17.0.dist-info"):
        if not dist.is_dir():
            raise RuntimeError(f"Missing dependency metadata: {dist}")
        shutil.copytree(dist, licenses / dist.name, dirs_exist_ok=True)
    packages = subprocess.check_output(["pacman", "-Q"], text=True) if shutil.which("pacman") else "unavailable"
    manifest = {"schema": 1, "renpy_version": VERSIONS["renpy"]["version"],
                "python_version": VERSIONS["python"]["version"], "hardware_tested": False,
                "sources": VERSIONS, "renpy_patch_sha256": digest(ROOT / "renpy.patch"),
                "runtime_patch_sha256": digest(ROOT / "patches/renpy-8.5.3-runtime.patch"),
                "lib_zip_sha256": digest(contents / "lib.zip"),
                "packages": [line for line in packages.splitlines() if line.startswith(("devkit", "switch-", "libnx "))]}
    (root / "runtime.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Runtime staged: {root}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", nargs="?", default="all", choices=("doctor", "sources", "python", "generate", "native", "runtime", "all"))
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 6))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    env = target_env()
    if args.stage == "doctor":
        doctor(env)
        return
    if args.stage in {"sources", "all", "generate", "runtime"}:
        source = prepare()
    if args.stage in {"python", "all"}:
        from build_python import build
        build(args.jobs, host_python())
    if args.stage in {"generate", "all"}:
        python = venv()
        env["RENPY_CYTHON"] = str(python.parent / "cython")
        run([python, ROOT / "tools/generate_modules.py", source, BUILD / "generated", "--jobs", args.jobs], env=env)
    if args.stage in {"native", "all"}:
        run(["cmake", "-S", ROOT, "-B", BUILD / "native"], env=env)
        run(["cmake", "--build", BUILD / "native", "--parallel", args.jobs], env=env)
    if args.stage in {"runtime", "all"}:
        runtime(source)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f"ERROR: {exc}")
