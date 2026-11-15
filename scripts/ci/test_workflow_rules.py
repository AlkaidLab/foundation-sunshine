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

    def test_the_release_action_runs_only_for_a_publishing_push(self):
        # LizardByte/create-release-action creates a release for a push to master only; a manual run has its own job
        release = step(self.build, "Create/Update GitHub Release")
        self.assertEqual(release["if"], "needs.setup_release.outputs.publish_release == 'true' && github.event_name == 'push'")

    def test_a_manual_publish_has_its_own_job_that_alone_can_write(self):
        publish = self.workflow["jobs"]["publish"]
        self.assertIn("workflow_dispatch", publish["if"])
        self.assertIn("publish_release == 'true'", publish["if"])
        self.assertEqual(publish["permissions"], {"contents": "write"})
        self.assertIn("build_win", publish["needs"])
        run = step(publish, "Create the pre-release")["run"]
        self.assertIn("scripts/ci/publish-prerelease.sh", run)
        script = (WORKFLOW.parents[2] / "scripts" / "ci" / "publish-prerelease.sh").read_text(encoding="utf-8")
        for needle in ("gh release create", "--prerelease", "--target", "isPrerelease"):
            self.assertIn(needle, script)

    def test_permissions_are_read_only_unless_a_job_asks_for_more(self):
        self.assertEqual(self.workflow["permissions"], {"contents": "read"})
        writers = sorted(name for name, job in self.workflow["jobs"].items()
                         if "write" in (job.get("permissions") or {}).values())
        # shard / shard_cleanup: the claim refs of the cold compile (refs/claims/<run>/<chunk>), nothing else
        self.assertEqual(writers, ["build_win", "publish", "setup_release", "shard", "shard_cleanup"])
        self.assertEqual(self.build["permissions"], {"contents": "read", "actions": "write"})

    def test_the_publish_decision_does_not_come_straight_from_the_release_action(self):
        output = self.setup["outputs"]["publish_release"]
        self.assertIn("steps.auto_gate.outputs.publish", output)
        self.assertNotIn("setup_release_auto.outputs.publish_release", output)

    def test_automatic_pre_releases_need_the_repository_variable(self):
        gate = step(self.setup, "Automatic pre-releases")
        self.assertEqual(gate["env"]["ENABLED"], "${{ vars.AUTO_PRERELEASE }}")
        self.assertIn('[ "$AUTO" = true ] && [ "$ENABLED" = true ]', gate["run"])
        self.assertEqual(gate["if"], "github.event_name == 'push' || github.event_name == 'pull_request'")

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

    def test_the_nightly_build_is_cold_and_never_publishes(self):
        schedule = self.workflow[True]["schedule"]
        self.assertEqual(len(schedule), 1)
        restore = step(self.build, "Cache compiler objects")
        # a sharded cold build restores what its shards compiled instead (see ShardTest)
        self.assertEqual(restore["if"], "github.event_name != 'schedule' && needs.shard.result != 'success'")
        manual = step(self.setup, "Setup Release (Manual)")
        self.assertIn("schedule", manual["if"])
        publish = self.workflow["jobs"]["publish"]
        self.assertNotIn("schedule", publish["if"])

    def test_artifacts_are_uploaded_whether_or_not_the_run_publishes(self):
        upload = step(self.build, "Upload Artifacts")
        self.assertNotIn("publish_release", upload.get("if", ""))


