"""config-fingerprint.py and hot-headers.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import gzip
import io
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

CI = pathlib.Path(__file__).resolve().parent
REPO = CI.parents[1]
SCRIPT = CI / "config-fingerprint.py"
HOT = CI / "hot-headers.py"


def fake_db(path, versions):
    with tarfile.open(path, "w:gz") as tar:
        for name, version in versions.items():
            data = f"%NAME%\nmingw-w64-ucrt-x86_64-{name}\n\n%VERSION%\n{version}\n".encode()
            info = tarfile.TarInfo(f"mingw-w64-ucrt-x86_64-{name}-{version}/desc")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))


class FingerprintTest(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        for rel in ("CMakeLists.txt", "cmake/compile_definitions/windows.cmake", "cmake/compile_definitions/linux.cmake",
                    "cmake/targets/common.cmake", "cmake/packaging/windows.cmake", "cmake/sources/host.cmake",
                    ".github/actions/windows-toolchain/action.yml", "src/config.h", "docs/ci.md"):
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"# {rel}\n")
        workflow = self.root / ".github" / "workflows" / "main.yml"
        workflow.parent.mkdir(parents=True, exist_ok=True)
        self.workflow_text = (
            "jobs:\n  build_win:\n    steps:\n      - name: Configure Windows\n        env:\n          CCACHE_MAXSIZE: 2G\n"
            "        run: |\n          cmake -B build -DBUILD_TESTS=ON\n")
        workflow.write_text(self.workflow_text)
        self.db = self.root / "ucrt64.db"
        self.versions = {"gcc": "16.2.0-7", "crt-git": "14.0.0-1", "headers-git": "14.0.0-1", "openssl": "3.6.5-1", "opus": "1.6.1-1"}
        fake_db(self.db, self.versions)

    def fingerprint(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), "--msys2-db", str(self.db)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def test_it_is_twelve_hex_digits_and_stable(self):
        first = self.fingerprint()
        self.assertRegex(first, r"^[0-9a-f]{12}$")
        self.assertEqual(first, self.fingerprint())

    def test_a_flag_change_moves_it(self):
        before = self.fingerprint()
        (self.root / "cmake/compile_definitions/windows.cmake").write_text("add_compile_options(-g1)\n")
        self.assertNotEqual(before, self.fingerprint())

    def test_the_configure_step_the_toolchain_action_and_the_top_level_file_move_it(self):
        before = self.fingerprint()
        (self.root / ".github/workflows/main.yml").write_text(self.workflow_text.replace("ON", "OFF"))
        changed_configure = self.fingerprint()
        self.assertNotEqual(before, changed_configure)
        (self.root / ".github/workflows/main.yml").write_text(self.workflow_text)
        for rel in (".github/actions/windows-toolchain/action.yml", "CMakeLists.txt", "cmake/targets/common.cmake"):
            path = self.root / rel
            old = path.read_text()
            path.write_text(old + "# changed\n")
            self.assertNotEqual(before, self.fingerprint(), rel)
            path.write_text(old)
        self.assertEqual(before, self.fingerprint())

    def test_what_does_not_touch_a_windows_compile_line_does_not_move_it(self):
        before = self.fingerprint()
        for rel in ("cmake/packaging/windows.cmake", "cmake/sources/host.cmake", "cmake/compile_definitions/linux.cmake",
                    "src/config.h", "docs/ci.md"):
            (self.root / rel).write_text("# something else\n")
        self.assertEqual(before, self.fingerprint())

    def test_a_package_version_moves_it(self):
        before = self.fingerprint()
        fake_db(self.db, dict(self.versions, gcc="16.3.0-1"))
        self.assertNotEqual(before, self.fingerprint())
        fake_db(self.db, self.versions)
        self.assertEqual(before, self.fingerprint())

    def test_only_the_installed_headers_package_moving_moves_it(self):
        before = self.fingerprint()
        fake_db(self.db, dict(self.versions, **{"headers-git": "14.0.1-1"}))
        self.assertNotEqual(before, self.fingerprint())

    def test_a_newer_opus_in_the_repository_does_not_move_it(self):
        before = self.fingerprint()
        fake_db(self.db, dict(self.versions, opus="1.7.0-1"))
        self.assertEqual(before, self.fingerprint())

    def test_an_unreachable_repository_still_yields_a_fingerprint(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), "--msys2-db", str(self.root / "missing.db")],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout.strip(), r"^[0-9a-f]{12}$")
        self.assertIn("unavailable", result.stderr)

    def test_the_real_repository_has_a_fingerprint(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(REPO), "--no-packages", "--details"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("cmake/compile_definitions/windows.cmake", result.stderr)
        self.assertNotIn("cmake/packaging/", result.stderr)
        self.assertNotIn("cmake/sources/", result.stderr)


class HotHeadersTest(unittest.TestCase):
    def run_hot(self, *args, stdin=""):
        return subprocess.run([sys.executable, str(HOT), *args], input=stdin, capture_output=True, text=True)

    def test_generate_weighs_the_headers_by_the_compile_time_of_the_object_files_that_include_them(self):
        # slow.obj takes 90 of 100 s and includes heavy.h; light.h is in nine quick object files (a count of 90%, a time share of 9%)
        deps = ("d/slow.obj: #deps 2, deps mtime 1 (VALID)\n    /w/src/src/heavy.h\n    /w/src/third-party/x/y.h\n\n")
        weights = "# ninja log v5\n0\t90000\t0\td/slow.obj\t0\n"
        for i in range(9):
            deps += f"d/q{i}.obj: #deps 1\n    /w/src/src/light.h\n    /w/src/src/a.cpp\n\n"
            weights += f"0\t1111\t0\td/q{i}.obj\t0\n"
        deps += "libx.a: #deps 1\n    /w/src/src/heavy.h\n"
        import os
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".ninja_log", delete=False) as handle:
            handle.write(weights)
        self.addCleanup(os.unlink, handle.name)
        result = self.run_hot("generate", "/w/src", handle.name, "30", stdin=deps)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [line for line in result.stdout.splitlines() if not line.startswith("#")]
        self.assertEqual(lines, ["src/heavy.h\t90"])

    def test_check_names_the_hot_headers_among_the_changed_files(self):
        listing = CI / "hot-headers.txt"
        self.assertTrue(listing.exists())
        self.assertIn("src/thread_safe.h", listing.read_text())
        first = next(line.split("\t")[0] for line in listing.read_text().splitlines() if line and not line.startswith("#"))
        hit = self.run_hot("check", str(listing), stdin=f"docs/x.md\n{first}\n")
        self.assertEqual(hit.returncode, 0)
        self.assertIn(first, hit.stdout)
        miss = self.run_hot("check", str(listing), stdin="docs/x.md\nsrc/some_rarely_included.cpp\n")
        self.assertEqual(miss.returncode, 1)
        self.assertEqual(miss.stdout.strip(), "")
        unknown = self.run_hot("check", str(listing), stdin="")
        self.assertEqual(unknown.returncode, 1)


if __name__ == "__main__":
    unittest.main()
