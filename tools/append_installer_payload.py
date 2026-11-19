#!/usr/bin/env python3
"""Build the self-contained installer: the shell exe, then the optional sections, then the Inno engine.

usage: append_installer_payload.py shell engine output [--ui setup-ui.exe] [--wv2 MicrosoftEdgeWebview2Setup.exe]

Layout: [shell][ui][wv2][engine][footer]. The footer keeps the V1 keys first (offset=, size=, sha256= of the engine), so
a shell that only knows V1 still finds the engine; <name>-offset=, <name>-size=, <name>-sha256= follow for each section.
  ui   the setup UI (Panel repo installer-ui), the web front end of the shell
  wv2  the WebView2 Evergreen bootstrapper, run by the elevated worker when the Runtime is missing
"""

import hashlib
import pathlib
import shutil
import sys


def take(args, flag):
    if flag not in args:
        return None
    at = args.index(flag)
    if at + 1 >= len(args):
        raise SystemExit(f"{flag} needs a path")
    value = pathlib.Path(args[at + 1])
    del args[at:at + 2]
    return value


def main() -> int:
    args = sys.argv[1:]
    sections = [(name, take(args, "--" + name)) for name in ("ui", "wv2")]
    sections = [(name, path.read_bytes()) for name, path in sections if path]
    if len(args) != 3:
        raise SystemExit("usage: append_installer_payload.py shell engine output [--ui setup-ui.exe] [--wv2 MicrosoftEdgeWebview2Setup.exe]")
    shell, engine, output = map(pathlib.Path, args)
    payload = engine.read_bytes()
    at = shell.stat().st_size
    placed = []
    for name, data in sections:
        placed.append((name, at, data))
        at += len(data)
    footer = (
        "\nALKAID-SHELL-PAYLOAD-V1\n"
        f"offset={at}\n"
        f"size={len(payload)}\n"
        f"sha256={hashlib.sha256(payload).hexdigest()}\n"
    )
    for name, offset, data in placed:
        footer += f"{name}-offset={offset}\n{name}-size={len(data)}\n{name}-sha256={hashlib.sha256(data).hexdigest()}\n"
    with output.open("wb") as destination:
        with shell.open("rb") as source:
            shutil.copyfileobj(source, destination, 1024 * 1024)
        for _, _, data in placed:
            destination.write(data)
        destination.write(payload)
        destination.write(footer.encode("ascii"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