class ShardTest(unittest.TestCase):
    def setUp(self):
        self.workflow = load()
        self.jobs = self.workflow["jobs"]

    def test_the_nightly_build_and_a_missing_cache_start_the_shards_and_the_warm_path_does_not(self):
        plan = step(self.jobs["cold_plan"], "Decide")
        self.assertIn('[[ "$EVENT" == schedule ]]', plan["run"])
        self.assertIn("no compiler cache entry to restore", plan["run"])
        self.assertEqual(self.jobs["shard"]["if"], "needs.cold_plan.outputs.sharded == 'true'")

    def test_the_build_job_waits_for_the_shards(self):
        self.assertIn("shard", self.jobs["build_win"]["needs"])

    def test_the_shards_configure_like_the_build_job_apart_from_what_only_orders_the_objects(self):
        shard = step(self.jobs["shard"], "Configure Windows")
        build = step(self.jobs["build_win"], "Configure Windows")
        for name in ("BUILD_VERSION", "SUNSHINE_RTX_HDR", "SUNSHINE_BUILD_COMMIT", "DRIVER_DEPS_REQUIRED"):
            self.assertEqual(shard["env"][name], build["env"][name], name)
        self.assertIn("-DSUNSHINE_BUILD_MAT_CAPTURE=OFF", shard["run"])
        self.assertNotIn("MAT_CAPTURE", build["run"])

    def test_the_gate_waits_for_the_shards_and_the_jobs_that_ran_inside_the_build_job_before(self):
        for name in ("shard", "adapter_config", "setup_ui"):
            self.assertIn(name, self.jobs["ci-gate"]["needs"])

    def test_the_claim_token_reaches_only_the_compile_script(self):
        shard = self.jobs["shard"]
        holders = [x["name"] for x in shard["steps"] if "CLAIM_TOKEN" in x.get("env", {})]
        self.assertEqual(holders, ["Compile object files, chunk by chunk"])
        script = pathlib.Path(__file__).with_name("shard-compile.sh").read_text(encoding="utf-8")
        self.assertIn("unset CLAIM_TOKEN", script)

    def test_the_moved_steps_run_in_their_own_jobs_and_the_build_job_consumes_the_setup_ui(self):
        build = self.jobs["build_win"]
        names = [x.get("name") for x in build["steps"]]
        self.assertNotIn("Test the adapter build configuration without the SDK", names)
        self.assertIn("Test the adapter build configuration without the SDK", [x.get("name") for x in self.jobs["adapter_config"]["steps"]])
        self.assertIn("setup_ui", build["needs"])
        self.assertEqual(step(build, "Installer setup UI")["with"]["name"], "foundation-setup-ui")

    def test_the_build_job_also_runs_after_a_failed_shard_but_not_after_a_cancelled_one(self):
        condition = self.jobs["build_win"]["if"]
        self.assertIn("!cancelled()", condition)
        self.assertIn("contains(fromJSON('[\"success\",\"skipped\",\"failure\"]'), needs.shard.result)", condition)
        for name in ("Merge the shard compiler caches", "Unpack the shard compiler caches"):
            self.assertIn("needs.shard.result == 'failure'", step(self.jobs["build_win"], name)["if"])
        # whatever the shards compiled before they died still reaches build_win
        for name in ("Pack the compiler cache", "Upload the compiler cache", "Upload the ninja log"):
            self.assertEqual(step(self.jobs["shard"], name)["if"], "${{ !cancelled() }}")

    def gate(self, **results):
        """Run the ci-gate script with the given job results (jobs not named succeed)."""
        script = step(self.jobs["ci-gate"], "Every needed job succeeded or was deliberately skipped")["run"]
        code = script.split("<<'PY'\n", 1)[1].rsplit("\nPY", 1)[0]
        code = "\n".join(line[10:] if line.startswith(" " * 10) else line for line in code.splitlines())
        needs = {name: {"result": "success"} for name in self.jobs["ci-gate"]["needs"]}
        for name, result in results.items():
            needs[name] = {"result": result}
        import json
        import os
        import subprocess
        import sys
        env = dict(os.environ, NEEDS=json.dumps(needs))
        return subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True).returncode

    def test_the_gate_forgives_a_failed_shard_only_when_the_build_job_succeeded(self):
        self.assertEqual(self.gate(), 0)
        self.assertEqual(self.gate(shard="skipped", cold_plan="success"), 0)
        self.assertEqual(self.gate(shard="failure"), 0)
        self.assertEqual(self.gate(shard="failure", build_win="failure"), 1)
        self.assertEqual(self.gate(shard="failure", build_win="skipped"), 1)
        self.assertEqual(self.gate(shard="failure", build_win="cancelled"), 1)
        self.assertEqual(self.gate(shard="cancelled"), 1)
        self.assertEqual(self.gate(build_win="failure"), 1)
        self.assertEqual(self.gate(adapter_config="failure"), 1)


if __name__ == "__main__":
    unittest.main()
