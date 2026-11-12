#!/usr/bin/env python3
"""Windows-parity checks for the Linux port.

Windows CI builds this repository, and this branch adds a lot of Linux code to
shared files. Two failure modes are easy to introduce and easy to miss:

  1. an edit that lands inside a `#if defined(_WIN32)` region, silently
     changing what Windows compiles (AGENTS.md: un-gating a Windows-only
     feature is fine only if the Windows branch is byte-identical afterwards);
  2. a Linux-only source that is listed in the shared build files
     (cmake/sources/host.cmake, cmake/targets/*) instead of the Linux ones, so
     the Windows build tries to compile it.

This script reports both. Run it before pushing:

    tools/audit-windows-parity.py                    # default origin/master..HEAD
    tools/audit-windows-parity.py <base>..<head>

Exit status is non-zero when check 2 finds a leak; check 1 always needs a
human eye, so it only prints.
"""
import fnmatch
import os
import re
import subprocess
import sys

IF_FAMILY = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")
HAS_WIN32 = re.compile(r"\b_WIN32\b")
NEGATED_WIN32 = re.compile(r"(#\s*ifndef\s+_WIN32)|!\s*_WIN32|!\s*defined\s*\(\s*_WIN32\s*\)")
HUNK = re.compile(r"@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@")

# Directories that only ever build on Linux; their sources must come from the
# Linux build files.
LINUX_ONLY = ("src/platform/linux/",)


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def windows_region_map(lines):
    """1-based line number -> would this line be compiled when _WIN32 is defined?"""
    stack = []
    result = {}
    for number, line in enumerate(lines, start=1):
        match = IF_FAMILY.match(line)
        if match:
            directive = match.group(1)
            if directive == "endif":
                if stack:
                    stack.pop()
            elif directive == "else":
                if stack:
                    stack[-1]["windows"] = stack[-1]["else_windows"]
                    stack[-1]["else_windows"] = not stack[-1]["else_windows"]
            elif directive == "elif":
                text = line
                matched = bool(HAS_WIN32.search(text)) and not NEGATED_WIN32.search(text)
                stack[-1]["windows"] = matched
                stack[-1]["else_windows"] = bool(HAS_WIN32.search(text)) and bool(NEGATED_WIN32.search(text))
            else:  # if / ifdef / ifndef
                text = line
                windows = bool(HAS_WIN32.search(text)) and not NEGATED_WIN32.search(text)
                else_windows = bool(HAS_WIN32.search(text)) and bool(NEGATED_WIN32.search(text))
                stack.append({"windows": windows, "else_windows": else_windows})
        result[number] = bool(stack) and any(frame["windows"] for frame in stack)
    return result


def hunk_lines(diff):
    old = new = 0
    for line in diff.splitlines():
        match = HUNK.match(line)
        if match:
            old, new = int(match.group(1)), int(match.group(2))
            continue
        if line.startswith("---") or line.startswith("+++"):
            continue
        if line.startswith("-"):
            yield ("-", old, line[1:])
            old += 1
        elif line.startswith("+"):
            yield ("+", new, line[1:])
            new += 1
        elif line.startswith(" "):
            old += 1
            new += 1


def changed_files(base, head):
    return [path for path in run("git", "diff", "--name-only", f"{base}..{head}").split("\n") if path]


def is_shared(path):
    return path.endswith((".cpp", ".h", ".hpp")) and not path.startswith(("src/platform/linux/", "tests/", "src_assets/"))


def check_windows_regions(base, head, files):
    print(f"== check 1: changed lines inside Windows-only regions ({base}..{head}) ==")
    hits = 0
    for path in files:
        if not is_shared(path) or not os.path.exists(path):
            continue
        try:
            before = run("git", "show", f"{base}:{path}").split("\n")
        except subprocess.CalledProcessError:
            print(f"  new file (review its guards by hand): {path}")
            continue

        after = run("git", "show", f"{head}:{path}").split("\n")
        diff = run("git", "diff", "-U0", f"{base}..{head}", "--", path)
        old_windows = windows_region_map(before)
        new_windows = windows_region_map(after)

        for kind, line, text in hunk_lines(diff):
            in_windows = old_windows.get(line) if kind == "-" else new_windows.get(line)
            if not in_windows:
                continue
            # Preprocessor directives themselves are part of the change's shape,
            # not of the code Windows compiles.
            if IF_FAMILY.match(text):
                continue
            verdict = "REMOVED from" if kind == "-" else "ADDED to"
            print(f"  {path}:{line} {verdict} the Windows build: {text.strip()}")
            hits += 1
    print(f"  -> {hits} line(s) to review by hand\n")
    return hits


def listed_tokens(cmake_file):
    """Path-ish tokens a CMake file adds to the build.

    Sources are written both bare (`src/foo.cpp`) and wrapped in
    `${CMAKE_SOURCE_DIR}`, quoted or not, and glob patterns look the same, so
    tokenise the file instead of trusting quotes.
    """
    text = open(cmake_file, encoding="utf-8").read()
    text = text.replace("${CMAKE_SOURCE_DIR}/", "").replace("${PROJECT_SOURCE_DIR}/", "")
    return [
        token
        for token in re.split(r"[\s()\"]+", text)
        if token.startswith("src/") and "/" in token
    ]


def check_linux_sources():
    print("== check 2: Linux-only sources are gated out of the shared build ==")
    host = "cmake/sources/host.cmake"
    platform = "cmake/compile_definitions/linux.cmake"

    leaks = [token for token in listed_tokens(host) if token.startswith(LINUX_ONLY)]
    for token in leaks:
        print(f"  LEAK: {host} lists Linux-only source {token}")

    patterns = [token for token in listed_tokens(platform) if token.startswith(LINUX_ONLY)]
    tracked = [path for path in run("git", "ls-files").split("\n") if path.startswith(LINUX_ONLY) and path.endswith(".cpp")]
    unlisted = [path for path in tracked if not any(fnmatch.fnmatch(path, pattern) for pattern in patterns)]
    for path in unlisted:
        print(f"  UNLISTED: {path} is under {LINUX_ONLY[0]} but no rule in {platform} builds it")

    print(f"  -> {len(tracked)} Linux-only source(s) checked, {len(unlisted)} unlisted, {len(leaks)} leak(s)\n")
    return len(unlisted) + len(leaks)


def main(argv):
    span = argv[1] if len(argv) > 1 else "origin/master..HEAD"
    base, _, head = span.partition("..")
    files = changed_files(base or "origin/master", head or "HEAD")
    check_windows_regions(base, head or "HEAD", files)
    return 1 if check_linux_sources() else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
