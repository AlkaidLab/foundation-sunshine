#!/usr/bin/env python3
"""Check that relative links and images in Markdown files point at files that exist.

usage: check-doc-links.py [--root DIR] [FILE.md ...]     (no files: every tracked *.md under the root)

Checks inline links/images  [text](target)  and reference definitions  [id]: target  whose target is a relative path.
Absolute URLs, mailto:, in-page anchors, links inside fenced code blocks, generated *.html pages and paths into
third-party/ (submodules may be absent) are ignored; a trailing #anchor or ?query is
stripped before the file is looked up. External URLs are deliberately not fetched: a CI check that depends on third-party
sites fails for reasons that are not in the change.

Exit status 1 and one ::error annotation per broken link (workflow command syntax, usable in fork pull requests).
"""

import argparse
import pathlib
import re
import subprocess
import sys
from urllib.parse import unquote

INLINE = re.compile(r"!?\[[^\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REFDEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s+.*)?$")


def targets(text):
    in_fence = False
    for number, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for match in INLINE.finditer(line):
            yield number, match.group(1)
        match = REFDEF.match(line)
        if match:
            yield number, match.group(1)


def is_local(target):
    return not (target.startswith(("#", "mailto:", "tel:", "data:")) or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("//"))


def check(root, files):
    broken = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, target in targets(text):
            if not is_local(target):
                continue
            clean = unquote(re.split(r"[#?]", target, maxsplit=1)[0])
            if not clean:
                continue
            # Doxygen output (md_docs_2guides.html) is generated, and submodule contents are not always checked out.
            if clean.endswith(".html") or "third-party/" in clean:
                continue
            base = root if clean.startswith("/") else path.parent
            resolved = (base / clean.lstrip("/")).resolve()
            if not resolved.exists():
                broken.append((path, number, target))
    return broken


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()
    if args.files:
        files = [pathlib.Path(f).resolve() for f in args.files if f.endswith(".md") and pathlib.Path(f).exists()]
    else:
        # -z: names with spaces or non-ASCII characters are listed verbatim, not quoted
        listed = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "*.md"], capture_output=True, text=True, check=True).stdout
        files = [root / f for f in listed.split("\0") if f]
    broken = check(root, files)
    for path, number, target in broken:
        rel = path.relative_to(root) if path.is_relative_to(root) else path
        print(f"::error file={rel},line={number},title=Broken link::{target} does not exist")
    print(f"checked {len(files)} Markdown files, {len(broken)} broken relative links")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
