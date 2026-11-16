#!/usr/bin/env python3
"""The MSYS2 toolchain pins: compare them with the repository, and rewrite them.

usage: msys2-pins.py check [--pins FILE]... [--db FILE] [--simulate name=version[,...]] [--json]
       msys2-pins.py bump  [--pins FILE]... [--db FILE]

--pins may be repeated; without it both pin files are used (msys2-pins.txt and, for the Clang builds, msys2-pins-clang.txt).

check reads the pins (scripts/ci/msys2-pins.txt), resolves the newest version of every pinned package from the UCRT64
repository database (the same database pacman synchronises; NAME, VERSION, FILENAME and SHA256SUM of every desc entry)
and reports every pin whose repository version carries a newer MAJOR -- openssl also on a newer MINOR, because the ABI
of its libraries moves within a major. --simulate overrides the repository versions (name=version, comma separated, short
or full names) to exercise the alert path without waiting for a real release. Without --json a table is printed; with it,
a machine-readable summary for .github/workflows/msys2-toolchain-watch.yml (including the Chinese issue title). A
repository that cannot be read is an error, not an empty result: a watch that silently sees nothing is worthless.

bump rewrites the pins file with the newest version, file name and sha256 of every pinned package, sorted by name; the
header comment is kept, and a package the repository no longer carries is left untouched and reported, not dropped. That
is the whole bump procedure: review the diff, adapt the code to API changes, run a Windows build, merge. The weekly watch
closes its tracking issue by itself once the pins cover the newer versions.
"""
import argparse
import io
import json
import os
import pathlib
import re
import subprocess
import sys
import tarfile
import time
import urllib.request

DB_URLS = ("https://repo.msys2.org/mingw/ucrt64/ucrt64.db", "https://mirror.msys2.org/mingw/ucrt64/ucrt64.db")
PREFIX = "mingw-w64-ucrt-x86_64-"
ROUNDS = 3


def fetch_db():
    """The repository database, from the first server that answers; MSYS2_DB_URLS (space separated) replaces them for the unit test."""
    urls = os.environ.get("MSYS2_DB_URLS", "").split() or DB_URLS
    pause = float(os.environ.get("MSYS2_PINS_RETRY_PAUSE", "5"))
    error = None
    for attempt in range(ROUNDS):
        for url in urls:
            try:
                return urllib.request.urlopen(url, timeout=30).read()
            except Exception as failure:
                error = failure
                print(f"warning: {url}: {failure}", file=sys.stderr)
        if attempt + 1 < ROUNDS:
            time.sleep(pause * (attempt + 1))
    raise SystemExit(f"::error::the MSYS2 repository database could not be read ({error})")


def read_db(path):
    raw = pathlib.Path(path).read_bytes() if path else fetch_db()
    tool = ["zstd", "-dc"] if raw[:4] == b"\x28\xb5\x2f\xfd" else ["gzip", "-dc"]
    tar_bytes = subprocess.run(tool, input=raw, capture_output=True, check=True).stdout
    packages = {}
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as tar:
        for member in tar.getmembers():
            if not member.name.endswith("/desc"):
                continue
            fields, current = {}, None
            for line in tar.extractfile(member).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    current = line.strip("%")
                    fields[current] = []
                elif line and current:
                    fields[current].append(line)
            packages[fields["NAME"][0]] = (fields["VERSION"][0], fields["FILENAME"][0], fields["SHA256SUM"][0])
    return packages


def load_pins(path):
    """(header lines, entries): entries are dicts with name, version, file and sha256, in file order."""
    header, entries, seen = [], [], set()
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            if not entries:
                header.append(line)
            continue
        parts = stripped.split()
        if len(parts) != 4:
            raise SystemExit(f"::error::{path}: a pin is not 'name version file sha256': {line}")
        if parts[0] in seen:
            raise SystemExit(f"::error::{path}: {parts[0]} is pinned twice")
        seen.add(parts[0])
        entries.append(dict(zip(("name", "version", "file", "sha256"), parts)))
    return header, entries


def components(version):
    """The dotted version components without the epoch and the package release: '16.2.0-7' gives ['16', '2', '0'],
    MSYS2's epoch spellings '1:2.0-3' and '1~2.0-3' both give ['2', '0']."""
    text = version.strip()
    text = re.sub(r"^\d+[:~]", "", text)
    text = text.rsplit("-", 1)[0] if "-" in text else text
    return text.split(".")


