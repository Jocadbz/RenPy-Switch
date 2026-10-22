#!/usr/bin/env python3
"""Validate a game directory and build a self-contained NRO without modifying it."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from common import BUILD, ROOT, digest, run, target_env

VERSION_RE = re.compile(r"\b(\d+\.\d+\.\d+(?:\.\d+)?)\b")
NATIVE_SUFFIXES = {".so", ".dll", ".pyd", ".dylib"}


def inspect_game(path):
    source = Path(path).resolve(strict=True)
    if not source.is_dir():
        raise ValueError("Supply an extracted game/ directory or its parent distribution directory")
    game = source / "game" if (source / "game").is_dir() else source
    files = []
    warnings = []
    evidence = []
    has_script = False
    for file in sorted(game.rglob("*")):
        rel = file.relative_to(game)
        if file.is_symlink():
            raise ValueError(f"Symlinks are not supported in game content: {rel}")
        if not file.is_file():
            if not file.is_dir():
                raise ValueError(f"Not a regular file/directory: {rel}")
            continue
        if ":" in rel.as_posix() or "\\" in rel.as_posix():
            raise ValueError(f"Invalid Switch path: {rel}")
        # User saves, host caches and bytecode are not game assets.
        if any(p in {"saves", "__pycache__", ".git"} for p in rel.parts) or file.suffix in {".pyc", ".pyo"}:
            continue
        if rel.parts[0] == "cache" and rel.name != "build_info.json":
            continue
        if file.stat().st_size >= 0x100000000:
            raise ValueError(f"File exceeds the FAT32 single-file limit: {rel}")
        suffix = file.suffix.lower()
        if suffix in {".rpy", ".rpyc"}:
            has_script = True
        if suffix == ".rpyc":
            with file.open("rb") as f:
                if f.read(10) != b"RENPY RPC2":
                    warnings.append(f"{rel}: old/custom compiled script format; compatibility unknown")
        if suffix == ".rpa":
            with file.open("rb") as f:
                header = f.readline(128)
            match = re.fullmatch(rb"RPA-([23])\.0 ([0-9a-fA-F]+)(?: ([0-9a-fA-F]+))?\s*", header)
            if not match or (match[1] == b"3" and not match[3]):
                raise ValueError(f"{rel}: unsupported or malformed RPA header (supported: RPA-2.0/3.0)")
            offset = int(match[2], 16)
            if not len(header) <= offset < file.stat().st_size:
                raise ValueError(f"{rel}: RPA index offset is outside the file")
            # No unpickling here: archive content is only interpreted by Ren'Py.
            has_script = True
            warnings.append(f"{rel}: RPA header checked; archived scripts/dependencies not inspected")
        if suffix in NATIVE_SUFFIXES or ".so." in file.name:
            warnings.append(f"{rel}: native library is not loadable by this static Horizon Python")
        if suffix == ".moc3":
            warnings.append(f"{rel}: Live2D is not built into this runtime")
        if suffix in {".glb", ".gltf", ".fbx"}:
            warnings.append(f"{rel}: assimp model loading is not built into this runtime")
        if suffix in {".rpy", ".py"} and file.stat().st_size < 4 * 1024 * 1024:
            text = file.read_text(encoding="utf-8-sig", errors="replace")
            for feature, pattern in (
                ("subprocess/process spawning", r"\b(?:import|from)\s+(?:subprocess|multiprocessing)\b|\bos\.(?:system|popen|fork)\s*\("),
                ("ctypes/native extensions", r"\b(?:import|from)\s+ctypes\b"),
                ("TLS/networking", r"\b(?:import|from)\s+(?:ssl|requests|urllib)\b"),
                ("desktop file dialogs", r"\brenpy\.tfd\b"),
            ):
                if re.search(pattern, text):
                    warnings.append(f"{rel}: references unsupported/unverified {feature}")
        files.append((rel.as_posix(), file))
    if not has_script:
        raise ValueError("No .rpy, .rpyc or .rpa game content found")
    # These are evidence, not proof that the original engine matches every asset.
    for file in (game / "cache/build_info.json", source / "renpy/vc_version.py",
                 source / "renpy/__init__.py", source / "log.txt"):
        if file.is_file() and file.stat().st_size < 4 * 1024 * 1024:
            text = file.read_text(encoding="utf-8", errors="replace")
            versions = set()
            if file.suffix == ".json":
                try:
                    info = json.loads(text)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"Invalid {file}: {exc}") from exc
                def find_version(obj):
                    if isinstance(obj, dict):
                        for key, value in obj.items():
                            if "renpy" in key.lower() and "version" in key.lower():
                                value = ".".join(map(str, value)) if isinstance(value, list) else str(value)
                                versions.update(VERSION_RE.findall(value))
                            find_version(value)
                    elif isinstance(obj, list):
                        for value in obj:
                            find_version(value)
                find_version(info)
            else:
                for line in text.splitlines():
                    stripped = line.strip()
                    if stripped.startswith("#"):
                        continue
                    match = re.match(r"\s*version_tuple\s*=\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)", line)
                    if match:
                        versions.add(".".join(match.groups()))
                    elif "renpy" in line.lower() or "version" in line.lower():
                        versions.update(VERSION_RE.findall(line))
            evidence.extend({"file": str(file.relative_to(source)), "version": version} for version in sorted(versions))
    versions = sorted({e["version"] for e in evidence})
    if not versions:
        warnings.append("Original Ren'Py version unknown; no compatibility claim can be made")
    elif any(not v.startswith("8.5.3") for v in versions):
        warnings.append("Engine migration to 8.5.3 requires game testing; original version(s): " + ", ".join(versions))
    warnings.append("Switch hardware compatibility is unverified; see docs/HARDWARE_TEST.md")
    return game, files, {"detected_versions": versions, "version_evidence": evidence, "warnings": warnings}


def validate_nro(path):
    total = Path(path).stat().st_size
    with Path(path).open("rb") as f:
        header = f.read(0x80)
        if len(header) < 0x80 or header[0x10:0x14] != b"NRO0":
            raise ValueError("elf2nro did not produce a valid NRO header")
        # elf2nro writes this as the end of the last load segment; the asset
        # header ("ASET") follows immediately after it.
        payload_end = struct.unpack_from("<I", header, 0x18)[0]
        if payload_end >= total:
            raise ValueError("NRO payload bounds are invalid")
        f.seek(payload_end)
        assets = f.read(0x38)
        if len(assets) != 0x38 or assets[:4] != b"ASET":
            raise ValueError("NRO is missing its asset header")
        nacp_offset, nacp_size, romfs_offset, romfs_size = struct.unpack_from("<QQQQ", assets, 0x18)
        if nacp_size != 0x4000 or romfs_size == 0:
            raise ValueError("NRO NACP/RomFS bounds are invalid")
        for off, length in ((nacp_offset, nacp_size), (romfs_offset, romfs_size)):
            # Asset offsets are relative to the payload end.
            if off < 0x38 or payload_end + off + length > total:
                raise ValueError("NRO NACP/RomFS bounds are invalid")


def package(args):
    game, files, report = inspect_game(args.game)
    if args.inspect:
        print(json.dumps(report, indent=2))
        return
    if not args.output or not args.id:
        raise ValueError("Packaging requires --output DIRECTORY and a stable --id (reuse it for game updates)")
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", args.id):
        raise ValueError("--id must be 1–80 ASCII letters, digits, underscores or hyphens")
    output = args.output.resolve()
    if output.exists():
        raise ValueError(f"Output already exists: {output}; choose a new directory")
    if output.is_relative_to(game):
        raise ValueError("Output must be outside the source game directory")
    runtime = args.runtime.resolve()
    manifest = json.loads((runtime / "runtime.json").read_text())
    if digest(runtime / "romfs/Contents/lib.zip") != manifest["lib_zip_sha256"]:
        raise ValueError("Runtime hash mismatch; rebuild the runtime stage")
    elf = args.elf.resolve(strict=True)
    env = target_env()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".renpy-package-", dir=output.parent) as temporary:
        work = Path(temporary)
        contents = work / "romfs/Contents"
        shutil.copytree(runtime / "romfs", work / "romfs")
        inventory = []
        for name, file in files:
            dest = contents / "game" / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file, dest)
            inventory.append({"path": name, "sha256": digest(dest), "size": dest.stat().st_size})
        identity = {"id": args.id, "name": args.name, "version": args.version}
        (contents / "switch-game.json").write_text(json.dumps(identity, indent=2) + "\n")
        deliver = work / "deliver"
        deliver.mkdir()
        nro = deliver / (args.id + ".nro")
        nacp = work / "game.nacp"
        run(["nacptool", "--create", args.name, args.author, args.version, nacp], env=env)
        command = ["elf2nro", elf, nro, f"--nacp={nacp}", f"--romfsdir={work / 'romfs'}"]
        if args.icon:
            command.append(f"--icon={args.icon.resolve(strict=True)}")
        run(command, env=env)
        validate_nro(nro)
        if nro.stat().st_size >= 0x100000000:
            raise ValueError("Packaged NRO exceeds FAT32's 4 GiB single-file limit")
        metadata = {"schema": 1, "game": identity, "compatibility": report, "runtime": manifest,
                    "elf_sha256": digest(elf), "nro_sha256": digest(nro), "files": inventory,
                    "script_compilation": "deferred to target engine; source game unchanged"}
        (deliver / "package.json").write_text(json.dumps(metadata, indent=2) + "\n")
        shutil.copytree(contents / "licenses", deliver / "licenses")
        shutil.copy2(ROOT / "docs/HARDWARE_TEST.md", deliver / "HARDWARE_TEST.md")
        deliver.rename(output)
    print(f"Packaged: {output / nro.name}")
    for warning in report["warnings"]:
        print("NOTE:", warning)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("--inspect", action="store_true", help="only print the validation/compatibility report")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--id", help="stable game identifier, also the SD save-directory key")
    parser.add_argument("--name", default="Ren'Py Switch")
    parser.add_argument("--author", default="Ren'Py Switch contributors")
    parser.add_argument("--version", default="0.1.0")
    parser.add_argument("--runtime", type=Path, default=BUILD / "runtime")
    parser.add_argument("--elf", type=Path, default=BUILD / "native/renpy-switch.elf")
    parser.add_argument("--icon", type=Path)
    args = parser.parse_args()
    try:
        package(args)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f"ERROR: {exc}")


if __name__ == "__main__":
    main()
