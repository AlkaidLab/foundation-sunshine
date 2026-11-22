"""config-fingerprint.py and hot-headers.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import pathlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

CI = pathlib.Path(__file__).resolve().parent
REPO = CI.parents[1]
SCRIPT = CI / "config-fingerprint.py"
HOT = CI / "hot-headers.py"


class FingerprintTest(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        for rel in ("CMakeLists.txt", "cmake/compile_definitions/windows.cmake", "cmake/compile_definitions/linux.cmake",
                    "cmake/targets/common.cmake", "cmake/packaging/windows.cmake", "cmake/sources/host.cmake",
                    ".github/actions/windows-toolchain/action.yml", "scripts/ci/msys2-pins.txt", "src/config.h", "docs/ci.md"):
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"# {rel}\n")
        (self.root / "cmake" / "compile_definitions" / "windows.cmake").write_text(
            'add_compile_definitions(SUNSHINE_PLATFORM="windows")\n'
            "file(GLOB NVPREFS_FILES CONFIGURE_DEPENDS\n"
            '        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.cpp")\n'
            "set(PLATFORM_TARGET_FILES\n"
            '        "${CMAKE_SOURCE_DIR}/src/platform/windows/one.cpp"\n'
            '        "${CMAKE_SOURCE_DIR}/src/platform/windows/two.cpp"\n'
            '        "${CMAKE_SOURCE_DIR}/src/platform/windows/three.cpp"  # tray (and friends\n'
            ')\n'
            "list(APPEND PLATFORM_TARGET_FILES\n"
            '        "${CMAKE_SOURCE_DIR}/third-party/tray/src/tray_windows.c")\n')
        (self.root / "scripts" / "ci" / "msys2-pins.txt").write_text(
            "# pins\nmingw-w64-ucrt-x86_64-gcc 16.2.0-7 mingw-w64-ucrt-x86_64-gcc-16.2.0-7-any.pkg.tar.zst " + "a" * 64 + "\n"
            "mingw-w64-ucrt-x86_64-openssl 4.1.0-1 mingw-w64-ucrt-x86_64-openssl-4.1.0-1-any.pkg.tar.zst " + "b" * 64 + "\n")
        workflow = self.root / ".github" / "workflows" / "main.yml"
        workflow.parent.mkdir(parents=True, exist_ok=True)
        self.workflow_text = (
            "jobs:\n  build_win:\n    steps:\n      - name: Configure Windows\n        env:\n          CCACHE_MAXSIZE: 2G\n"
            "        run: |\n          cmake -B build -DBUILD_TESTS=ON\n")
        workflow.write_text(self.workflow_text)

    def fingerprint(self, toolchain=None):
        env = {key: value for key, value in os.environ.items() if key not in ("TOOLCHAIN", "CI_TOOLCHAIN")}
        if toolchain:
            env["TOOLCHAIN"] = toolchain
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root)], capture_output=True, text=True, env=env)
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

    def test_a_source_list_edit_keeps_it(self):
        # A ccache entry is keyed on the compile command line (ccache manual, "How ccache works"), so a
        # translation unit added or removed never invalidates another unit's entry; the lists that
        # enumerate them must not move the fingerprint. Two source-list-only edits forced 8-shard cold
        # builds on 2026-11-21 while no compile command line had changed.
        before = self.fingerprint()
        path = self.root / "cmake/compile_definitions/windows.cmake"
        old = path.read_text()
        added = old.replace("one.cpp", 'one.cpp"\n        "${CMAKE_SOURCE_DIR}/src/platform/windows/added.cpp')
        self.assertNotEqual(old, added, "the addition case must actually change the file")
        path.write_text(added)
        self.assertEqual(before, self.fingerprint(), "a file added to the platform target list")
        appended = old + 'list(APPEND PLATFORM_TARGET_FILES "${CMAKE_SOURCE_DIR}/src/platform/windows/more.cpp")\n'
        self.assertNotEqual(old, appended, "the append case must actually change the file")
        path.write_text(appended)
        self.assertEqual(before, self.fingerprint(), "another list(APPEND) of the platform target list")
        removed = old.replace('        "${CMAKE_SOURCE_DIR}/src/platform/windows/two.cpp"\n', "")
        self.assertNotEqual(old, removed, "the deletion case must actually delete the source file line")
        path.write_text(removed)
        self.assertEqual(before, self.fingerprint(), "a file removed from the platform target list")
        targets = self.root / "cmake/targets/common.cmake"
        targets_old = targets.read_text()
        targets.write_text(targets_old + 'set(TEST_HELPER_SOURCES "${CMAKE_SOURCE_DIR}/tests/helper.cpp")\n')
        self.assertEqual(before, self.fingerprint(), "a source-list variable in another hashed module")
        targets.write_text(targets_old)
        path.write_text(old)
        self.assertEqual(before, self.fingerprint())

    def test_a_glob_pattern_or_a_compile_option_list_still_moves_it(self):
        # What selects or flags translation units stays hashed; only the enumerations are cut.
        before = self.fingerprint()
        path = self.root / "cmake/compile_definitions/windows.cmake"
        old = path.read_text()
        path.write_text(old.replace("nvprefs/*.cpp", "nvprefs/**/*.cpp"))
        self.assertNotEqual(before, self.fingerprint(), "a file(GLOB) pattern")
        path.write_text(old + "list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-probe)\n")
        self.assertNotEqual(before, self.fingerprint(), "a compile option")
        path.write_text(old)
        self.assertEqual(before, self.fingerprint())

    def test_parens_in_comments_quotes_and_bracket_arguments_stay_inside_the_statement(self):
        # cmake-language(7), Grammar: a hash runs to the end of the line, a quoted argument may
        # hold unbalanced parentheses and a bracket argument [=*[ ... ]=*] ends at its own closing
        # bracket. The fixture closes its set() on the line after such a comment; the cut must end
        # at that parenthesis, so the flag line below still moves the fingerprint (an over-eager
        # cut would swallow the rest of the file and hide it). Source-list edits still keep it.
        before = self.fingerprint()
        path = self.root / "cmake/compile_definitions/windows.cmake"
        old = path.read_text()
        self.assertIn("# tray (and friends" + chr(10) + ")", old)
        path.write_text(old.replace("one.cpp", "one_renamed.cpp"))
        self.assertEqual(before, self.fingerprint(), "a source-list edit inside a commented statement")
        path.write_text(old + "list(APPEND PLATFORM_TARGET_FILES [[src (paren).cpp]])" + chr(10))
        self.assertEqual(before, self.fingerprint(), "a bracket argument containing a parenthesis")
        path.write_text(old + 'set(QUOTED_SOURCES "src (weird) name.cpp")\n')
        self.assertEqual(before, self.fingerprint(), "a quoted argument containing a parenthesis")
        flagged = old.replace('add_compile_definitions(SUNSHINE_PLATFORM="windows")',
                               'add_compile_definitions(SUNSHINE_PLATFORM="windows" SUNSHINE_PROBE=1)')
        self.assertNotEqual(old, flagged, "the flag edit must change the fixture")
        path.write_text(flagged)
        self.assertNotEqual(before, self.fingerprint(), "a flag change after a commented parenthesis")
        path.write_text(old)
        self.assertEqual(before, self.fingerprint())

    def test_the_configure_step_the_toolchain_action_and_the_top_level_file_move_it(self):
        before = self.fingerprint()
        (self.root / ".github/workflows/main.yml").write_text(self.workflow_text.replace("ON", "OFF"))
        changed_configure = self.fingerprint()
        self.assertNotEqual(before, changed_configure)
        (self.root / ".github/workflows/main.yml").write_text(self.workflow_text)
        for rel in (".github/actions/windows-toolchain/action.yml", "scripts/ci/msys2-pins.txt", "CMakeLists.txt", "cmake/targets/common.cmake"):
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

    def test_a_pin_change_moves_it(self):
        before = self.fingerprint()
        path = self.root / "scripts" / "ci" / "msys2-pins.txt"
        old = path.read_text()
        path.write_text(old.replace("16.2.0-7", "17.1.0-1").replace("gcc-16.2.0-7", "gcc-17.1.0-1"))
        self.assertNotEqual(before, self.fingerprint())
        path.write_text(old)
        self.assertEqual(before, self.fingerprint())

    def test_the_compiler_and_the_clang_pins_move_it_only_for_clang(self):
        clang_pins = self.root / "scripts" / "ci" / "msys2-pins-clang.txt"
        clang_pins.write_text("# clang pins\nmingw-w64-ucrt-x86_64-clang 23.1.5-1 mingw-w64-ucrt-x86_64-clang-23.1.5-1-any.pkg.tar.zst " + "c" * 64 + "\n")
        gcc, clang = self.fingerprint("gcc"), self.fingerprint("clang")
        self.assertEqual(gcc, self.fingerprint(), "gcc is the default")
        self.assertNotEqual(gcc, clang)
        clang_pins.write_text(clang_pins.read_text().replace("23.1.5", "24.1.0"))
        self.assertEqual(gcc, self.fingerprint("gcc"), "the clang pins are not part of a gcc build")
        self.assertNotEqual(clang, self.fingerprint("clang"))

    def test_an_unknown_compiler_is_refused(self):
        env = dict(os.environ, TOOLCHAIN="msvc")
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root)], capture_output=True, text=True, env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported Windows toolchain", result.stderr)

    def test_the_repository_database_is_not_read(self):
        # The pins decide the cache key, not what the repository carries today: a newer upstream package must not
        # invalidate the cache while the build still installs the pinned file.
        env = {key: value for key, value in os.environ.items() if key not in ("TOOLCHAIN", "CI_TOOLCHAIN")}
        env["MSYS2_DB_URLS"] = "http://127.0.0.1:1/ucrt64.db"
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root)], capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), self.fingerprint())

    def test_the_real_repository_has_a_fingerprint(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(REPO), "--details"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("cmake/compile_definitions/windows.cmake", result.stderr)
        self.assertIn("scripts/ci/msys2-pins.txt", result.stderr)
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
