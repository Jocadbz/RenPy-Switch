#!/usr/bin/env python3
"""Build pinned CPython as a Switch static library in build/python.

Configuration informed by PyNX (Copyright 2018 PyNX Authors; ISC), without
copying obsolete CPython internals. See docs/AUDIT.md for the reference revision.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from common import BUILD, ROOT, extract, run, target_env


def replace_once(path, old, new):
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise RuntimeError(f"Patch context not unique in {path}: {old!r}")
    path.write_text(text.replace(old, new))


def build(jobs, host_python):
    env = target_env()
    host_python = Path(shutil.which(host_python) or host_python).resolve()
    version = subprocess.check_output([host_python, "-c", "import sys; print('%d.%d' % sys.version_info[:2])"], text=True).strip()
    if version != "3.13":
        raise RuntimeError("CPython cross compilation needs host Python 3.13; use uv python install 3.13.14")
    source = extract("python", BUILD / "deps")
    out = BUILD / "python"
    out.mkdir(parents=True, exist_ok=True)
    # A configure-generated feature table is retained; unsupported calls are
    # disabled explicitly rather than replacing large files from an older Python.
    configure = source / "configure"
    text = configure.read_text()
    text = text.replace("\t*-*-linux*)", "\t*-*-linux*|aarch64-none-elf)")
    configure.write_text(text)
    replace_once(source / "Modules/timemodule.c", "#ifdef _MSC_VER\n#  define _Py_timezone", "#if defined(_MSC_VER) || defined(__SWITCH__)\n#  define _Py_timezone")
    # devkitA64 newlib defines clockid_t as unsigned long (64-bit on AArch64).
    replace_once(source / "Modules/timemodule.c", "#ifdef _AIX\n    long long clk_id", "#ifdef __SWITCH__\n    unsigned long clk_id = PyLong_AsUnsignedLong(obj);\n#elif defined(_AIX)\n    long long clk_id")
    replace_once(source / "Lib/subprocess.py",
                 'sys.platform not in {"emscripten", "wasi", "ios", "tvos", "watchos"}',
                 'sys.platform not in {"emscripten", "wasi", "ios", "tvos", "watchos", "horizon"}')
    dkp = Path(env["DEVKITPRO"])
    portlibs = dkp / "portlibs/switch"
    site = out / "config.site"
    disabled = ("mmap", "getrlimit", "setrlimit", "statvfs", "fstatvfs", "fork", "vfork",
                "execv", "execve", "fexecve", "posix_spawn", "posix_spawnp", "wait", "waitpid",
                "system", "popen", "pipe", "pipe2", "kill", "getuid", "geteuid", "getgid",
                "getegid", "getppid", "setsid", "setpgid", "fchdir", "chroot", "setgroups",
                "fdatasync", "symlink", "symlinkat", "readlink", "readlinkat", "link", "linkat")
    site.write_text("ac_cv_file__dev_ptmx=no\nac_cv_file__dev_ptc=no\nac_cv_lib_dl_dlopen=no\n"
                    "ac_cv_func_getentropy=yes\nac_cv_header_sys_resource_h=no\n"
                    "ac_cv_header_sys_statvfs_h=no\n" +
                    "".join(f"ac_cv_func_{name}=no\n" for name in disabled))
    arch = "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -fPIE"
    env.update(CC="aarch64-none-elf-gcc", CXX="aarch64-none-elf-g++", AR="aarch64-none-elf-ar",
               RANLIB="aarch64-none-elf-ranlib", CONFIG_SITE=str(site),
               CFLAGS=f"-O2 -g -ffunction-sections -fdata-sections {arch} -D__SWITCH__ -D_GNU_SOURCE -DSSIZE_MAX=__LONG_MAX__",
               CPPFLAGS=f"-I{dkp}/libnx/include -I{portlibs}/include",
               LDFLAGS=f"{arch} -specs={dkp}/libnx/switch.specs -L{dkp}/libnx/lib -L{portlibs}/lib",
               LIBS="-lnx -lm")
    if not (out / "Makefile").exists():
        run([configure, "--host=aarch64-none-elf", "--build=" + subprocess.check_output([source / "config.guess"], text=True).strip(),
             "--with-build-python=" + str(host_python), "--disable-shared", "--without-mimalloc",
             "--disable-ipv6", "--disable-test-modules", "--without-ensurepip"], cwd=out, env=env)
    # Optional modules need to be in the inittab, not un-loadable target .so files.
    setup = (out / "Modules/Setup.stdlib").read_text().replace("*shared*", "*static*")
    lines = []
    for line in setup.splitlines():
        name = line.split()[0] if line.split() else ""
        if name in {"_decimal", "pyexpat", "_elementtree", "_posixsubprocess", "_multiprocessing", "_posixshmem", "fcntl", "grp", "pwd", "syslog", "termios"}:
            line = "# " + line
        if name == "_sha2":
            line = "_sha2 sha2module.c -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_SHA2.c -D_BSD_SOURCE -D_DEFAULT_SOURCE"
        lines.append(line)
    local = out / "Modules/Setup.local"
    content = "\n".join(lines) + "\n"
    if not local.exists() or local.read_text() != content:
        local.write_text(content)
    run(["make", f"-j{jobs}", "libpython3.13.a"], cwd=out, env=env)
    prefix = out / "install"
    (prefix / "lib").mkdir(parents=True, exist_ok=True)
    shutil.copy2(out / "libpython3.13.a", prefix / "lib/libpython3.13.a")
    shutil.copytree(source / "Include", prefix / "include/python3.13", dirs_exist_ok=True)
    shutil.copy2(out / "pyconfig.h", prefix / "include/python3.13/pyconfig.h")
    print(f"Static Python: {prefix}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 6))
    parser.add_argument("--host-python", default="python3.13")
    args = parser.parse_args()
    try:
        build(args.jobs, args.host_python)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f"ERROR: {exc}")
