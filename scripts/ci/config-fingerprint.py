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
import re
import pathlib
import sys

import yaml

# A ccache entry is keyed on the compile command line, the compiler and the header contents a translation unit reads
# (ccache manual, "How ccache works"), so adding or removing a translation unit never invalidates another unit's entry.
# Statements that only enumerate source files therefore must not move the fingerprint: two source-list-only edits forced
# 8-shard cold builds on 2026-11-21 although no compile command line had changed. Cut whole set()/list() statements that
# fill a variable whose name ends in _TARGET_FILES or _SOURCES (PLATFORM_TARGET_FILES and its kin); file(GLOB ...) patterns
# stay hashed, as do flag, definition and dependency statements.
SOURCE_LIST_STATEMENT = re.compile(
    r"(?<![\w.])(?:set|list)\s*\(\s*(?:(?:APPEND|PREPEND|INSERT)\s+)?[A-Za-z0-9_]*(?:_TARGET_FILES|_SOURCES)\b",
    re.IGNORECASE,
)
def statement_end(text, start):
    """Index just past the parenthesis that closes the command at start, or None.

    Follows the CMake language grammar (cmake-language(7), Grammar): a hash runs to the end of
    the line, a quoted argument may contain unbalanced parentheses and escapes, and a bracket
    argument [=*[ ... ]=*] ends only at its own closing bracket, so none of their parentheses
    count towards the nesting depth. None means the statement never closes; the caller then
    keeps it hashed as it is rather than guessing where it ends.
    """
    index, depth = start, 0
    while index < len(text):
        char = text[index]
        if char == "#":
            newline = text.find("\n", index)
            index = len(text) if newline < 0 else newline + 1
            continue
        if char == '"':
            index += 1
            while index < len(text):
                if text[index] == "\\":
                    index += 2
                elif text[index] == '"':
                    index += 1
                    break
                else:
                    index += 1
            continue
        if char == "[" and (index == 0 or text[index - 1] in " \t("):
            opening = index + 1
            while opening < len(text) and text[opening] == "=":
                opening += 1
            if opening < len(text) and text[opening] == "[":
                closing = "]" + "=" * (opening - index - 1) + "]"
                end = text.find(closing, opening + 1)
                if end < 0:
                    return None
                index = end + len(closing)
                continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return None


def strip_source_lists(text):
    """Remove whole source-list statements, with the line they sit on.

    Cutting only the statement would leave its indentation and newline behind, so adding or
    removing a complete set()/list() line would still move the fingerprint.
    """
    out = []
    position = 0
    while True:
        match = SOURCE_LIST_STATEMENT.search(text, position)
        if not match:
            out.append(text[position:])
            return "".join(out)
        line_start = text.rfind("\n", position, match.start()) + 1
        if text[line_start:match.start()].strip():
            line_start = match.start()
        end = statement_end(text, match.start())
        if end is None:
            # An unclosed statement is kept hashed as it is: guessing its extent could swallow
            # the rest of the file and hide real compile-command changes from the fingerprint.
            out.append(text[position:match.end()])
            position = match.end()
            continue
        out.append(text[position:line_start])
        while end < len(text) and text[end] in " \t":
            end += 1
        if end < len(text) and text[end] == "\r":
            end += 1
        if end < len(text) and text[end] == "\n":
            end += 1
        position = end


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
        data = path.read_bytes()
        if path.name == "CMakeLists.txt" or path.suffix == ".cmake":
            data = strip_source_lists(data.decode("utf-8", errors="surrogateescape")).encode("utf-8", errors="surrogateescape")
        add(path.relative_to(root).as_posix(), data)
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
