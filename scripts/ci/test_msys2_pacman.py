"""msys2-pacman.sh against a stub pacman/curl/sleep and a scratch MSYS2 tree: the repositories kept, the mirror order of every attempt,
the retry and its pauses, the pinned package set of msys2-pins.txt, the cache pruning and key
(run: python3 -m unittest discover -s scripts/ci -p 'test_*.py'). Skipped where bash is not available."""

import hashlib
import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import unittest

CI = pathlib.Path(__file__).resolve().parent

PACMAN_CONF = """[options]
HoldPkg     = pacman
CheckSpace
SigLevel    = Required DatabaseOptional

[msys]
Include = /etc/pacman.d/mirrorlist.msys

[mingw32]
Include = /etc/pacman.d/mirrorlist.mingw

[ucrt64]
Include = /etc/pacman.d/mirrorlist.mingw

[clangarm64]
Include = /etc/pacman.d/mirrorlist.mingw
"""
MINGW = """# See https://www.msys2.org/dev/mirrors
## Primary
Server = https://mirror.msys2.org/mingw/$repo/
Server = https://repo.msys2.org/mingw/$repo/
## Tier 1
Server = https://ftp2.osuosl.org/pub/msys2/mingw/$repo/
Server = https://mirror.yandex.ru/mirrors/msys2/mingw/$repo/
Server = https://mirror.accum.se/mirror/msys2.org/mingw/$repo/
Server = https://ftp.nluug.nl/pub/os/windows/msys2/builds/mingw/$repo/
Server = https://mirror.selfnet.de/msys2/mingw/$repo/
"""
MSYS = MINGW.replace("mingw/$repo", "msys/$arch")

# Fails with exit code $FAIL_CODE for the first $FAILS calls (counted in $COUNT), then succeeds. Every call is logged with the first
# server of the mingw list at that moment.
PACMAN = """#!/bin/bash
n=$(( $(cat "$COUNT" 2>/dev/null || echo 0) + 1 )); echo $n > "$COUNT"
echo "$* | $(grep -m1 '^Server' "$MSYS2_ROOT/etc/pacman.d/mirrorlist.mingw")" >> "$LOG"
if [[ "$1" == "-Q" ]]; then cat "$INSTALLED"; exit 0; fi
if (( n <= FAILS )); then touch "$MSYS2_ROOT/var/lib/pacman/db.lck"; exit "${FAIL_CODE:-1}"; fi
"""
SLEEP = '#!/bin/bash\necho "$1" >> "$SLEEPS"\n'
# Succeeds for the servers named in $CURL_OK, fails for the others; writes the file given with -o.
CURL = """#!/bin/bash
url="${@: -1}"; out=""; while [[ $# -gt 0 ]]; do [[ "$1" == "-o" ]] && out="$2"; shift; done
echo "$url" >> "$CURLLOG"
if [[ "$url" == *"$CURL_OK"* ]]; then echo pkg > "$out"; exit 0; fi
exit 22
"""


