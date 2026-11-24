"""publish-stable.sh against a fake gh (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("publish-stable.sh")
# FAKE_STATE: missing (no release of that tag), true / false (a release exists, FAKE_STATE is its isPrerelease), broken (the API fails)
FAKE_GH = """#!/usr/bin/env bash
echo "gh $*" >> "$FAKE_LOG"
case "$1 $2" in
  "release view")
    if [[ "$*" == *--json\\ tagName* ]]; then
      case "$FAKE_STATE" in
        missing) echo "release not found" >&2; exit 1 ;;
        broken) echo "HTTP 500" >&2; exit 1 ;;
        *) echo '{"tagName":"v1"}' ;;
      esac
    elif [[ "$*" == *--json\\ isPrerelease* ]]; then
      echo "${FAKE_CREATED:-false}"
    else
      echo "https://example.invalid/release"
    fi ;;
esac
"""


@unittest.skipUnless(shutil.which("bash"), "bash is required")
class PublishStableTest(unittest.TestCase):
    def run_script(self, state, created="false", token="bot-token"):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "bin").mkdir()
            gh = tmp / "bin" / "gh"
            gh.write_text(FAKE_GH, encoding="utf-8")
            gh.chmod(0o755)
            (tmp / "dist").mkdir()
            (tmp / "dist" / "a.zip").write_text("x")
            env = dict(os.environ, PATH=f"{tmp / 'bin'}:{os.environ['PATH']}", FAKE_LOG=str(tmp / "log"), FAKE_STATE=state, FAKE_CREATED=created, GH_TOKEN=token,
                       TAG="v1.杂鱼", COMMIT="a" * 40, GITHUB_REPOSITORY="o/r", BODY="body")
            result = subprocess.run(["bash", str(SCRIPT), str(tmp / "dist")], env=env, capture_output=True, text=True)
            log = (tmp / "log").read_text(encoding="utf-8") if (tmp / "log").exists() else ""
            return result, log

    def test_the_release_is_created_with_the_bot_token_and_never_without_one(self):
        # the workflow token starts no other workflow, so the signing workflow would not run for the stable release
        result, log = self.run_script("missing", token="")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GH_TOKEN is empty", result.stdout)
        self.assertEqual(log, "")

    def test_a_missing_release_is_created_as_the_latest_full_release(self):
        result, log = self.run_script("missing")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gh release create v1.杂鱼", log)
        self.assertIn("--latest", log)
        self.assertNotIn("--prerelease", log)
        self.assertIn("--target " + "a" * 40, log)

    def test_a_pre_release_of_the_same_tag_is_never_replaced_or_promoted(self):
        result, log = self.run_script("true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already exists", result.stdout)
        for forbidden in ("release create", "release upload", "release edit"):
            self.assertNotIn(forbidden, log)

    def test_a_published_release_is_never_touched(self):
        result, log = self.run_script("false")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("release create", log)
        self.assertNotIn("release upload", log)

    def test_an_unreadable_release_is_an_error_and_not_a_missing_one(self):
        result, log = self.run_script("broken")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("could not read", result.stdout)
        self.assertNotIn("release create", log)

    def test_a_release_that_came_out_as_a_pre_release_fails_the_run(self):
        result, _ = self.run_script("missing", created="true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("created as a pre-release", result.stdout)
