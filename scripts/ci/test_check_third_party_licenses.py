#!/usr/bin/env python3
"""Tests for check-third-party-licenses.py."""

from __future__ import annotations

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).with_name("check-third-party-licenses.py")
SPEC = importlib.util.spec_from_file_location("check_third_party_licenses", SCRIPT)
CHECKER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(CHECKER)


def make_tree(destination: Path) -> None:
    shutil.copytree(ROOT / "third_party" / "licenses", destination / "third_party" / "licenses")
    shutil.copytree(ROOT / "cmake" / "packaging", destination / "cmake" / "packaging")
    shutil.copy2(ROOT / "NOTICE", destination / "NOTICE")


class CheckerTest(unittest.TestCase):
    def test_repository_manifest_passes(self) -> None:
        self.assertEqual(CHECKER.check(ROOT), [])

    def test_missing_license_file_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_tree(root)
            (root / "third_party/licenses/zlib/LICENSE").unlink()
            errors = CHECKER.check(root)
            self.assertTrue(any("zlib/LICENSE" in error for error in errors), errors)

    def test_changed_license_text_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_tree(root)
            license_file = root / "third_party/licenses/nefcon/LICENSE"
            license_file.write_text(license_file.read_text() + "\n", encoding="utf-8")
            errors = CHECKER.check(root)
            self.assertTrue(any("SHA-256" in error for error in errors), errors)

    def test_new_fetch_call_requires_manifest_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_tree(root)
            fetch = root / "cmake/packaging/FetchDriverDeps.cmake"
            fetch.write_text(fetch.read_text() + "\n_fetch_new_driver()\n", encoding="utf-8")
            errors = CHECKER.check(root)
            self.assertTrue(any("_fetch_new_driver" in error for error in errors), errors)

    def test_indented_fetch_call_requires_manifest_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_tree(root)
            fetch = root / "cmake/packaging/FetchDriverDeps.cmake"
            fetch.write_text(
                fetch.read_text() + "\nif(TRUE)\n  _fetch_new_driver()\nendif()\n",
                encoding="utf-8",
            )
            errors = CHECKER.check(root)
            self.assertTrue(any("_fetch_new_driver" in error for error in errors), errors)

    def test_version_pin_drift_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_tree(root)
            fetch = root / "cmake/packaging/FetchDriverDeps.cmake"
            fetch.write_text(
                fetch.read_text().replace(
                    'set(VIGEMBUS_VERSION "v1.22.0"', 'set(VIGEMBUS_VERSION "v1.22.1"', 1
                ),
                encoding="utf-8",
            )
            errors = CHECKER.check(root)
            self.assertTrue(any("VIGEMBUS_VERSION" in error for error in errors), errors)

    def test_effective_pin_override_fails(self) -> None:
        errors = CHECKER.check(ROOT, {"NEFCON_VERSION": "v1.16.0"})
        self.assertTrue(any("NEFCON_VERSION" in error for error in errors), errors)

    def test_reviewed_alternative_pin_passes(self) -> None:
        self.assertEqual(CHECKER.check(ROOT, {"NEFCON_VERSION": "v1.17.40"}), [])


if __name__ == "__main__":
    unittest.main()
