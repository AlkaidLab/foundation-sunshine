#!/usr/bin/env python3
"""The dictionary of the error codes the host reports: code -> the fixed words and the places in the source that log them.

The legacy `error` event carried a code, `e` and eight hex digits, the FNV-1a hash of the fixed words a log message starts with (src/telemetry/error_code.h). This script finds every
BOOST_LOG(error) / BOOST_LOG(fatal) statement in a source tree, takes the literal text it starts with, computes the same code and writes the table, so that an investigator can turn a code
from the data into the message and the file:line that logs it. A statement that starts with a value (a variable, a call) has no fixed words: it is listed under the code of the empty head
and with its place.
In --templates mode, emit the build catalogue of all static stream operands with placeholders for values.

usage: error_code_catalogue.py SRC_DIR [--json OUT.json | --templates OUT.inc]
Standard library only. `error_head` mirrors the C++ function; tests/unit/test_error_code.cpp (ErrorCode*) and scripts/telemetry/test_error_code_catalogue.py hold the same vectors.
"""

import json
import os
import re
import sys
from pathlib import Path

LOG_START = re.compile(r"BOOST_LOG\((error|fatal)\)\s*<<")


def _drop_controls(data: bytes) -> bytes:
    """The bytes a log line keeps after events.cpp drop_controls: tab, CR and LF
    become a space; other C0 characters and DEL go; C1, U+2028..U+202E,
    U+2066..U+2069 and the BOM go. The host hashes this form, not the literal."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        c = data[i]
        if c in (0x09, 0x0A, 0x0D):
            out.append(0x20)
            i += 1
            continue
        if c < 0x20 or c == 0x7F:
            i += 1
            continue
        if c == 0xC2 and i + 1 < n and 0x80 <= data[i + 1] <= 0x9F:
            i += 2
            continue
        if c == 0xE2 and i + 2 < n:
            b, d = data[i + 1], data[i + 2]
            if (b == 0x80 and 0xA8 <= d <= 0xAE) or (b == 0x81 and 0xA6 <= d <= 0xA9):
                i += 3
                continue
        if c == 0xEF and i + 2 < n and data[i + 1] == 0xBB and data[i + 2] == 0xBF:
            i += 3
            continue
        out.append(c)
        i += 1
    return bytes(out)


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


_ID_CHARS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
_RAW_PREFIXES = ("u8R", "uR", "UR", "LR", "R")


def _raw_string_at(text: str, i: int):
    """If a raw string literal (R"d(...)d", optionally u8/u/U/L-prefixed) starts at ``i``,
    return (content_start, content_end, end); otherwise None."""
    if i > 0 and text[i - 1] in _ID_CHARS:
        return None
    for prefix in _RAW_PREFIXES:
        if text.startswith(prefix + '"', i):
            j = i + len(prefix) + 1
            break
    else:
        return None
    paren = text.find("(", j, j + 17)  # the delimiter has at most 16 characters
    if paren < 0:
        return None
    delim = text[j:paren]
    # a double quote may stand in the delimiter: only space, parentheses,
    # backslash and control characters are not allowed (C++ [lex.string])
    if any(c in delim for c in ' ()\\\t\v\f\n'):
        return None
    close = text.find(")" + delim + '"', paren + 1)
    if close < 0:
        return None
    return paren + 1, close, close + len(delim) + 2


def _is_digit_separator(text: str, i: int) -> bool:
    """True when the quote at ``i`` is a C++14 digit separator (1'000, 0xFF'FF), not a character literal."""
    if text[i] != "'" or i == 0 or text[i - 1] not in _ID_CHARS:
        return False
    k = i
    # the dot of a floating literal (0x1.a'bp0, .5'0) belongs to the token:
    # without it the quote after the fraction looks like a character literal
    while k > 0 and (text[k - 1] in _ID_CHARS or text[k - 1] in "'."):
        k -= 1
    token = text[k:i]
    if token in ("u8", "u", "U", "L"):
        return False  # u8'a', L'a': a character literal with an encoding prefix
    return token[:1].isdigit() or (token[:1] == "." and len(token) > 1 and token[1].isdigit())


def _literal_end(text: str, i: int):
    """If a string or character literal (raw ones included) starts at ``i``, the index after it; otherwise None."""
    raw = _raw_string_at(text, i)
    if raw is not None:
        return raw[2]
    if text[i] not in "\"'" or _is_digit_separator(text, i):
        return None
    quote = text[i]
    j = i + 1
    n = len(text)
    while j < n:
        if text[j] == "\\":
            j += 2
        elif text[j] == quote:
            return j + 1
        else:
            j += 1
    return n


def _comment_end(text: str, start: int) -> int:
    """Return the first character after the C++ comment at ``start``."""
    if text.startswith("//", start):
        # a backslash right before the newline splices the next line into the comment (translation phase 2)
        end = text.find("\n", start + 2)
        while end >= 0 and text[start:end].rstrip("\r").endswith("\\"):
            end = text.find("\n", end + 1)
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


def _mask_comments(text: str, *, literals=False) -> str:
    """Mask comments and optionally literals without changing offsets or line numbers."""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        end = _literal_end(text, i)
        if end is not None:
            if literals:
                out[i:end] = ["\n" if c == "\n" else " " for c in text[i:end]]
            i = end
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
    depth = 0
    while i < n:
        if text.startswith("//", i) or text.startswith("/*", i):
            i = _comment_end(text, i)
            continue
        end = _literal_end(text, i)
        if end is not None:
            i = end
            continue
        c = text[i]
        if c == ";" and depth == 0:
            return i
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return i
            depth -= 1
        i += 1
    return n


def split_pieces(statement: str):
    """The operands of the `<<` chain, ignoring strings and C++ comments."""
    pieces, cur, i, n = [], [], 0, len(statement)
    depth = 0
    while i < n:
        if statement.startswith("//", i) or statement.startswith("/*", i):
            end = _comment_end(statement, i)
            cur.append("\n" if "\n" in statement[i:end] else " ")
            i = end
            continue
        end = _literal_end(statement, i)
        if end is not None:
            cur.append(statement[i:end])
            i = end
            continue
        if depth == 0 and statement.startswith("<<", i):
            pieces.append("".join(cur))
            cur = []
            i += 2
            continue
        c = statement[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
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
    raw = _raw_string_at(text, i)
    if raw is not None:
        content_start, content_end, j = raw
        value = text[content_start:content_end].encode("utf-8")  # a raw literal has no escapes
        suffix_start = _skip_space_and_comments(text, j)
        suffixed = False
        for suffix in ("sv", "s"):
            if text.startswith(suffix, suffix_start):
                j = suffix_start + len(suffix)
                suffixed = True
                break
        return value, _skip_space_and_comments(text, j), suffixed
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
    suffixed = False
    for suffix in ("sv", "s"):
        if text.startswith(suffix, suffix_start):
            j = suffix_start + len(suffix)
            suffixed = True
            break
    return value, _skip_space_and_comments(text, j), suffixed


def _literal_operand(piece: str):
    pos = 0
    parts = bytearray()
    found = False
    suffixed = False
    while (parsed := _literal_at(piece, pos)) is not None:
        value, pos, has_suffix = parsed
        parts.extend(value)
        found = True
        suffixed = suffixed or has_suffix
    if not found or _skip_space_and_comments(piece, pos) != len(piece):
        return None
    if not suffixed and 0 in parts:
        del parts[parts.index(0):]  # plain literals are inserted as C strings
    return bytes(parts)


def _leading_literal_bytes(statement: str) -> bytes:
    """The literal bytes at the front of the stream chain (legacy catalogue)."""
    out = bytearray()
    for piece in split_pieces(statement):
        literal = _literal_operand(piece)
        if literal is None:
            break
        out.extend(literal)
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
                # the host hashes the message after drop_controls (events.cpp), so the table does the same
                message_bytes = _drop_controls(_leading_literal_bytes(text[m.end():statement_end(text, m.end())]))
                code = error_code(message_bytes)
                entry = table.setdefault(code, {"head": error_head(message_bytes), "sites": []})
                entry["sites"].append({"file": os.path.relpath(path, root).replace(os.sep, "/"), "line": line, "level": m.group(1), "text": message_bytes[:120].decode("utf-8", errors="replace")})
    return table


def message_template(statement: str) -> bytes:
    """Keep static stream operands on either side of each argument."""
    parts = [_literal_operand(piece) for piece in split_pieces(statement)]
    if not any(part and part.strip() for part in parts):
        return b""  # no fixed words: the caller uses the source site
    return b"".join(part if part is not None else b"{}" for part in parts)


def _cpp_bytes(text: bytes) -> str:
    return '"' + "".join(chr(c) if 32 <= c < 127 and c not in (34, 92)
                         else f"\\{c:03o}" for c in text) + '"'


def write_templates(root: Path, output: Path):
    # cpp-lex-string-20261120, [lex.string]: join adjacent literals and
    # preserve the compiled bytes. Every other stream operand is an argument.
    rows = []
    # python-pathlib-20261120, Path.rglob and Path.read_text.
    for path in sorted((root / "src").rglob("*")):
        if path.suffix not in (".cpp", ".h", ".mm", ".hpp"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        file = path.relative_to(root).as_posix()
        for match in LOG_START.finditer(_mask_comments(text, literals=True)):
            line = text.count("\n", 0, match.start()) + 1
            template = message_template(text[match.end():statement_end(text, match.end())])
            rows.append((file, line, template))
    # An ambiguous physical site falls back to location. The runtime lookup
    # has only the location of the statement, so it cannot pick either row.
    counts = {}
    for file, line, _ in rows:
        counts[file, line] = counts.get((file, line), 0) + 1
    lines = [
        f'  {{ "{file}", {line}, {{ {_cpp_bytes(template)}, {len(template)} }} }},'
        for file, line, template in sorted(rows) if template and counts[file, line] == 1
    ]
    content = "// Generated by error_code_catalogue.py. Do not edit.\n" + "\n".join(lines) + "\n"
    output.write_text(content, encoding="ascii")


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if len(args) == 3 and args[1] == "--templates":
        write_templates(Path(args[0]), Path(args[2]))
        sys.exit()
    table = scan(args[0])
    if "--json" in args:
        with open(args[args.index("--json") + 1], "w", encoding="utf-8") as out:
            json.dump(table, out, ensure_ascii=False, indent=1, sort_keys=True)
    for code in sorted(table):
        e = table[code]
        print(f"{code}  {e['head']!r}  ({len(e['sites'])} places, e.g. {e['sites'][0]['file']}:{e['sites'][0]['line']})")
