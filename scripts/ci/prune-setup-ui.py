#!/usr/bin/env python3
"""Remove assets the installer's setup UI never uses before Tauri embeds them.

The setup UI pages are foundation-ui's MInstaller root. Its stylesheet is the whole
foundation-ui stylesheet, so it also carries the clock components and their four
fonts; the page has no clock (its built JavaScript mentions no clock class), and a
clock rule or font that is never applied cannot change the window. The clock rules
and the @font-face blocks that point at those files are removed from the emitted
CSS, the font files are deleted, and source maps (none in the release build) are
removed. The script refuses to delete a file that the remaining CSS, JavaScript or
HTML still names, and writes a JSON report with the bytes of every removal.

usage: prune-setup-ui.py --dist dist-installer [--report report.json]

Exit codes: 0 = pruned, 2 = a refused removal or a missing input.

The embedded-directory shape is the one Tauri documents for frontendDist
(fs-tauri-config-20261122).
"""

import argparse
import json
import pathlib
import re
import sys

CLOCK_FONT = re.compile(r"^clock-[A-Za-z0-9_-]+\.woff2$")


class _UnsupportedCSS(Exception):
    """The stylesheet has a construct this parser does not understand; leave it unchanged."""


def _skip_comment(css, index):
    end = css.find("*/", index + 2)
    if end < 0:
        raise _UnsupportedCSS
    return end + 2


