"""shard-compile.sh against stub ninja/curl/ccache: chunk claiming, the 422 check, the fixed split, a runner that claims nothing
(run: python3 -m unittest discover -s scripts/ci -p 'test_*.py'). Skipped where bash is not available."""

import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import unittest

CI = pathlib.Path(__file__).resolve().parent
NINJA = """#!/bin/bash
if [[ "$*" == *"-t commands"* ]]; then
  for i in $(seq 1 200); do echo "g++ -o d/o$i.obj -c s.cpp"; done
else
  echo "$*" >> "$LOGF"; touch build/.ninja_log
fi
"""
# POST (claim) answers follow $POST_PLAN, one word per call ("201 422 ..."), then repeat the last; GET answers with $GET_CODE.
CURL = """#!/bin/bash
cat > /dev/null
if [[ "$*" == *"-X POST"* ]]; then
  n=$(( $(cat "$CNT" 2>/dev/null || echo 0) + 1 )); echo $n > "$CNT"
  plan=($POST_PLAN); i=$(( n - 1 )); (( i >= ${#plan[@]} )) && i=$(( ${#plan[@]} - 1 ))
  printf '%s' "${plan[$i]}"
else
  printf '%s' "$GET_CODE"
fi
"""


class ShardCompileTest(unittest.TestCase):
    def setUp(self):
        if shutil.which("bash") is None or shutil.which("python3") is None:
            self.skipTest("bash and python3 are needed")
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        bin_dir = self.tmp / "bin"
        repo = self.tmp / "repo"
        (repo / "build").mkdir(parents=True)
        (repo / "scripts" / "ci").mkdir(parents=True)
        bin_dir.mkdir()
        for name in ("shard-targets.py", "shard-compile.sh", "shard-weights.ninja_log"):
            shutil.copy(CI / name, repo / "scripts" / "ci" / name)
        for name, body in (("ninja", NINJA), ("curl", CURL), ("ccache", "#!/bin/bash\n"), ("python", '#!/bin/bash\nexec python3 "$@"\n')):
            path = bin_dir / name
            path.write_text(body)
            path.chmod(path.stat().st_mode | stat.S_IEXEC)
        (repo / "build" / "build.ninja").touch()
        self.repo, self.bin = repo, bin_dir
        self.log = self.tmp / "log"

    def run_script(self, index, count, token="t", post_plan="201", get_code="404", remove_log=False):
        env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", LOGF=str(self.log), CNT=str(self.tmp / "cnt"),
                   POST_PLAN=post_plan, GET_CODE=get_code, GITHUB_REPOSITORY="o/r", GITHUB_RUN_ID="1", GITHUB_RUN_ATTEMPT="1",
                   GITHUB_SHA="abc", SHARD_CHUNKS="32")
        env.pop("CLAIM_TOKEN", None)
        if token:
            env["CLAIM_TOKEN"] = token
        if remove_log:
            (self.repo / "build" / ".ninja_log").unlink(missing_ok=True)
        result = subprocess.run(["bash", "scripts/ci/shard-compile.sh", str(index), str(count)], cwd=self.repo, env=env,
                                capture_output=True, text=True, timeout=300)
        built = [line.split("ci_chunk_")[1] for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, built

    def test_every_claim_won_builds_every_chunk(self):
        result, built = self.run_script(0, 4)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(built), 32)

    def test_a_runner_that_finds_every_chunk_taken_succeeds_and_leaves_a_build_log_file(self):
        result, built = self.run_script(3, 8, post_plan="422", get_code="200", remove_log=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(built, [])
        log = self.repo / "ninja_log.shard"
        self.assertTrue(log.exists() and log.stat().st_size == 0)

    def test_a_422_for_a_ref_that_does_not_exist_leaves_the_chunk_to_the_fixed_split(self):
        # index 0 of 4 owns chunks 0, 7, 8, 15, 16, 23, 24, 31 in the fixed split
        result, built = self.run_script(0, 4, post_plan="422", get_code="404")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(map(int, built)), [0, 7, 8, 15, 16, 23, 24, 31])
        self.assertIn("does not exist", result.stdout)

    def test_a_read_only_token_falls_back_to_the_fixed_split_and_every_chunk_has_one_owner(self):
        owners = {}
        for index in range(4):
            self.log.unlink(missing_ok=True)
            (self.tmp / "cnt").unlink(missing_ok=True)
            result, built = self.run_script(index, 4, post_plan="403")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("falling back", result.stdout)
            for chunk in built:
                owners.setdefault(int(chunk), []).append(index)
        self.assertEqual(sorted(owners), list(range(32)))
        self.assertTrue(all(len(v) == 1 for v in owners.values()))

    def test_no_token_means_the_fixed_split_without_any_request(self):
        result, built = self.run_script(1, 4, token="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(built), 8)
        self.assertFalse((self.tmp / "cnt").exists())


if __name__ == "__main__":
    unittest.main()
