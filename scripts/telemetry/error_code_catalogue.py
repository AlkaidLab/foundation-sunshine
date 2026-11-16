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

LOG_START = re.compile(r"BOOST_LOG\((error|fatal)\)\s*<<")


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


def statement_end(text: str, start: int) -> int:
    """The index of the semicolon that ends the statement, skipping string and character literals (a `;` inside a message does not end it)."""
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
        elif c == ";":
            return i
        i += 1
    return n


def split_pieces(statement: str):
    """The operands of the `<<` chain (a `<<` inside a literal is not a separator)."""
    pieces, cur, i, n = [], [], 0, len(statement)
    while i < n:
        c = statement[i]
        if c in "\"'":
            quote = c
            j = i + 1
            while j < n and statement[j] != quote:
                j += 2 if statement[j] == "\\" else 1
            cur.append(statement[i:j + 1])
            i = j + 1
            continue
        if statement.startswith("<<", i):
            pieces.append("".join(cur))
            cur = []
            i += 2
            continue
        cur.append(c)
        i += 1
    pieces.append("".join(cur))
    return pieces


LITERAL = re.compile(r'\s*"((?:[^"\\]|\\.)*)"\s*(?:sv|s)?\s*')


def unescape(raw: str) -> str:
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), raw)


def leading_literals(statement: str) -> str:
    """The text the message starts with: the string literals at the front of the `<<` chain, adjacent literals ("a" "b") joined, up to the first operand that is not a literal."""
    out = []
    for piece in split_pieces(statement):
        pos, parts = 0, []
        while pos < len(piece):
            m = LITERAL.match(piece, pos)
            if not m:
                break
            parts.append(unescape(m.group(1)))
            pos = m.end()
        if parts and pos == len(piece):
            out.append("".join(parts))
        else:
            break
    return "".join(out)


def scan(root: str):
    table = {}
    for dirpath, dirnames, files in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "node_modules", "third-party", "build")]
        for name in files:
            if not name.endswith((".cpp", ".h", ".mm", ".hpp")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, errors="ignore") as handle:
                text = handle.read()
            for m in LOG_START.finditer(text):
                message = leading_literals(text[m.end():statement_end(text, m.end())])
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