def _scan_to(css, start, stops):
    """Index of the first top-level character in stops, or None at the end of the text."""
    quote = None
    parens = brackets = 0
    index = start
    while index < len(css):
        char = css[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = None
            index += 1
            continue
        if char in ("'", '"'):
            quote = char
            index += 1
            continue
        if css.startswith("/*", index):
            index = _skip_comment(css, index)
            continue
        if char == "(":
            parens += 1
        elif char == ")":
            parens = max(0, parens - 1)
        elif char == "[":
            brackets += 1
        elif char == "]":
            brackets = max(0, brackets - 1)
        elif parens == 0 and brackets == 0 and char in stops:
            return index
        index += 1
    return None


def _block_end(css, start):
    """Index of the closing brace matching the opening brace at start."""
    depth = 1
    quote = None
    index = start + 1
    while index < len(css):
        char = css[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = None
            index += 1
            continue
        if char in ("'", '"'):
            quote = char
        elif css.startswith("/*", index):
            index = _skip_comment(css, index)
            continue
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise _UnsupportedCSS


def _parse_rules(css, start=0):
    """Return (text with clock rules removed, index after the parsed text).

    Semicolon-terminated at-rules (@charset, @import, @namespace, @layer a, b;) are copied verbatim before the next
    rule is examined; only rule blocks are filtered. Anything not understood raises _UnsupportedCSS so the caller can
    leave the whole stylesheet unchanged.
    """
    out = []
    index = start
    while index < len(css):
        char = css[index]
        if char.isspace():
            out.append(char)
            index += 1
            continue
        if css.startswith("/*", index):
            end = _skip_comment(css, index)
            out.append(css[index:end])
            index = end
            continue
        if char == ";":
            # Empty statements are legal; keep them where they are.
            out.append(char)
            index += 1
            continue
        if char == "}":
            raise _UnsupportedCSS
        if char == "@":
            stop = _scan_to(css, index, ";{")
            if stop is None:
                raise _UnsupportedCSS
            if css[stop] == ";":
                out.append(css[index:stop + 1])
                index = stop + 1
                continue
            prelude = css[index:stop]
            end = _block_end(css, stop)
            body = css[stop + 1:end]
            name = prelude.split(None, 1)[0].lower() if prelude.split() else ""
            if name == "@font-face":
                if "clock-" not in body.lower() and re.search(r"font-family\s*:\s*clock", body, re.I) is None:
                    out.append(css[index:end + 1])
            elif name in ("@media", "@supports", "@layer", "@container", "@scope"):
                inner, _ = _parse_rules(body)
                # An emptied @layer keeps its first-declaration position: the
                # cascade layer order is global, so dropping the block would
                # reorder every later layer (CSS Cascade 5, Layer Ordering,
                # fs-w3-css-cascade-5-20261122). The other groups can go.
                if inner.strip() or name == "@layer":
                    out.append(prelude + "{" + inner + "}")
            elif name in ("@keyframes", "@-webkit-keyframes"):
                if "clock" not in prelude.lower():
                    out.append(css[index:end + 1])
            elif name == "@property":
                out.append(css[index:end + 1])
            else:
                raise _UnsupportedCSS
            index = end + 1
            continue
        # A qualified rule: selector { ... }
        stop = _scan_to(css, index, ";{")
        if stop is None or css[stop] != "{":
            raise _UnsupportedCSS
        prelude = css[index:stop]
        end = _block_end(css, stop)
        if "clock" not in prelude.lower():
            out.append(css[index:end + 1])
        index = end + 1
    return "".join(out), index


def _strip_clock(css):
    """Return (text, supported). An unsupported stylesheet is returned unchanged."""
    try:
        stripped, index = _parse_rules(css)
    except _UnsupportedCSS:
        return css, False
    if index != len(css):
        return css, False
    return stripped, True


def _referenced(root):
    """Every file name the remaining CSS, JavaScript and HTML mentions."""
    names = set()
    for path in root.rglob("*"):
        if path.suffix.lower() not in (".css", ".js", ".html", ".json"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        names.update(re.findall(r"[A-Za-z0-9._-]+\.[A-Za-z0-9]{2,5}", text))
    return names


def prune(dist, report_path=None):
    dist = pathlib.Path(dist)
    if not dist.is_dir():
        raise SystemExit(f"prune-setup-ui: {dist} is not a directory")
    removed, rewritten, unsupported = [], [], []

    for css_path in sorted(dist.rglob("*.css")):
        before = css_path.read_text(encoding="utf-8")
        after, supported = _strip_clock(before)
        if not supported:
            unsupported.append(str(css_path.relative_to(dist)))
            continue
        if after != before:
            css_path.write_text(after, encoding="utf-8", newline="")
            rewritten.append({"file": str(css_path.relative_to(dist)), "before": len(before.encode()), "after": len(after.encode())})

    referenced = _referenced(dist)
    clock_fonts = [p for p in dist.rglob("*") if p.is_file() and CLOCK_FONT.match(p.name)]
    if unsupported:
        # A stylesheet this parser does not understand may still name the fonts: leave every asset as it is.
        print("::warning title=setup UI assets::CSS not understood in " + ", ".join(unsupported) + "; assets left unchanged")
    else:
        for path in clock_fonts:
            if path.name in referenced:
                raise SystemExit(f"prune-setup-ui: refusing to remove {path.relative_to(dist)}: still referenced")
        for path in sorted(dist.rglob("*.map")):
            if path.name in referenced:
                raise SystemExit(f"prune-setup-ui: refusing to remove {path.relative_to(dist)}: still referenced")
            removed.append({"file": str(path.relative_to(dist)), "bytes": path.stat().st_size})
            path.unlink()
        for path in clock_fonts:
            removed.append({"file": str(path.relative_to(dist)), "bytes": path.stat().st_size})
            path.unlink()

    for path in sorted(dist.rglob("*")):
        if path.is_file() and path.suffix.lower() in (".css", ".js", ".html"):
            text = path.read_text(encoding="utf-8", errors="replace")
            for name in re.findall(r"(?:src|href)=\"\./([^\"]+)\"", text):
                if not (path.parent / name).exists():
                    raise SystemExit(f"prune-setup-ui: {path.relative_to(dist)} names a missing file: {name}")

    result = {
        "removed": removed,
        "removed_bytes": sum(entry["bytes"] for entry in removed),
        "rewritten": rewritten,
        "rewritten_bytes": sum(entry["before"] - entry["after"] for entry in rewritten),
        "unsupported_css": unsupported,
        "locales_removed": 0,
        "debug_info_removed": 0,
    }
    if report_path:
        pathlib.Path(report_path).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for entry in removed:
        print(f"removed {entry['file']}: {entry['bytes']:,} bytes")
    for entry in rewritten:
        print(f"rewrote {entry['file']}: {entry['before'] - entry['after']:,} bytes")
    print(f"setup UI assets: {result['removed_bytes'] + result['rewritten_bytes']:,} bytes reclaimed")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dist", required=True, help="the dist-installer directory that Tauri embeds")
    parser.add_argument("--report", help="write the per-removal report here")
    args = parser.parse_args(argv)
    prune(args.dist, args.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
