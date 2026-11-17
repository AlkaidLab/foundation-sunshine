#!/usr/bin/env python3
"""Fingerprint of everything that decides whether an earlier compiler cache still hits.

usage: config-fingerprint.py [--root DIR] [--workflow FILE] [--details]

A ccache entry is keyed on the compile command line, the compiler and the contents of the headers a translation unit reads. When
one of those moves for (nearly) every translation unit, a restored cache is worth nothing and the build is a cold one, however
many cache entries exist. This hashes what moves them for the whole build:

  * CMakeLists.txt and the CMake modules that set flags, definitions, targets and dependencies (everything under cmake/ except
    cmake/packaging, which only shapes the installer, cmake/sources, the lists of source files, and the Linux, macOS and Unix
    modules: none of those moves a Windows compile command line);
  * the cmake invocation of build_win's "Configure Windows" step and its ccache settings;
  * the toolchain action, the compiler of the job (TOOLCHAIN or CI_TOOLCHAIN: gcc, the default, or clang) and the MSYS2 package
    pins (scripts/ci/msys2-pins.txt, plus msys2-pins-clang.txt for a Clang job: the name, version, file and sha256 of every
    pinned package -- the exact compiler, CRT and libraries the Windows job installs). The repository is not read: an
    upstream update the pins do not take must not invalidate the cache, because the build still runs the pinned versions.

The first 12 hex digits go into the compiler cache key (ccache-windows-<fingerprint>-<run id>), so an entry is only restored by a
run with the same inputs, and cold_plan shards the compile when no entry exists for the fingerprint. main.yml appends the
toolchain mode to it (-pinned, or -newest for a probe build the weekly toolchain watch dispatches), so the two modes never
share an entry. Prints the digits.
"""
import argparse
import hashlib
import os
import pathlib
import sys

import yaml


def files_to_hash(root, toolchain):
    found = [root / "CMakeLists.txt"]
    cmake = root / "cmake"
    if cmake.is_dir():
        for path in sorted(cmake.rglob("*")):
            relative = path.relative_to(cmake).as_posix()
            parts = relative.split("/")
            other_platform = any(word in path.name.lower() for word in ("linux", "macos", "unix", "apple", "libcap", "libdrm", "libva",
                                                                         "systemd", "udev", "wayland", "evdev"))
            if path.is_file() and parts[0] not in ("packaging", "sources") and not other_platform:
                found.append(path)
    found.append(root / ".github" / "actions" / "windows-toolchain" / "action.yml")
    found.append(root / "scripts" / "ci" / "msys2-pins.txt")
    if toolchain == "clang":
        found.append(root / "scripts" / "ci" / "msys2-pins-clang.txt")
    return [p for p in found if p.is_file()]


def configure_step(workflow):
    with open(workflow, encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    for step in data["jobs"]["build_win"]["steps"]:
        if step.get("name") == "Configure Windows":
            return step
    raise SystemExit("no 'Configure Windows' step in build_win")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--workflow", default=".github/workflows/main.yml")
    parser.add_argument("--details", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(args.root)
    digest = hashlib.sha256()
    details = []
    toolchain = os.environ.get("TOOLCHAIN", os.environ.get("CI_TOOLCHAIN", "gcc")).lower()
    if toolchain not in ("gcc", "clang"):
        raise SystemExit(f"unsupported Windows toolchain: {toolchain}")

    def add(label, data):
        part = hashlib.sha256(data).hexdigest()[:12]
        digest.update(label.encode() + b"\0" + data + b"\0")
        details.append(f"{label}: {part}")

    for path in files_to_hash(root, toolchain):
        add(path.relative_to(root).as_posix(), path.read_bytes())
    step = configure_step(root / args.workflow)
    add("Configure Windows", (step["run"] + "\n" + "\n".join(f"{k}={v}" for k, v in sorted(step.get("env", {}).items()))).encode())
    add("Windows toolchain", toolchain.encode())
    for name in ("msys2-pins.txt",) + (("msys2-pins-clang.txt",) if toolchain == "clang" else ()):
        pins = root / "scripts" / "ci" / name
        if pins.is_file():
            # Not part of the digest (the file itself is hashed whole); the job summary shows the toolchain it covers.
            details.extend(f"    {line.split()[0]} {line.split()[1]}" for line in pins.read_text(encoding="utf-8").splitlines()
                           if line.strip() and not line.lstrip().startswith("#"))
    if args.details:
        print("\n".join(details), file=sys.stderr)
    print(digest.hexdigest()[:12])


main()
