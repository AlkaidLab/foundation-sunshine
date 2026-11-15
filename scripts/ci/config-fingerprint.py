#!/usr/bin/env python3
"""Fingerprint of everything that decides whether an earlier compiler cache still hits.

usage: config-fingerprint.py [--root DIR] [--workflow FILE] [--msys2-db FILE] [--no-packages] [--details]

A ccache entry is keyed on the compile command line, the compiler and the contents of the headers a translation unit reads. When
one of those moves for (nearly) every translation unit, a restored cache is worth nothing and the build is a cold one, however
many cache entries exist. This hashes what moves them for the whole build:

  * CMakeLists.txt and the CMake modules that set flags, definitions, targets and dependencies (everything under cmake/ except
    cmake/packaging, which only shapes the installer, cmake/sources, the lists of source files, and the Linux, macOS and Unix
    modules: none of those moves a Windows compile command line);
  * the cmake invocation of build_win's "Configure Windows" step and its ccache settings;
  * the toolchain action (the package list and pins);
  * the versions of the compiler, CRT, headers and library packages in the MSYS2 UCRT64 repository, read from the repository
    database (the Windows job installs the newest ones with pacman, so these are what it will compile with).

The first 12 hex digits go into the compiler cache key (ccache-windows-<fingerprint>-<run id>), so an entry is only restored by a
run with the same inputs, and cold_plan shards the compile when no entry exists for the fingerprint. Prints the digits.
"""
import argparse
import hashlib
import io
import pathlib
import subprocess
import sys
import tarfile
import urllib.request

import yaml

# The names the toolchain action installs (crt, headers and winpthreads come as the -git packages). opus is not listed: the
# action pins one exact package file, so its version is covered by the action.yml hash, not by the repository's newest opus.
PACKAGES = ("gcc", "gcc-libs", "binutils", "crt-git", "headers-git", "winpthreads-git", "libwinpthread-git", "curl-winssl",
            "MinHook", "miniupnpc", "nlohmann-json", "libvpl", "openssl", "cppwinrt")
PREFIX = "mingw-w64-ucrt-x86_64-"
DB_URL = "https://repo.msys2.org/mingw/ucrt64/ucrt64.db"


def files_to_hash(root):
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
    return [p for p in found if p.is_file()]


def configure_step(workflow):
    with open(workflow, encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    for step in data["jobs"]["build_win"]["steps"]:
        if step.get("name") == "Configure Windows":
            return step
    raise SystemExit("no 'Configure Windows' step in build_win")


def read_db(path):
    raw = pathlib.Path(path).read_bytes() if path else urllib.request.urlopen(DB_URL, timeout=60).read()
    tool = ["zstd", "-dc"] if raw[:4] == b"\x28\xb5\x2f\xfd" else ["gzip", "-dc"]
    tar_bytes = subprocess.run(tool, input=raw, capture_output=True, check=True).stdout
    versions = {}
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as tar:
        for member in tar.getmembers():
            if not member.name.endswith("/desc"):
                continue
            fields, current = {}, None
            for line in tar.extractfile(member).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    current = line.strip("%")
                    fields[current] = []
                elif line and current:
                    fields[current].append(line)
            versions[fields["NAME"][0]] = fields["VERSION"][0]
    return versions


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--workflow", default=".github/workflows/main.yml")
    parser.add_argument("--msys2-db")
    parser.add_argument("--no-packages", action="store_true")
    parser.add_argument("--details", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(args.root)
    digest = hashlib.sha256()
    details = []

    def add(label, data):
        part = hashlib.sha256(data).hexdigest()[:12]
        digest.update(label.encode() + b"\0" + data + b"\0")
        details.append(f"{label}: {part}")

    for path in files_to_hash(root):
        add(path.relative_to(root).as_posix(), path.read_bytes())
    step = configure_step(root / args.workflow)
    add("Configure Windows", (step["run"] + "\n" + "\n".join(f"{k}={v}" for k, v in sorted(step.get("env", {}).items()))).encode())
    if args.no_packages:
        add("msys2 packages", b"skipped")
    else:
        try:
            versions = read_db(args.msys2_db)
            lines = [f"{name} {versions.get(PREFIX + name, 'absent')}" for name in PACKAGES]
        except Exception as error:  # an unreachable repository must not stop the build: the fingerprint then lacks the versions
            print(f"warning: MSYS2 package versions unavailable ({error})", file=sys.stderr)
            lines = ["unavailable"]
        add("msys2 packages", "\n".join(lines).encode())
        details.extend("    " + line for line in lines)
    if args.details:
        print("\n".join(details), file=sys.stderr)
    print(digest.hexdigest()[:12])


main()
