"""Rules of .github/workflows/main.yml that a typo would break silently (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import pathlib
import unittest

import yaml

WORKFLOW = pathlib.Path(__file__).resolve().parents[2] / ".github" / "workflows" / "main.yml"


def load():
    with open(WORKFLOW, encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def step(job, name):
    return next(s for s in job["steps"] if s.get("name") == name)


class PublishTest(unittest.TestCase):
    def setUp(self):
        self.workflow = load()
        self.setup = self.workflow["jobs"]["setup_release"]
        self.build = self.workflow["jobs"]["build_win"]

    def test_the_release_step_runs_only_when_the_run_publishes(self):
        release = step(self.build, "Create/Update GitHub Release")
        self.assertEqual(release["if"], "needs.setup_release.outputs.publish_release == 'true'")

    def test_the_publish_decision_does_not_come_straight_from_the_release_action(self):
        output = self.setup["outputs"]["publish_release"]
        self.assertIn("steps.auto_gate.outputs.publish", output)
        self.assertNotIn("setup_release_auto.outputs.publish_release", output)

    def test_automatic_pre_releases_need_the_repository_variable(self):
        gate = step(self.setup, "Automatic pre-releases")
        self.assertEqual(gate["env"]["ENABLED"], "${{ vars.AUTO_PRERELEASE }}")
        self.assertIn('[ "$AUTO" = true ] && [ "$ENABLED" = true ]', gate["run"])
        self.assertEqual(gate["if"], "github.event_name != 'workflow_dispatch'")

    def test_manual_publish_is_an_input_that_defaults_to_off_and_is_wired_through(self):
        # PyYAML reads the key `on` as the boolean True
        dispatch = self.workflow[True]["workflow_dispatch"]["inputs"]["publish"]
        self.assertEqual(dispatch["type"], "boolean")
        self.assertIs(dispatch["default"], False)
        manual = step(self.setup, "Setup Release (Manual)")
        self.assertEqual(manual["env"]["PUBLISH"], "${{ inputs.publish }}")
        self.assertIn("publish_release=true", manual["run"])

    def test_a_build_that_does_not_publish_is_a_dev_build(self):
        configure = step(self.build, "Configure Windows")
        channel = configure["env"]["SUNSHINE_BUILD_CHANNEL"]
        self.assertEqual(channel, "${{ needs.setup_release.outputs.publish_release == 'true' && 'beta' || 'dev' }}")

    def test_artifacts_are_uploaded_whether_or_not_the_run_publishes(self):
        upload = step(self.build, "Upload Artifacts")
        self.assertNotIn("publish_release", upload.get("if", ""))


if __name__ == "__main__":
    unittest.main()
