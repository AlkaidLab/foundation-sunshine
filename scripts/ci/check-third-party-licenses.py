#!/usr/bin/env python3
"""Check that every driver fetched for the Windows package carries a license.

usage: check-third-party-licenses.py [--root DIR]

The review record is ``third_party/licenses/manifest.json``. For every download
function invoked by ``cmake/packaging/FetchDriverDeps.cmake`` the manifest names
the upstream tag, the CMake version pins that select the asset, the repository
the release comes from, the vendored license file and its SHA-256. The check
fails when a fetch call has no entry, when a pin no longer matches the reviewed
version, when a license file is missing or changed, or when either installer
path stops shipping the licenses directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

FETCH = Path("cmake/packaging/FetchDriverDeps.cmake")
PACKAGING = Path("cmake/packaging/windows.cmake")
INNO = Path("cmake/packaging/sunshine.iss.in")
MANIFEST = Path("third_party/licenses/manifest.json")
NOTICE = Path("NOTICE")

FETCH_CALL = re.compile(r"(?m)^\s*(_fetch_[A-Za-z0-9_]+)\(\)\s*$")
CACHE_PIN = re.compile(r'(?m)^set\(([A-Za-z0-9_]+)\s+"([^"]*)"\s*\n?\s*CACHE\s+STRING')
PLAIN_SET = re.compile(r'(?m)^set\(([A-Za-z0-9_]+)\s+"([^"]*)"\s*\)')
RELEASE_REPOSITORY = re.compile(
    r"https://github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)/releases/download/"
)
REPOSITORY_VALUE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
VARIABLE = re.compile(r"\$\{([A-Za-z0-9_]+)\}")
REQUIRED_FIELDS = (
    "id",
    "name",
    "version",
    "license",
    "copyright",
    "upstream",
    "notice_url",
    "license_source",
    "source_key",
    "license_file",
    "license_sha256",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def check(root: Path, effective_pins: dict[str, str] | None = None) -> list[str]:
    errors: list[str] = []

    manifest_path = root / MANIFEST
    if not manifest_path.is_file():
        return [f"{MANIFEST}: missing"]
    try:
        manifest = json.loads(read(manifest_path))
    except json.JSONDecodeError as error:
        return [f"{MANIFEST}: invalid JSON: {error}"]

    components = manifest.get("components", [])
    runtime_libraries = manifest.get("runtime_libraries", [])
    if not components:
        return [f"{MANIFEST}: no components"]

    for entry in components + runtime_libraries:
        label = entry.get("id", "<unnamed>")
        for field in REQUIRED_FIELDS:
            if not entry.get(field):
                errors.append(f"{MANIFEST}: {label} has no {field}")
        license_file = entry.get("license_file")
        if not license_file:
            continue
        path = root / license_file
        if not path.is_file():
            errors.append(f"{license_file}: missing for {label}")
            continue
        if path.stat().st_size == 0:
            errors.append(f"{license_file}: empty for {label}")
        expected = entry.get("license_sha256", "")
        actual = digest(path)
        if actual != expected:
            errors.append(
                f"{license_file}: SHA-256 is {actual}, manifest records {expected}"
            )

    fetch_path = root / FETCH
    if not fetch_path.is_file():
        errors.append(f"{FETCH}: missing")
        return errors
    fetch_text = read(fetch_path)

    invoked = set(FETCH_CALL.findall(fetch_text))
    declared = {call for entry in components for call in entry.get("fetch_calls", [])}
    for call in sorted(invoked - declared):
        errors.append(f"{FETCH}: {call}() has no license entry in {MANIFEST}")
    for call in sorted(declared - invoked):
        errors.append(f"{MANIFEST}: {call}() is not invoked by {FETCH}")

    pins = dict(CACHE_PIN.findall(fetch_text))
    for name, value in PLAIN_SET.findall(fetch_text):
        if REPOSITORY_VALUE.match(value):
            pins.setdefault(name, value)
    if effective_pins:
        # The configure step passes the cache values it selected, including any
        # -D override. A source default is only a fallback for standalone runs.
        pins.update(effective_pins)

    for entry in components:
        label = entry["id"]
        for variable, expected in entry.get("cmake_variables", {}).items():
            accepted = [expected] + entry.get("cmake_alternatives", {}).get(variable, [])
            actual = pins.get(variable)
            if actual is None:
                errors.append(f"{FETCH}: {label} names {variable}, which is not pinned")
            elif actual not in accepted:
                errors.append(
                    f"{FETCH}: {variable} is {actual}, manifest records "
                    + " or ".join(accepted)
                )

    resolved = VARIABLE.sub(lambda match: pins.get(match.group(1), match.group(0)), fetch_text)
    repositories = set(RELEASE_REPOSITORY.findall(resolved))
    declared_repositories = {
        repository for entry in components for repository in entry.get("repositories", [])
    }
    for repository in sorted(repositories - declared_repositories):
        errors.append(
            f"{FETCH}: downloads from {repository}, which no manifest entry covers"
        )

    notice_path = root / NOTICE
    if not notice_path.is_file():
        errors.append(f"{NOTICE}: missing")
    else:
        notice = read(notice_path)
        for entry in components + runtime_libraries:
            label = entry["id"]
            if entry["name"] not in notice:
                errors.append(f"{NOTICE}: does not name {entry['name']} ({label})")
            for version in entry.get("notice_versions", []):
                if version not in notice:
                    errors.append(f"{NOTICE}: does not list {entry['name']} {version}")
            if entry["notice_url"] not in notice:
                errors.append(f"{NOTICE}: does not link {entry['notice_url']} ({label})")

    packaging = read(root / PACKAGING) if (root / PACKAGING).is_file() else ""
    license_directory = MANIFEST.parent.as_posix()
    if f'"${{CMAKE_SOURCE_DIR}}/{license_directory}/"' not in packaging:
        errors.append(f"{PACKAGING}: does not install {license_directory}/")
    if 'DESTINATION "licenses"' not in packaging:
        errors.append(f'{PACKAGING}: does not install to DESTINATION "licenses"')

    inno = read(root / INNO) if (root / INNO).is_file() else ""
    if r"\licenses\*" not in inno:
        errors.append(f"{INNO}: does not copy the licenses directory")
    if r"\NOTICE" not in inno:
        errors.append(f"{INNO}: does not copy NOTICE")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: the checkout containing this script)",
    )
    parser.add_argument(
        "--pin",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="effective CMake cache value that overrides the source default",
    )
    arguments = parser.parse_args()
    pins: dict[str, str] = {}
    for item in arguments.pin:
        if "=" not in item:
            parser.error(f"--pin expects NAME=VALUE, got {item!r}")
        name, value = item.split("=", 1)
        pins[name] = value
    root = arguments.root.resolve()
    errors = check(root, pins)
    if errors:
        for error in errors:
            print(f"::error::{error}")
        print(f"third-party licenses: {len(errors)} error(s)")
        return 1
    manifest = json.loads(read(root / MANIFEST))
    print(
        "third-party licenses: "
        f"{len(manifest['components'])} fetched drivers and "
        f"{len(manifest.get('runtime_libraries', []))} runtime libraries verified"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
