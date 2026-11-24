#!/usr/bin/env python3
"""Fail when the staged install tree holds a path longer than the installer's budget.

The installer lets a person choose the install folder (tools/installer_dir.h). The longest folder it accepts is
MAX_PATH - 1 - kLongestRelative - kPathMargin, so that the longest path of the package still fits under MAX_PATH
(the setup engine, the batch scripts and the service wrapper work within it). kLongestRelative is a constant of the
header; this check keeps it true: it measures the deepest path of the staged tree (a leading backslash plus the
relative path with backslashes) and fails when the package has grown past the constant, so that the header is raised
(and the longest accepted folder shrinks) in the same change that adds the file.

usage: check-install-path-budget.py <staging directory> [--header tools/installer_dir.h]
"""

import argparse
import pathlib
import re
import sys


def read_budget(header: pathlib.Path) -> int:
    text = header.read_text(encoding="utf-8")
    found = re.search(r"kLongestRelative\s*=\s*(\d+)\s*;", text)
    if not found:
        sys.exit(f"kLongestRelative not found in {header}")
    return int(found.group(1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("staging", type=pathlib.Path)
    parser.add_argument("--header", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2] / "tools" / "installer_dir.h")
    args = parser.parse_args()
    if not args.staging.is_dir():
        sys.exit(f"not a directory: {args.staging}")
    budget = read_budget(args.header)
    deepest = ("", 0)
    for path in args.staging.rglob("*"):
        if not path.is_file():
            continue
        # UTF-16 units, as Windows counts them: a character outside the BMP is two
        relative = "\\" + "\\".join(path.relative_to(args.staging).parts)
        length = sum(2 if ord(c) > 0xFFFF else 1 for c in relative)
        if length > deepest[1]:
            deepest = (relative, length)
    print(f"deepest path in the package: {deepest[1]} characters (budget {budget}): {deepest[0]}")
    if deepest[1] > budget:
        print(f"error: raise kLongestRelative in {args.header.name} to at least {deepest[1]}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
