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


def fnv1a(data) -> int:
    h = 2166136261
    for b in (data.encode() if isinstance(data, str) else data):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def _alpha(b: int) -> bool:
    return 65 <= b <= 90 or 97 <= b <= 122  # std::isalpha in the C locale: ASCII letters only


def error_head_bytes(text, max_bytes: int = 96) -> bytes:
    """Byte for byte what error_head() in src/telemetry/error_code.h does: it works on the UTF-8 bytes, lower-cases ASCII only, and cuts at `max_bytes` BYTES (a multi-byte character may be cut in half)."""
    s = text.encode() if isinstance(text, str) else bytes(text)
    head = bytearray()
    i = 0
    if s[:1] == b"[":
        close = s.find(b"]")
        if 1 < close <= 41 and all(_alpha(c) or c in b" _-." for c in s[1:close]):
            head += s[1:close].lower() + b" "
            i = close + 1
    prefix_used = False
    n = len(s)
    while i < n:
        c = s[i]
        if 48 <= c <= 57 or c in b'="[({</\\':
            break
        if c == 0x27:  # '
            if head and _alpha(head[-1]) and i + 1 < n and _alpha(s[i + 1]):
                head.append(c)
                i += 1
                continue
            break
        if c == 0x3A:  # :
            words = head.count(b" ") + (1 if head else 0)
            if not prefix_used and words <= 2 and len(head) <= 24 and i + 1 < n and s[i + 1] == 0x20:
                prefix_used = True
                if head and head[-1] != 0x20:
                    head.append(0x20)
                i += 1
                continue
            break
        if c == 0x20 and (not head or head[-1] == 0x20):
            i += 1
            continue
        head.append(c + 32 if 65 <= c <= 90 else c)
        i += 1
    while head and head[-1] == 0x20:
        head.pop()
    if len(head) > max_bytes:
        del head[max_bytes:]
        sp = head.rfind(b" ")
        if sp > 0:
            del head[sp:]
    return bytes(head)


def error_head(text: str, max_bytes: int = 96) -> str:
    """The head as text for display (a cut multi-byte character shows as a replacement character; the code is computed from the bytes)."""
    return error_head_bytes(text, max_bytes).decode("utf-8", errors="replace")


def error_code(text: str) -> str:
    return "e%08x" % fnv1a(error_head_bytes(text))


def _comment_end(text: str, start: int) -> int:
    """Return the first character after the C++ comment at ``start``."""
    if text.startswith("//", start):
        end = text.find("\n", start + 2)
        return len(text) if end < 0 else end
    if text.startswith("/*", start):
        end = text.find("*/", start + 2)
        return len(text) if end < 0 else end + 2
    return start


def _skip_space_and_comments(text: str, start: int) -> int:
    i = start
    while i < len(text):
        if text[i].isspace():
            i += 1
        elif text.startswith("//", i) or text.startswith("/*", i):
            i = _comment_end(text, i)
        else:
            break
    return i


