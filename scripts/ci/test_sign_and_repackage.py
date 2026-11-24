"""Rules of .github/workflows/sign-and-repackage.yml: signing must keep what kind of release it signs for (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

import yaml

WORKFLOW = pathlib.Path(__file__).resolve().parents[2] / ".github" / "workflows" / "sign-and-repackage.yml"
# FAKE_STATE: missing (no release of that tag), true / false (a release exists, FAKE_STATE is its isPrerelease), broken (the API fails)
FAKE_GH = """#!/usr/bin/env bash
echo "gh $*" >> "$FAKE_LOG"
if [[ "$1 $2" == "release view" ]]; then
  case "$FAKE_STATE" in
    missing) echo "release not found" >&2; exit 1 ;;
    broken) echo "HTTP 500" >&2; exit 1 ;;
    *) echo "$FAKE_STATE" ;;
  esac
fi
"""


def load():
    with open(WORKFLOW, encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def step(name):
    return next(s for s in load()["jobs"]["sign-and-repackage"]["steps"] if s.get("name") == name)


@unittest.skipUnless(shutil.which("bash"), "bash is required")
class SignReleaseStateTest(unittest.TestCase):
    def run_state(self, event, state="missing", event_prerelease=""):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "bin").mkdir()
            gh = tmp / "bin" / "gh"
            gh.write_text(FAKE_GH, encoding="utf-8")
            gh.chmod(0o755)
            output = tmp / "output"
            output.touch()
            env = dict(os.environ, PATH=f"{tmp / 'bin'}:{os.environ['PATH']}", FAKE_LOG=str(tmp / "log"), FAKE_STATE=state, GITHUB_OUTPUT=str(output),
                       GITHUB_REPOSITORY="o/r", EVENT=event, EVENT_PRERELEASE=event_prerelease, RELEASE_TAG="v1", GH_TOKEN="t")
            result = subprocess.run(["bash", "-c", step("Read the release state")["run"]], env=env, capture_output=True, text=True)
            values = dict(line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines() if "=" in line)
            log = (tmp / "log").read_text(encoding="utf-8") if (tmp / "log").exists() else ""
        return result, values, log

    def test_a_published_full_release_stays_a_full_release_and_is_built_as_stable(self):
        result, values, log = self.run_state("release", event_prerelease="false")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values, {"prerelease": "false", "channel": "stable"})
        self.assertEqual(log, "", "a release event is read from the event, not from the API")

    def test_a_manual_run_on_a_full_release_keeps_it_a_full_release(self):
        result, values, _ = self.run_state("workflow_dispatch", state="false")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values, {"prerelease": "false", "channel": "stable"})

    def test_a_manual_run_on_a_pre_release_or_a_tag_without_release_is_a_pre_release_as_before(self):
        for state in ("true", "missing"):
            result, values, _ = self.run_state("workflow_dispatch", state=state)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(values, {"prerelease": "true", "channel": "beta"}, state)

    def test_an_unreadable_release_stops_the_run_and_is_not_taken_for_a_missing_one(self):
        result, values, _ = self.run_state("workflow_dispatch", state="broken")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(values, {})

    def test_the_release_step_passes_the_state_and_the_build_records_the_channel(self):
        release = step("Create Release")["with"]
        self.assertEqual(release["prerelease"], "${{ steps.release_state.outputs.prerelease == 'true' }}")
        self.assertIs(release["draft"], True)
        self.assertIs(release["allowUpdates"], True)
        configure = step("Configure Windows")["env"]
        self.assertEqual(configure["SUNSHINE_BUILD_CHANNEL"], "${{ steps.release_state.outputs.channel }}")

    def test_the_automatic_trigger_is_kept(self):
        workflow = load()
        self.assertEqual(workflow[True]["release"]["types"], ["published"])
        condition = workflow["jobs"]["sign-and-repackage"]["if"]
        self.assertIn("github.event_name == 'release' && !github.event.release.prerelease", condition)

    def test_the_state_is_read_before_it_is_used(self):
        names = [s.get("name") for s in load()["jobs"]["sign-and-repackage"]["steps"]]
        self.assertLess(names.index("Read the release state"), names.index("Configure Windows"))
        self.assertLess(names.index("Read the release state"), names.index("Create Release"))


if __name__ == "__main__":
    unittest.main()
