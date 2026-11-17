"""msys2-pins.py against a fake repository database (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import io
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest

CI = pathlib.Path(__file__).resolve().parent
SCRIPT = CI / "msys2-pins.py"
PREFIX = "mingw-w64-ucrt-x86_64-"


def fake_db(path, packages):
    """A gzip-compressed repository database: name -> (version, filename, sha256)."""
    with tarfile.open(path, "w:gz") as tar:
        for name, (version, filename, sha256) in packages.items():
            data = (f"%NAME%\n{name}\n\n%VERSION%\n{version}\n\n%FILENAME%\n{filename}\n\n"
                    f"%SHA256SUM%\n{sha256}\n").encode()
            info = tarfile.TarInfo(f"{name}-{version}/desc")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))


class PinsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, self.tmp, ignore_errors=True)
        self.pins = self.tmp / "msys2-pins.txt"
        self.pins.write_text(
            "# pins header\n"
            + f"{PREFIX}cmake 4.5.0-1 {PREFIX}cmake-4.5.0-1-any.pkg.tar.zst " + "1" * 64 + "\n"
            + f"{PREFIX}gcc 16.2.0-7 {PREFIX}gcc-16.2.0-7-any.pkg.tar.zst " + "2" * 64 + "\n"
            + f"{PREFIX}openssl 4.1.0-1 {PREFIX}openssl-4.1.0-1-any.pkg.tar.zst " + "3" * 64 + "\n"
            + f"{PREFIX}opus 1.6.1-1 {PREFIX}opus-1.6.1-1-any.pkg.tar.zst " + "4" * 64 + "\n")
        self.db = self.tmp / "ucrt64.db"

    def run_pins(self, *args, env=None):
        environment = dict(os.environ, **(env or {}))
        return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True, env=environment)

    def check_json(self, *args):
        result = self.run_pins("check", "--pins", str(self.pins), "--db", str(self.db), "--json", *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 1, "the JSON summary must be the whole stdout (the workflow pipes it into jq)")
        return json.loads(result.stdout)

    def test_reports_a_newer_major_and_the_openssl_minor_but_nothing_else(self):
        fake_db(self.db, {
            PREFIX + "cmake": ("4.5.1-2", PREFIX + "cmake-4.5.1-2-any.pkg.tar.zst", "5" * 64),   # patch: no alert
            PREFIX + "gcc": ("16.2.0-9", PREFIX + "gcc-16.2.0-9-any.pkg.tar.zst", "6" * 64),     # pkgrel: no alert
            PREFIX + "openssl": ("4.2.0-1", PREFIX + "openssl-4.2.0-1-any.pkg.tar.zst", "7" * 64),  # minor: alert
            PREFIX + "opus": ("1.6.1-2", PREFIX + "opus-1.6.1-2-any.pkg.tar.zst", "8" * 64),     # pkgrel: no alert
        })
        result = self.check_json()
        self.assertTrue(result["changed"])
        self.assertEqual([bump["short"] for bump in result["bumps"]], ["openssl"])
        self.assertEqual(result["bumps"][0]["level"], "minor")
        self.assertEqual(result["title"], "工具链有大版本更新：openssl 4.1 → 4.2")

    def test_the_title_names_the_major_when_openssl_did_not_move(self):
        fake_db(self.db, {
            PREFIX + "cmake": ("4.5.0-1", PREFIX + "cmake-4.5.0-1-any.pkg.tar.zst", "1" * 64),
            PREFIX + "gcc": ("17.1.0-1", PREFIX + "gcc-17.1.0-1-any.pkg.tar.zst", "2" * 64),
            PREFIX + "openssl": ("4.1.0-1", PREFIX + "openssl-4.1.0-1-any.pkg.tar.zst", "3" * 64),
            PREFIX + "opus": ("1.6.1-1", PREFIX + "opus-1.6.1-1-any.pkg.tar.zst", "4" * 64),
        })
        result = self.check_json()
        self.assertEqual(result["title"], "工具链有大版本更新：gcc 16 → 17")
        self.assertEqual(result["bumps"][0]["level"], "major")

    def test_a_missing_package_is_a_warning_and_an_epoch_version_is_compared_by_its_major(self):
        fake_db(self.db, {
            PREFIX + "cmake": ("4.5.0-1", PREFIX + "cmake-4.5.0-1-any.pkg.tar.zst", "1" * 64),
            PREFIX + "openssl": ("4.1.0-1", PREFIX + "openssl-4.1.0-1-any.pkg.tar.zst", "3" * 64),
            PREFIX + "opus": ("1.6.1-1", PREFIX + "opus-1.6.1-1-any.pkg.tar.zst", "4" * 64),
        })
        result = self.check_json()
        self.assertFalse(result["changed"])
        self.assertEqual(result["title"], "")
        # pkgconf carries an epoch (1~3.0.11-2): its major is 3, not 1~3
        self.pins.write_text(
            "# pins header\n"
            + f"{PREFIX}pkgconf 1~3.0.11-2 {PREFIX}pkgconf-1~3.0.11-2-any.pkg.tar.zst " + "9" * 64 + "\n")
        fake_db(self.db, {PREFIX + "pkgconf": ("1~3.1.0-1", PREFIX + "pkgconf-1~3.1.0-1-any.pkg.tar.zst", "8" * 64)})
        self.assertFalse(self.check_json()["changed"])
        fake_db(self.db, {PREFIX + "pkgconf": ("1~4.0.0-1", PREFIX + "pkgconf-1~4.0.0-1-any.pkg.tar.zst", "8" * 64)})
        self.assertTrue(self.check_json()["changed"])

    def test_simulate_overrides_the_repository_and_marks_the_result(self):
        fake_db(self.db, {})
        result = self.check_json("--simulate", "openssl=4.2.0-1,gcc=17.1.0-1")
        self.assertTrue(result["simulated"])
        self.assertEqual(sorted(bump["short"] for bump in result["bumps"]), ["gcc", "openssl"])
        self.assertEqual(result["title"], "工具链有大版本更新：openssl 4.1 → 4.2")

    def test_only_newer_major_or_openssl_minor_is_reported(self):
        fake_db(self.db, {
            PREFIX + "gcc": ("15.9.0-1", PREFIX + "gcc-15.9.0-1-any.pkg.tar.zst", "2" * 64),
            PREFIX + "openssl": ("4.10.0-1", PREFIX + "openssl-4.10.0-1-any.pkg.tar.zst", "7" * 64),
        })
        result = self.check_json()
        self.assertEqual([bump["short"] for bump in result["bumps"]], ["openssl"])
        self.pins.write_text(
            "# pins header\n"
            + f"{PREFIX}openssl 4.10.0-1 {PREFIX}openssl-4.10.0-1-any.pkg.tar.zst " + "7" * 64 + "\n")
        fake_db(self.db, {
            PREFIX + "openssl": ("4.9.0-1", PREFIX + "openssl-4.9.0-1-any.pkg.tar.zst", "8" * 64),
        })
        self.assertFalse(self.check_json()["changed"])

    def test_a_repository_that_cannot_be_read_fails_the_check(self):
        env = {"MSYS2_DB_URLS": "http://127.0.0.1:1/ucrt64.db", "MSYS2_PINS_RETRY_PAUSE": "0"}
        result = self.run_pins("check", "--pins", str(self.pins), env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("::error::", result.stderr + result.stdout)

    def test_bump_rewrites_versions_files_and_digests_and_keeps_the_header(self):
        fake_db(self.db, {
            PREFIX + "cmake": ("4.6.0-1", PREFIX + "cmake-4.6.0-1-any.pkg.tar.zst", "a" * 64),
            PREFIX + "gcc": ("16.2.0-7", PREFIX + "gcc-16.2.0-7-any.pkg.tar.zst", "2" * 64),  # unchanged
            PREFIX + "openssl": ("4.2.0-1", PREFIX + "openssl-4.2.0-1-any.pkg.tar.zst", "b" * 64),
        })  # opus and everything else are gone from the repository: kept unchanged
        result = self.run_pins("bump", "--pins", str(self.pins), "--db", str(self.db))
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.pins.read_text().splitlines()
        self.assertEqual(lines[0], "# pins header")
        entries = {line.split()[0]: line.split()[1:] for line in lines[1:]}
        self.assertEqual(sorted(entries), sorted([PREFIX + "cmake", PREFIX + "gcc", PREFIX + "openssl", PREFIX + "opus"]))
        self.assertEqual(entries[PREFIX + "cmake"], ["4.6.0-1", PREFIX + "cmake-4.6.0-1-any.pkg.tar.zst", "a" * 64])
        self.assertEqual(entries[PREFIX + "gcc"], ["16.2.0-7", PREFIX + "gcc-16.2.0-7-any.pkg.tar.zst", "2" * 64])
        self.assertEqual(entries[PREFIX + "opus"], ["1.6.1-1", PREFIX + "opus-1.6.1-1-any.pkg.tar.zst", "4" * 64])

    def test_check_and_bump_cover_every_pin_file_given(self):
        clang = self.tmp / "msys2-pins-clang.txt"
        clang.write_text("# clang pins\n" + f"{PREFIX}clang 23.1.5-1 {PREFIX}clang-23.1.5-1-any.pkg.tar.zst " + "9" * 64 + "\n")
        fake_db(self.db, {
            PREFIX + "gcc": ("16.2.0-7", PREFIX + "gcc-16.2.0-7-any.pkg.tar.zst", "2" * 64),
            PREFIX + "clang": ("24.1.0-1", PREFIX + "clang-24.1.0-1-any.pkg.tar.zst", "d" * 64),
        })
        checked = self.run_pins("check", "--pins", str(self.pins), "--pins", str(clang), "--db", str(self.db), "--json")
        self.assertEqual(checked.returncode, 0, checked.stderr)
        summary = json.loads(checked.stdout)
        self.assertEqual(summary["checked"], 5)
        self.assertEqual([bump["short"] for bump in summary["bumps"]], ["clang"])
        bumped = self.run_pins("bump", "--pins", str(self.pins), "--pins", str(clang), "--db", str(self.db))
        self.assertEqual(bumped.returncode, 0, bumped.stderr)
        self.assertEqual(clang.read_text().splitlines()[0], "# clang pins")
        self.assertEqual(clang.read_text().splitlines()[1].split()[1:], ["24.1.0-1", PREFIX + "clang-24.1.0-1-any.pkg.tar.zst", "d" * 64])


if __name__ == "__main__":
    unittest.main()
