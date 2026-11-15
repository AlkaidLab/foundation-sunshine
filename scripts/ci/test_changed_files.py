"""Unit tests for changed-files.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import importlib.util
import pathlib
import subprocess
import unittest

spec = importlib.util.spec_from_file_location("changed_files", pathlib.Path(__file__).with_name("changed-files.py"))
changed_files = importlib.util.module_from_spec(spec)
spec.loader.exec_module(changed_files)

SHA_A = "a" * 40
SHA_B = "b" * 40


class Recorder:
    def __init__(self, stdout="", returncode=0):
        self.calls = []
        self.stdout = stdout
        self.returncode = returncode

    def __call__(self, command, **_):
        self.calls.append(command)
        return subprocess.CompletedProcess(command, self.returncode, self.stdout, "boom" if self.returncode else "")


class EndpointTest(unittest.TestCase):
    def test_pull_request(self):
        path, jq = changed_files.endpoint("pull_request", "o/r", "7", None, None)
        self.assertEqual(path, "repos/o/r/pulls/7/files")
        self.assertIn("previous_filename", jq)

    def test_push_compares_the_two_commits(self):
        path, jq = changed_files.endpoint("push", "o/r", None, SHA_A, SHA_B)
        self.assertEqual(path, f"repos/o/r/compare/{SHA_A}...{SHA_B}")
        self.assertTrue(jq.startswith(".files[]"))

    def test_unknown_cases_have_no_endpoint(self):
        self.assertIsNone(changed_files.endpoint("push", "o/r", None, "0" * 40, SHA_B), "new branch")
        self.assertIsNone(changed_files.endpoint("push", "o/r", None, "", SHA_B))
        self.assertIsNone(changed_files.endpoint("workflow_dispatch", "o/r", None, SHA_A, SHA_B))
        self.assertIsNone(changed_files.endpoint("schedule", "o/r", None, None, None))
        self.assertIsNone(changed_files.endpoint("pull_request", "o/r", None, None, None))


class ChangedFilesTest(unittest.TestCase):
    def test_push_lists_the_files_with_renames_under_both_names(self):
        run = Recorder("README.md\nnew/name.cpp\nold/name.cpp\n")
        files = changed_files.changed_files("push", "o/r", before=SHA_A, after=SHA_B, run=run)
        self.assertEqual(files, ["README.md", "new/name.cpp", "old/name.cpp"])
        self.assertEqual(run.calls[0][:3], ["gh", "api", "--paginate"])

    def test_unknown_cases_make_no_api_call(self):
        run = Recorder("x\n")
        self.assertEqual(changed_files.changed_files("push", "o/r", before="0" * 40, after=SHA_B, run=run), [])
        self.assertEqual(changed_files.changed_files("workflow_dispatch", "o/r", run=run), [])
        self.assertEqual(run.calls, [])

    def test_failed_call_means_everything(self):
        self.assertEqual(changed_files.changed_files("push", "o/r", before=SHA_A, after=SHA_B, run=Recorder(returncode=1)), [])

    def test_too_many_files_means_everything(self):
        many = "".join(f"f{i}\n" for i in range(changed_files.LIMIT))
        self.assertEqual(changed_files.changed_files("push", "o/r", before=SHA_A, after=SHA_B, run=Recorder(many)), [])


class ClassifiedTest(unittest.TestCase):
    """What a docs-only push to master turns into, end to end through the classifier."""

    def test_readme_only_push_builds_nothing(self):
        classify_spec = importlib.util.spec_from_file_location("classify", pathlib.Path(__file__).with_name("classify-changes.py"))
        classify = importlib.util.module_from_spec(classify_spec)
        classify_spec.loader.exec_module(classify)
        files = changed_files.changed_files("push", "o/r", before=SHA_A, after=SHA_B, run=Recorder("docs/readme/README.en.md\nassets/readme/p3.webp\n"))
        flags = classify.classify(files)
        self.assertTrue(flags["docs"])
        self.assertFalse(flags["cpp"])
        code = classify.classify(changed_files.changed_files("push", "o/r", before=SHA_A, after=SHA_B, run=Recorder("src/config.cpp\n")))
        self.assertTrue(code["cpp"])


if __name__ == "__main__":
    unittest.main()
