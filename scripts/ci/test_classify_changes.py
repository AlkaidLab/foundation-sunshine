"""Unit tests for classify-changes.py (run: python3 -m unittest scripts/ci/test_classify_changes.py)."""

import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("classify", pathlib.Path(__file__).with_name("classify-changes.py"))
classify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(classify)


def flags(*paths):
    return classify.classify(list(paths))


class ClassifyTest(unittest.TestCase):
    def test_docs_only(self):
        f = flags("docs/getting_started.md", "docs/images/a.png", "README.zh.md", ".github/ISSUE_TEMPLATE/bug_report_en.yml")
        self.assertTrue(f["docs"])
        self.assertFalse(f["any_code"])
        self.assertFalse(f["cpp"])

    def test_docs_plus_code_is_not_docs_only(self):
        f = flags("README.md", "src/config.cpp")
        self.assertFalse(f["docs"])
        self.assertTrue(f["cpp"])
        self.assertTrue(f["docs_touched"])

    def test_code_only_touches_no_docs(self):
        self.assertFalse(flags("src/config.cpp")["docs_touched"])

    def test_unknown_path_is_cpp(self):
        f = flags("brand_new_dir/file.txt")
        self.assertTrue(f["cpp"])
        self.assertFalse(f["docs"])

    def test_build_inputs_that_look_like_docs(self):
        self.assertTrue(flags("docs/CMakeLists.txt")["cpp"])
        self.assertTrue(flags("src_assets/windows/misc/readme.md")["cpp"])

    def test_web_ui_only(self):
        f = flags("src_assets/common/assets/web/views/Home.vue", "package-lock.json")
        self.assertTrue(f["webui"])
        self.assertFalse(f["cpp"])
        self.assertTrue(f["any_code"])

    def test_locale_text_is_web_ui_not_docs(self):
        f = flags("src_assets/common/assets/web/public/assets/locale/zh.json")
        self.assertTrue(f["webui"])
        self.assertFalse(f["docs"])

    def test_other_workflow_is_lint_only(self):
        f = flags(".github/workflows/translate-readme.yml")
        self.assertTrue(f["workflow"])
        self.assertFalse(f["cpp"])
        self.assertFalse(f["any_code"])

    def test_pipeline_change_runs_everything(self):
        f = flags(".github/workflows/main.yml")
        self.assertTrue(f["cpp"] and f["webui"] and f["workflow"])
        f = flags("scripts/ci/report-junit.py")
        self.assertTrue(f["cpp"] and f["webui"])

    def test_installer_pulls_in_build(self):
        f = flags("tools/installer_shell.cpp")
        self.assertTrue(f["installer"] and f["cpp"])

    def test_submodule_pin_is_cpp(self):
        self.assertTrue(flags("third-party/googletest")["cpp"])
        f = flags("src_assets/common/sunshine-control-panel")
        self.assertTrue(f["installer"] and f["cpp"])

    def test_cpp_changes_also_run_the_web_ui_tests(self):
        # Web UI tests read src/config.cpp, tests/fixtures and tools/installer_* to keep the pages in step with the host.
        f = flags("src/config.cpp")
        self.assertTrue(f["cpp"] and f["webui"])

    def test_documents_the_web_ui_tests_read_are_not_docs_only(self):
        for path in ("README.md", "docs/authority/source-index.tsv", "assets/brand/default-apps/README.md"):
            f = flags(path)
            self.assertTrue(f["webui"], path)
            self.assertFalse(f["docs"], path)
        self.assertTrue(flags("docs/api/auth.md")["docs"])

    def test_empty_is_not_docs(self):
        self.assertFalse(classify.classify([])["docs"])


if __name__ == "__main__":
    unittest.main()