def bump_level(name, pinned, newest):
    """'major' when the first component moved, 'minor' for openssl's second component, else None (no alert)."""
    old, new = components(pinned), components(newest)
    if not old or not new:
        return None
    if not old[0].isdigit() or not new[0].isdigit():
        return None
    if int(new[0]) > int(old[0]):
        return "major"
    if (name.endswith("-openssl") and len(old) > 1 and len(new) > 1 and old[1].isdigit() and new[1].isdigit()
            and int(new[0]) == int(old[0]) and int(new[1]) > int(old[1])):
        return "minor"
    return None


def display(name, version):
    """What the issue title shows: the major for everything, major.minor for openssl (its ABI moves within a major)."""
    parts = components(version)
    return ".".join(parts[:2]) if name.endswith("-openssl") and len(parts) > 1 else parts[0]


def alert_title(bumps):
    pick = next((b for b in bumps if b["name"].endswith("-openssl")), bumps[0])
    return f"工具链有大版本更新：{pick['name'][len(PREFIX):]} {display(pick['name'], pick['pinned'])} → {display(pick['name'], pick['newest'])}"


def full_name(name):
    return name if name.startswith(PREFIX) else PREFIX + name


def resolve_newest(simulate):
    """name -> version for every simulated package (short or full names accepted)."""
    newest = {}
    if simulate:
        for pair in simulate.split(","):
            name, _, version = pair.strip().partition("=")
            if not version:
                raise SystemExit(f"::error::--simulate needs name=version pairs, got {pair!r}")
            newest[full_name(name)] = version.strip()
    return newest


def check(args):
    entries = [entry for path in args.pins for entry in load_pins(path)[1]]
    db = read_db(args.db)
    simulated = resolve_newest(args.simulate)
    bumps, missing = [], []
    table = sys.stderr if args.json else sys.stdout  # with --json the machine summary is the whole stdout
    for pin in entries:
        name = pin["name"]
        if name in simulated:
            newest = simulated[name]
        elif name in db:
            newest = db[name][0]
        else:
            missing.append(name)
            continue
        level = bump_level(name, pin["version"], newest)
        if level:
            bumps.append({"name": name, "short": name[len(PREFIX):], "pinned": pin["version"], "newest": newest, "level": level})
        else:
            print(f"{name} {pin['version']} -> {newest}", file=table)
    for bump in bumps:
        print(f"{bump['name']} {bump['pinned']} -> {bump['newest']} ({bump['level']})", file=table)
    for name in missing:
        print(f"warning: {name} is pinned but not in the repository database; the pin stays and no alert is made", file=sys.stderr)
    if args.json:
        print(json.dumps({"changed": bool(bumps), "simulated": bool(simulated), "checked": len(entries),
                          "bumps": bumps, "title": alert_title(bumps) if bumps else ""}, ensure_ascii=False))


def bump(args):
    db = read_db(args.db)
    for path in args.pins:
        bump_file(path, db)


def bump_file(path, db):
    header, entries = load_pins(path)
    lines = list(header)
    for pin in sorted(entries, key=lambda entry: entry["name"]):
        name = pin["name"]
        if name not in db:
            print(f"warning: {name} is not in the repository database; the pin is kept unchanged", file=sys.stderr)
            lines.append(f"{name} {pin['version']} {pin['file']} {pin['sha256']}")
            continue
        version, filename, sha256 = db[name]
        old, new = components(pin["version"]), components(version)
        if old and new and new[0].isdigit() and old[0].isdigit() and int(new[0]) < int(old[0]):
            print(f"warning: {name}: the repository carries {version}, older than the pinned {pin['version']} (rolled back?); taking it anyway", file=sys.stderr)
        note = "unchanged" if version == pin["version"] else f"{pin['version']} -> {version}"
        print(f"{name} {note}")
        lines.append(f"{name} {version} {filename} {sha256}")
    pathlib.Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{path}: {len(entries)} pins rewritten")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "bump"):
        subparser = sub.add_parser(name)
        subparser.add_argument("--pins", action="append", help="a pin file; repeat for several (default: msys2-pins.txt and msys2-pins-clang.txt)")
        subparser.add_argument("--db", help="read the repository database from this file instead of the servers")
    check_parser = sub.choices["check"]
    check_parser.add_argument("--simulate", default="", help="simulated repository versions, name=version, comma separated")
    check_parser.add_argument("--json", action="store_true", help="print a machine-readable summary instead of the table")
    args = parser.parse_args()
    if not args.pins:
        here = pathlib.Path(__file__).resolve().parent
        args.pins = [str(here / name) for name in ("msys2-pins.txt", "msys2-pins-clang.txt") if (here / name).is_file()]
    (check if args.command == "check" else bump)(args)


main()
