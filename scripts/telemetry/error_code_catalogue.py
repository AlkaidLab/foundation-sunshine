#!/usr/bin/env python3
"""The dictionary of the error codes the host reports: code -> the fixed words and the places in the source that log them.

An `error` event carries only a code, `e` and eight hex digits, the FNV-1a hash of the fixed words a log message starts with (src/telemetry/error_code.h). This script finds every
BOOST_LOG(error) / BOOST_LOG(fatal) statement in a source tree, takes the literal text it starts with, computes the same code and writes the table, so that an investigator can turn a code
from the data into the message and the file:line that logs it. A statement that starts with a value (a variable, a call) has no fixed words: it is listed under the code of the empty head
and with its place, which is all the data can say about it.

usage: error_code_catalogue.py SRC_DIR [--json OUT.json]
Standard library only. `error_head` mirrors the C++ function; tests/unit/test_telemetry.cpp (ErrorCode*) and scripts/telemetry/test_error_code_catalogue.py hold the same vectors.
"""

import json
import os
import re
import sys

LOG = re.compile(r"BOOST_LOG\((error|fatal)\)\s*<<\s*((?:[^;]|\n)*?);", re.S)
PIECE = re.compile(r'\s*("(?:[^"\\]|\\.)*")\s*(?:sv|s)?\s*')


def fnv1a(text: str) -> int:
    h = 2166136261
    for b in text.encode():
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def error_head(text: str, max_chars: int = 96) -> str:
    head = ""
    i = 0
    if text.startswith("["):
        close = text.find("]")
        if 1 < close <= 41 and all(c.isalpha() and c.isascii() or c in " _-." for c in text[1:close]):
            head = text[1:close].lower() + " "
            i = close + 1
    prefix_used = False
    while i < len(text):
        c = text[i]
        if c.isdigit() and c.isascii() or c in '="[({</\\':
            break
        if c == "'":
            if head and head[-1].isalpha() and head[-1].isascii() and i + 1 < len(text) and text[i + 1].isalpha() and text[i + 1].isascii():
                head += c
                i += 1
                continue
            break
        if c == ":":
            words = head.count(" ") + (1 if head else 0)
            if not prefix_used and words <= 2 and len(head) <= 24 and i + 1 < len(text) and text[i + 1] == " ":
                prefix_used = True
                if head and head[-1] != " ":
                    head += " "
                i += 1
                continue
            break
        if c == " " and (not head or head[-1] == " "):
            i += 1
            continue
        head += c.lower() if c.isascii() else c
        i += 1
    head = head.rstrip(" ")
    if len(head) > max_chars:
        head = head[:max_chars]
        sp = head.rfind(" ")
        if sp > 0:
            head = head[:sp]
    return head


def error_code(text: str) -> str:
    return "e%08x" % fnv1a(error_head(text))


def leading_literals(statement: str) -> str:
    out = []
    for piece in statement.split("<<"):
        m = PIECE.fullmatch(piece)
        if not m:
            break
        out.append(bytes(m.group(1)[1:-1], "utf-8").decode("unicode_escape", errors="ignore") if "\\" in m.group(1) else m.group(1)[1:-1])
    return "".join(out)


def scan(root: str):
    table = {}
    for dirpath, dirnames, files in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "node_modules", "third-party", "build")]
        for name in files:
            if not name.endswith((".cpp", ".h", ".mm", ".hpp")):
                continue
            path = os.path.join(dirpath, name)
            text = open(path, errors="ignore").read()
            for m in LOG.finditer(text):
                message = leading_literals(m.group(2))
                line = text.count("\n", 0, m.start()) + 1
                code = error_code(message)
                entry = table.setdefault(code, {"head": error_head(message), "sites": []})
                entry["sites"].append({"file": os.path.relpath(path, root).replace(os.sep, "/"), "line": line, "level": m.group(1), "text": message[:120]})
    return table


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    table = scan(args[0])
    if "--json" in args:
        with open(args[args.index("--json") + 1], "w", encoding="utf-8") as out:
            json.dump(table, out, ensure_ascii=False, indent=1, sort_keys=True)
    for code in sorted(table):
        e = table[code]
        print(f"{code}  {e['head']!r}  ({len(e['sites'])} places, e.g. {e['sites'][0]['file']}:{e['sites'][0]['line']})")