def _mask_comments(text: str) -> str:
    """Replace C++ comments with spaces while preserving strings and line numbers."""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        if text[i] in "\"'":
            quote = text[i]
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                elif text[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
        elif text.startswith("//", i):
            end = _comment_end(text, i)
            for j in range(i, end):
                if text[j] != "\n":
                    out[j] = " "
            i = end
        elif text.startswith("/*", i):
            end = _comment_end(text, i)
            for j in range(i, end):
                if text[j] != "\n":
                    out[j] = " "
            i = end
        else:
            i += 1
    return "".join(out)


def statement_end(text: str, start: int) -> int:
    """The index of the semicolon that ends the statement, skipping literals and comments."""
    i = start
    n = len(text)
    while i < n:
        if text.startswith("//", i) or text.startswith("/*", i):
            i = _comment_end(text, i)
            continue
        if text[i] in "\"'":
            quote = text[i]
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
        elif text[i] == ";":
            return i
        i += 1
    return n


def split_pieces(statement: str):
    """The operands of the `<<` chain, ignoring strings and C++ comments."""
    pieces, cur, i, n = [], [], 0, len(statement)
    while i < n:
        if statement.startswith("//", i) or statement.startswith("/*", i):
            end = _comment_end(statement, i)
            cur.append("\n" if "\n" in statement[i:end] else " ")
            i = end
            continue
        if statement[i] in "\"'":
            quote = statement[i]
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
        c = statement[i]
        cur.append(c)
        i += 1
    pieces.append("".join(cur))
    return pieces


LITERAL = re.compile(r'\s*"((?:[^"\\]|\\.)*)"\s*(?:sv|s)?\s*')


_SIMPLE_ESCAPES = {
    "a": 0x07,
    "b": 0x08,
    "f": 0x0C,
    "n": 0x0A,
    "r": 0x0D,
    "t": 0x09,
    "v": 0x0B,
    "\\": 0x5C,
    "'": 0x27,
    '"': 0x22,
    "?": 0x3F,
}


def _decode_cpp_literal(raw: str) -> bytes:
    """Decode an ordinary C++ string literal to the bytes used by a narrow literal."""
    out = bytearray()
    i = 0
    while i < len(raw):
        if raw[i] != "\\":
            end = i + 1
            while end < len(raw) and raw[end] != "\\":
                end += 1
            out.extend(raw[i:end].encode("utf-8"))
            i = end
            continue
        i += 1
        if i >= len(raw):
            break
        escaped = raw[i]
        if escaped in _SIMPLE_ESCAPES:
            out.append(_SIMPLE_ESCAPES[escaped])
            i += 1
        elif escaped in "01234567":
            end = i + 1
            while end < len(raw) and end < i + 3 and raw[end] in "01234567":
                end += 1
            out.append(int(raw[i:end], 8) & 0xFF)
            i = end
        elif escaped == "x":
            end = i + 1
            while end < len(raw) and raw[end] in "0123456789abcdefABCDEF":
                end += 1
            if end == i + 1:
                out.extend(b"x")
                i = end
            else:
                out.append(int(raw[i + 1:end], 16) & 0xFF)
                i = end
        else:
            # Keep the historical behaviour for unsupported escapes.  GCC and
            # Clang both use the escaped character for these diagnostics.
            out.extend(escaped.encode("utf-8"))
            i += 1
    return bytes(out)


def unescape(raw: str) -> str:
    """Decode an ordinary C++ string literal for callers that need text."""
    return _decode_cpp_literal(raw).decode("utf-8", errors="replace")


def _literal_at(text: str, start: int):
    i = _skip_space_and_comments(text, start)
    for prefix in ("u8", "u", "U", "L"):
        if text.startswith(prefix, i):
            i += len(prefix)
            break
    if i >= len(text) or text[i] != '"':
        return None
    j = i + 1
    while j < len(text):
        if text[j] == "\\":
            j += 2
        elif text[j] == '"':
            break
        else:
            j += 1
    if j >= len(text):
        return None
    value = _decode_cpp_literal(text[i + 1:j])
    j += 1
    suffix_start = _skip_space_and_comments(text, j)
    for suffix in ("sv", "s"):
        if text.startswith(suffix, suffix_start):
            j = suffix_start + len(suffix)
            break
    return value, _skip_space_and_comments(text, j)


def _leading_literal_bytes(statement: str) -> bytes:
    """The literal bytes at the front of the `<<` chain."""
    out = bytearray()
    for piece in split_pieces(statement):
        pos = 0
        parts = bytearray()
        while True:
            parsed = _literal_at(piece, pos)
            if parsed is None:
                break
            value, pos = parsed
            parts.extend(value)
        if not parts or _skip_space_and_comments(piece, pos) != len(piece):
            break
        out.extend(parts)
    return bytes(out)


def leading_literals(statement: str) -> str:
    """The text the message starts with: the string literals at the front of the `<<` chain, adjacent literals ("a" "b") joined, up to the first operand that is not a literal."""
    return _leading_literal_bytes(statement).decode("utf-8", errors="replace")


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
            for m in LOG_START.finditer(_mask_comments(text)):
                line = text.count("\n", 0, m.start()) + 1
                message_bytes = _leading_literal_bytes(text[m.end():statement_end(text, m.end())])
                code = error_code(message_bytes)
                entry = table.setdefault(code, {"head": error_head(message_bytes), "sites": []})
                entry["sites"].append({"file": os.path.relpath(path, root).replace(os.sep, "/"), "line": line, "level": m.group(1), "text": message_bytes[:120].decode("utf-8", errors="replace")})
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
