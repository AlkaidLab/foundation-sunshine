"""publish-prerelease.sh against a fake gh (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("publish-prerelease.sh")
FAKE_GH = """#!/usr/bin/env bash
echo "gh $*" >> "$FAKE_LOG"
case "$1 $2" in
  "release view")
    if [[ "$*" == *--json\\ isPrerelease* ]]; then
      case "$FAKE_STATE" in
        missing) echo "release not found" >&2; exit 1 ;;
        broken) echo "HTTP 500" >&2; exit 1 ;;
        *) echo "$FAKE_STATE" ;;
      esac
    else
      echo "https://example.invalid/release"
    fi ;;
esac
"""


@unittest.skipUnless(shutil.which("bash"), "bash is required")
class PublishTest(unittest.TestCase):
    def run_script(self, state):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "bin").mkdir()
            gh = tmp / "bin" / "gh"
            gh.write_text(FAKE_GH, encoding="utf-8")
            gh.chmod(0o755)
            (tmp / "dist").mkdir()
            (tmp / "dist" / "a.zip").write_text("x")
            env = dict(os.environ, PATH=f"{tmp / 'bin'}:{os.environ['PATH']}", FAKE_LOG=str(tmp / "log"), FAKE_STATE=state,
                       TAG="v1.杂鱼", COMMIT="a" * 40, GITHUB_REPOSITORY="o/r", BODY="body")
            result = subprocess.run(["bash", str(SCRIPT), str(tmp / "dist")], env=env, capture_output=True, text=True)
            log = (tmp / "log").read_text(encoding="utf-8") if (tmp / "log").exists() else ""
            return result, log

    def test_a_missing_release_is_created_as_a_pre_release(self):
        result, log = self.run_script("missing")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gh release create v1.杂鱼", log)
        self.assertIn("--prerelease", log)

    def test_an_existing_pre_release_gets_its_assets_replaced(self):
        result, log = self.run_script("true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gh release upload v1.杂鱼", log)
        self.assertNotIn("gh release create", log)

    def test_a_full_release_is_never_touched(self):
        result, log = self.run_script("false")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a pre-release", result.stdout)
        self.assertNotIn("gh release upload", log)
        self.assertNotIn("gh release create", log)

    def test_an_unreadable_release_is_an_error_not_a_reason_to_create(self):
        result, log = self.run_script("broken")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("gh release create", log)


if __name__ == "__main__":
    unittest.main()