class Msys2PacmanTest(unittest.TestCase):
    def setUp(self):
        if shutil.which("bash") is None or shutil.which("awk") is None:
            self.skipTest("bash and awk are needed")
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.root = self.tmp / "msys64"
        (self.root / "etc" / "pacman.d").mkdir(parents=True)
        (self.root / "var" / "lib" / "pacman").mkdir(parents=True)
        self.cache = self.root / "var" / "cache" / "pacman" / "pkg"
        self.cache.mkdir(parents=True)
        (self.root / "etc" / "pacman.conf").write_text(PACMAN_CONF)
        (self.root / "etc" / "pacman.d" / "mirrorlist.mingw").write_text(MINGW)
        (self.root / "etc" / "pacman.d" / "mirrorlist.msys").write_text(MSYS)
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        for name, body in (("pacman", PACMAN), ("sleep", SLEEP), ("curl", CURL)):
            path = bin_dir / name
            path.write_text(body)
            path.chmod(path.stat().st_mode | stat.S_IEXEC)
        self.bin = bin_dir
        self.log, self.sleeps, self.curl_log = self.tmp / "log", self.tmp / "sleeps", self.tmp / "curl"
        self.installed = self.tmp / "installed"
        self.installed.write_text("")
        # The default pins file: one pinned package whose file the curl stub writes ("pkg\n"), so a plain install succeeds.
        self.pins = self.tmp / "msys2-pins.txt"
        self.pkg_digest = hashlib.sha256(b"pkg\n").hexdigest()
        self.write_pins(f"p-gcc 1.0-1 p-gcc-1.0-1-any.pkg.tar.zst {self.pkg_digest}")

    def write_pins(self, text):
        self.pins.write_text("# pins\n" + text + "\n")

    def run_script(self, *args, fails=0, fail_code=1, **extra):
        # TOOLCHAIN is set for the whole workflow (clang on release builds): a test names the compiler itself.
        env = dict({k: v for k, v in os.environ.items() if k != "TOOLCHAIN"}, PATH=f"{self.bin}:{os.environ['PATH']}", MSYS2_ROOT=str(self.root), LOG=str(self.log),
                   COUNT=str(self.tmp / "count"), FAILS=str(fails), FAIL_CODE=str(fail_code), SLEEPS=str(self.sleeps),
                   CURLLOG=str(self.curl_log), INSTALLED=str(self.installed), CURL_OK="repo.msys2.org")
        env["PINS_FILE"] = str(self.pins)
        env.update({k: str(v) for k, v in extra.items()})
        return subprocess.run(["bash", str(CI / "msys2-pacman.sh"), *args], env=env, capture_output=True, text=True)

    def read(self, path):
        return path.read_text().splitlines() if path.exists() else []

    def first_server(self, name="mirrorlist.mingw"):
        return next(line for line in (self.root / "etc" / "pacman.d" / name).read_text().splitlines() if line.startswith("Server"))

    def test_prepare_keeps_only_msys_and_ucrt64(self):
        result = self.run_script("prepare")
        self.assertEqual(result.returncode, 0, result.stderr)
        conf = (self.root / "etc" / "pacman.conf").read_text()
        self.assertIn("#CheckSpace", conf)
        self.assertIn("\n[msys]\nInclude", conf)
        self.assertIn("\n[ucrt64]\nInclude", conf)
        self.assertIn("#[mingw32]", conf)
        self.assertIn("#Include = /etc/pacman.d/mirrorlist.mingw", conf)
        self.assertIn("HoldPkg     = pacman", conf)
        self.assertIn("IgnorePkg = p-gcc", conf)
        self.assertEqual(sum(1 for line in conf.splitlines() if line.startswith("Include")), 2)
        # idempotent: the second run changes nothing
        self.run_script("prepare")
        self.assertEqual((self.root / "etc" / "pacman.conf").read_text(), conf)

    def test_repo_msys2_org_leads_the_first_attempt(self):
        self.run_script("prepare")
        self.assertEqual(self.first_server(), "Server = https://repo.msys2.org/mingw/$repo/")
        self.assertEqual(self.first_server("mirrorlist.msys"), "Server = https://repo.msys2.org/msys/$arch/")
        servers = [line for line in (self.root / "etc" / "pacman.d" / "mirrorlist.mingw").read_text().splitlines() if line.startswith("Server")]
        self.assertEqual(len(servers), 7)  # nothing is dropped
        self.assertEqual(servers[1], "Server = https://mirror.msys2.org/mingw/$repo/")

    def test_success_needs_one_attempt(self):
        result = self.run_script("upgrade")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.read(self.log)), 1)
        self.assertEqual(self.read(self.sleeps), [])

    def test_retry_with_growing_pauses_and_another_first_mirror_each_time(self):
        result = self.run_script("upgrade", fails=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.read(self.log)
        self.assertEqual(len(calls), 4)
        self.assertTrue(all(call.startswith("--noconfirm -Syuu --overwrite *") for call in calls))
        firsts = [call.split(" | ")[1] for call in calls]
        self.assertEqual(len(set(firsts)), 4, firsts)
        self.assertIn("repo.msys2.org", firsts[0])
        self.assertIn("mirror.msys2.org", firsts[1])
        self.assertEqual(self.read(self.sleeps), ["10", "20", "40"])
        self.assertFalse((self.root / "var" / "lib" / "pacman" / "db.lck").exists(), "a failed attempt leaves pacman's lock behind")

    def test_gives_up_after_the_attempts_with_the_exit_status(self):
        result = self.run_script("upgrade", fails=99, fail_code=7, PACMAN_ATTEMPTS=3)
        self.assertEqual(result.returncode, 7)
        self.assertEqual(len(self.read(self.log)), 3)
        self.assertIn("::error::pacman -Syuu failed after 3 attempts", result.stdout)

    def test_pause_stops_growing_at_a_minute(self):
        self.run_script("upgrade", fails=99, PACMAN_ATTEMPTS=7)
        self.assertEqual(self.read(self.sleeps), ["10", "20", "40", "60", "60", "60"])

    def test_install_retries_the_package_list_and_installs_the_pinned_set(self):
        result = self.run_script("install", "git", "p-gcc", fails=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.read(self.log)
        self.assertEqual(len(calls), 3)  # failed -S, second -S, then -U of the pinned set
        self.assertTrue(calls[1].startswith("--noconfirm --needed -S git"), calls[1])  # a pinned name never reaches -S
        self.assertNotIn("p-gcc", calls[1])
        self.assertIn("-U", calls[2])
        self.assertTrue(calls[2].split(" | ")[0].endswith("p-gcc-1.0-1-any.pkg.tar.zst"), calls[2])
        self.assertEqual(self.read(self.curl_log), ["https://repo.msys2.org/mingw/ucrt64/p-gcc-1.0-1-any.pkg.tar.zst"])

    def test_pinned_package_is_fetched_from_the_next_server_when_the_first_one_fails(self):
        self.run_script("prepare")
        # repo.msys2.org leads the list, so make it the failing one: only mirror.msys2.org serves the file
        self.write_pins(f"p-x 1-1 o-1-any.pkg.tar.zst {self.pkg_digest}")
        result = self.run_script("install", "git", CURL_OK="mirror.msys2.org")
        self.assertEqual(result.returncode, 0, result.stderr)
        urls = self.read(self.curl_log)
        self.assertEqual(urls[0], "https://repo.msys2.org/mingw/ucrt64/o-1-any.pkg.tar.zst")
        self.assertEqual(urls[1], "https://mirror.msys2.org/mingw/ucrt64/o-1-any.pkg.tar.zst")
        self.assertTrue((self.cache / "o-1-any.pkg.tar.zst").is_file())
        self.assertFalse((self.cache / "o-1-any.pkg.tar.zst.part").exists())

    def test_pinned_package_failing_everywhere_fails_the_step(self):
        self.write_pins(f"p-x 1-1 o-1-any.pkg.tar.zst {self.pkg_digest}")
        result = self.run_script("install", "git", CURL_OK="nowhere", PACMAN_ATTEMPTS=2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("::error::download of o-1-any.pkg.tar.zst failed after 2 attempts", result.stdout)

    def test_pinned_package_must_match_its_sha256(self):
        self.run_script("prepare")
        ok = self.run_script("install", "git", CURL_OK="repo.msys2.org")
        self.assertEqual(ok.returncode, 0, ok.stderr)
        (self.cache / "p-gcc-1.0-1-any.pkg.tar.zst").unlink()
        self.write_pins(f"p-gcc 1.0-1 p-gcc-1.0-1-any.pkg.tar.zst {'0' * 64}")
        bad = self.run_script("install", "git", CURL_OK="repo.msys2.org", PACMAN_ATTEMPTS=2)
        self.assertNotEqual(bad.returncode, 0)
        self.assertFalse((self.cache / "p-gcc-1.0-1-any.pkg.tar.zst").exists(), "a file with the wrong digest is never kept")

    def test_pinned_download_stops_at_the_time_of_the_attempt(self):
        self.run_script("prepare")
        self.write_pins(f"p-x 1-1 o-1-any.pkg.tar.zst {self.pkg_digest}")
        result = self.run_script("install", "git", CURL_OK="nowhere", PACMAN_ATTEMPTS=1, PACMAN_ATTEMPT_TIMEOUT=0)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.read(self.curl_log), [], "no server is tried once the attempt time is used up")

    def test_install_installs_every_pin_even_one_the_caller_did_not_name(self):
        result = self.run_script("install", "git")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.read(self.log)
        self.assertEqual(len(calls), 2)  # -S git, then -U of the pinned set
        self.assertTrue(calls[0].startswith("--noconfirm --needed -S git"))
        self.assertIn("-U", calls[1])

    def test_unpinned_mode_installs_the_newest_and_prepares_no_ignore_list(self):
        self.run_script("prepare", TOOLCHAIN_UNPINNED=1)
        self.assertNotIn("IgnorePkg", (self.root / "etc" / "pacman.conf").read_text())
        result = self.run_script("install", "git", "p-gcc", TOOLCHAIN_UNPINNED=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.read(self.log)
        self.assertEqual(len(calls), 1)  # one -S with every name; no -U, no pinned download
        self.assertTrue(calls[0].startswith("--noconfirm --needed -S git p-gcc"), calls[0])
        self.assertEqual(self.read(self.curl_log), [])

    def test_pinned_mode_fails_without_the_pins_file(self):
        result = self.run_script("prepare", PINS_FILE=str(self.tmp / "gone.txt"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("::error::the toolchain is pinned", result.stdout)

    def write_clang_pins(self):
        self.clang_pins = self.tmp / "msys2-pins-clang.txt"
        self.clang_pins.write_text(f"# clang pins\np-clang 1.0-1 p-clang-1.0-1-any.pkg.tar.zst {self.pkg_digest}\n")

    def test_a_clang_build_pins_and_installs_the_clang_packages_too(self):
        self.write_clang_pins()
        result = self.run_script("prepare", TOOLCHAIN="clang", CLANG_PINS_FILE=str(self.clang_pins))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("IgnorePkg = p-gcc p-clang", (self.root / "etc" / "pacman.conf").read_text())
        result = self.run_script("install", "git", "p-clang", TOOLCHAIN="clang", CLANG_PINS_FILE=str(self.clang_pins))
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.read(self.log)
        self.assertTrue(calls[-1].startswith("--noconfirm -U"), calls[-1])
        self.assertIn("p-clang-1.0-1-any.pkg.tar.zst", calls[-1])
        self.assertIn("p-gcc-1.0-1-any.pkg.tar.zst", calls[-1])
        self.assertNotIn("p-clang", " ".join(call for call in calls if " -S " in call), "a pinned name is never passed to -S")

    def test_a_gcc_build_ignores_the_clang_pins(self):
        self.write_clang_pins()
        self.run_script("prepare", CLANG_PINS_FILE=str(self.clang_pins))
        conf = (self.root / "etc" / "pacman.conf").read_text()
        self.assertIn("IgnorePkg = p-gcc\n", conf)
        self.assertNotIn("p-clang", conf)
        result = self.run_script("install", "git", CLANG_PINS_FILE=str(self.clang_pins))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("p-clang", self.read(self.log)[-1])

    def test_a_clang_build_fails_without_the_clang_pins_file(self):
        result = self.run_script("prepare", TOOLCHAIN="clang", CLANG_PINS_FILE=str(self.tmp / "gone-clang.txt"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("::error::the Clang toolchain is pinned", result.stdout)

    def test_prune_keeps_installed_versions_and_signatures_only(self):
        self.installed.write_text("gcc 16.1.0-1\nopus 1.6.1-1\nlib 1:2.0-3\n")
        keep = ["gcc-16.1.0-1-x86_64.pkg.tar.zst", "gcc-16.1.0-1-x86_64.pkg.tar.zst.sig", "opus-1.6.1-1-any.pkg.tar.zst", "lib-1~2.0-3-any.pkg.tar.zst"]
        drop = ["gcc-15.2.0-1-x86_64.pkg.tar.zst", "opus-1.7.0-1-any.pkg.tar.zst", "removed-1-1-any.pkg.tar.zst", "gcc-libs-16.1.0-1-x86_64.pkg.tar.zst"]
        for name in keep + drop:
            (self.cache / name).write_text("x")
        result = self.run_script("prune")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(p.name for p in self.cache.iterdir()), sorted(keep))

    def test_cache_key_follows_the_file_list_and_the_toolchain_mode(self):
        (self.cache / "a-1-1-any.pkg.tar.zst").write_text("x")
        first = self.run_script("cache-key").stdout.strip()
        again = self.run_script("cache-key").stdout.strip()
        self.assertRegex(first, r"^pinned-[0-9a-f]{16}$")
        self.assertEqual(first, again)
        (self.cache / "b-2-1-any.pkg.tar.zst").write_text("x")
        self.assertNotEqual(first, self.run_script("cache-key").stdout.strip())
        self.assertRegex(self.run_script("cache-key", TOOLCHAIN_UNPINNED=1).stdout.strip(), r"^newest-[0-9a-f]{16}$")


if __name__ == "__main__":
    unittest.main()
