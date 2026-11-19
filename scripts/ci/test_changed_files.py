"""Unit tests for changed-files.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import importlib.util
import json
import pathlib
import subprocess
import unittest

HERE = pathlib.Path(__file__).parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


changed_files = load("changed_files", "changed-files.py")
classify = load("classify", "classify-changes.py")

SHA_A = "a" * 40
SHA_B = "b" * 40


def entries(*files):
    """What the jq filter prints for each file object: [filename, previous_filename]."""
    return [json.dumps([f[0], f[1]] if isinstance(f, tuple) else [f, None]) for f in files]


def compare(status, *files):
    return "\n".join([status] + entries(*files)) + "\n"


class Recorder:
    def __init__(self, stdout="", returncode=0):
        self.calls = []
        self.stdout = stdout
        self.returncode = returncode

    def __call__(self, command, **_):
        self.calls.append(command)
        return subprocess.CompletedProcess(command, self.returncode, self.stdout, "boom" if self.returncode else "")


def push(stdout, **kwargs):
    run = Recorder(stdout, **kwargs)
    return changed_files.changed_files("push", "o/r", before=SHA_A, after=SHA_B, run=run), run


class EndpointTest(unittest.TestCase):
    def test_pull_request(self):
        path, jq, paginate, limit = changed_files.endpoint("pull_request", "o/r", "7", None, None)
        self.assertEqual(path, "repos/o/r/pulls/7/files")
        self.assertTrue(paginate)
        self.assertEqual(limit, 3000)

    def test_push_compares_the_two_commits_on_the_first_page_only(self):
        path, jq, paginate, limit = changed_files.endpoint("push", "o/r", None, SHA_A, SHA_B)
        self.assertEqual(path, f"repos/o/r/compare/{SHA_A}...{SHA_B}")
        self.assertIn(".status", jq)
        self.assertFalse(paginate, "files appear on the first page only; --paginate adds nothing")
        self.assertEqual(limit, 300)

    def test_unknown_cases_have_no_endpoint(self):
        self.assertIsNone(changed_files.endpoint("push", "o/r", None, "0" * 40, SHA_B), "new branch")
        self.assertIsNone(changed_files.endpoint("push", "o/r", None, "", SHA_B))
        self.assertIsNone(changed_files.endpoint("workflow_dispatch", "o/r", None, SHA_A, SHA_B))
        self.assertIsNone(changed_files.endpoint("schedule", "o/r", None, None, None))
        self.assertIsNone(changed_files.endpoint("pull_request", "o/r", None, None, None))


class ChangedFilesTest(unittest.TestCase):
    def test_fast_forward_push_lists_the_files_with_renames_under_both_names(self):
        files, run = push(compare("ahead", "README.md", ("new/name.cpp", "old/name.cpp")))
        self.assertEqual(files, ["README.md", "new/name.cpp", "old/name.cpp"])
        self.assertEqual(run.calls[0][:2], ["gh", "api"])
        self.assertNotIn("--paginate", run.calls[0])

    def test_unknown_cases_make_no_api_call(self):
        run = Recorder("x\n")
        self.assertEqual(changed_files.changed_files("push", "o/r", before="0" * 40, after=SHA_B, run=run), [])
        self.assertEqual(changed_files.changed_files("workflow_dispatch", "o/r", run=run), [])
        self.assertEqual(run.calls, [])

    def test_failed_call_means_everything(self):
        self.assertEqual(push("", returncode=1)[0], [])

    def test_a_push_that_is_not_a_fast_forward_means_everything(self):
        # B changed src/config.cpp; A forked from the same ancestor and changed a document. Forcing B -> A reverts the code
        # change, which the three-dot compare B...A does not list.
        for status in ("diverged", "behind", "identical", ""):
            self.assertEqual(push(compare(status, "docs/a.md"))[0], [], status)

    def test_compare_returns_at_most_300_files_so_300_means_everything(self):
        # 300 documents are visible and src/config.cpp is the 301st, cut off by the API: the list must not be trusted
        docs = [f"docs/page{i}.md" for i in range(300)]
        self.assertEqual(push(compare("ahead", *docs))[0], [])
        self.assertEqual(len(push(compare("ahead", *docs[:299]))[0]), 299)

    def test_file_objects_are_counted_before_renames_are_expanded(self):
        # 150 renames are 300 names but only 150 file objects: the list is complete
        renames = [(f"new/{i}.md", f"old/{i}.md") for i in range(150)]
        self.assertEqual(len(push(compare("ahead", *renames))[0]), 300)

    def test_pull_request_limit_is_3000_objects(self):
        run = Recorder("\n".join(entries(*[f"f{i}" for i in range(2999)])) + "\n")
        self.assertEqual(len(changed_files.changed_files("pull_request", "o/r", pr="7", run=run)), 2999)
        run = Recorder("\n".join(entries(*[f"f{i}" for i in range(3000)])) + "\n")
        self.assertEqual(changed_files.changed_files("pull_request", "o/r", pr="7", run=run), [])


class ClassifiedTest(unittest.TestCase):
    """What pushes to master turn into, end to end through the classifier."""

    def flags(self, *files):
        return classify.classify(push(compare("ahead", *files))[0], "push")

    def test_readme_only_push_builds_nothing(self):
        flags = self.flags("docs/readme/README.en.md", "assets/readme/p3.webp")
        self.assertTrue(flags["docs"])
        self.assertFalse(flags["any_code"])

    def test_root_readme_only_push_does_not_build(self):
        flags = self.flags("README.md")
        self.assertTrue(flags["webui_inputs"])
        self.assertFalse(flags["build"] or flags["any_code"])

    def test_code_push_builds(self):
        self.assertTrue(self.flags("src/config.cpp")["cpp"])

    def test_web_ui_only_push_is_code_and_must_still_be_released(self):
        # cpp is false, but a master push that changes the Web UI ships a new package: the workflow builds on any_code
        flags = self.flags("src_assets/common/assets/web/pages/example.vue")
        self.assertFalse(flags["cpp"])
        self.assertTrue(flags["webui"] and flags["any_code"] and flags["build"])

    def test_installer_only_push_is_code(self):
        self.assertTrue(self.flags("scripts/build-setup-ui.ps1")["any_code"])

    def test_workflow_only_push_publishes_nothing(self):
        flags = self.flags(".github/workflows/translate-readme.yml")
        self.assertTrue(flags["workflow"])
        self.assertFalse(flags["any_code"])


if __name__ == "__main__":
    unittest.main()
