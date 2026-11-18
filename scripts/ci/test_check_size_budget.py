"""Unit tests for check-size-budget.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_size_budget", HERE / "check-size-budget.py")
budget_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget_tool)


class EvaluateTest(unittest.TestCase):
    def test_under_the_warning_line_is_ok(self):
        self.assertEqual(budget_tool.evaluate(40_000_000, 46_000_000, 0.9)[0], "ok")

    def test_the_warning_line_itself_warns(self):
        self.assertEqual(budget_tool.evaluate(41_400_000, 46_000_000, 0.9)[0], "warn")

    def test_exactly_the_limit_still_passes_with_a_warning(self):
        self.assertEqual(budget_tool.evaluate(46_000_000, 46_000_000, 0.9)[0], "warn")

    def test_one_byte_over_the_limit_fails(self):
        status, message = budget_tool.evaluate(46_000_001, 46_000_000, 0.9)
        self.assertEqual(status, "fail")
        self.assertIn("车主批准", message)
        self.assertIn("size-budget.json", message)
        self.assertIn("1 字节", message)


class MainTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.dir.name)
        self.addCleanup(self.dir.cleanup)
        self.budget = self.root / "budget.json"
        self.budget.write_text(json.dumps({"installer_max_bytes": 1000, "warn_fraction": 0.9}), encoding="utf-8")
        self.summary = self.root / "summary.md"

    def file(self, name, size):
        path = self.root / name
        path.write_bytes(b"x" * size)
        return str(path)

    def run_tool(self, *extra, installer=None):
        argv = ["--budget", str(self.budget), "--summary", str(self.summary),
                "--installer", installer or self.file("setup.exe", 500), *extra]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = budget_tool.main(argv)
        return code, out.getvalue()

    def test_a_small_installer_passes_and_the_summary_lists_the_measured_files(self):
        code, out = self.run_tool("--measure", f"sunshine.exe={self.file('sunshine.exe', 700)}",
                                  "--measure", f"便携包={self.root / 'missing.zip'}")
        self.assertEqual(code, 0)
        self.assertNotIn("::warning::", out)
        text = self.summary.read_text(encoding="utf-8")
        self.assertIn("| sunshine.exe | 700 |", text)
        self.assertIn("| 便携包 | 未生成 |", text)
        self.assertIn("结果：通过。", text)

    def test_a_file_inside_the_portable_zip_is_measured_uncompressed(self):
        archive = self.root / "portable.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("Sunshine/sunshine.exe", b"\0" * 5000)
        code, _ = self.run_tool("--measure-zip", f"sunshine.exe={archive}::Sunshine/sunshine.exe",
                                "--measure-zip", f"gui={archive}::Sunshine/assets/gui/sunshine-gui.exe")
        self.assertEqual(code, 0)
        text = self.summary.read_text(encoding="utf-8")
        self.assertIn("| sunshine.exe | 5,000 |", text)
        self.assertIn("| gui | 未生成 |", text)
        code, out = self.run_tool("--measure-zip", "broken")
        self.assertEqual(code, 2)

    def test_the_warning_is_printed_but_does_not_fail(self):
        code, out = self.run_tool(installer=self.file("big.exe", 950))
        self.assertEqual(code, 0)
        self.assertIn("::warning::", out)

    def test_an_over_budget_installer_fails_with_an_error_annotation(self):
        code, out = self.run_tool(installer=self.file("huge.exe", 1001))
        self.assertEqual(code, 1)
        self.assertIn("::error::", out)
        self.assertIn("失败", self.summary.read_text(encoding="utf-8"))

    def test_a_missing_installer_is_an_error_not_a_pass(self):
        code, out = self.run_tool(installer=str(self.root / "nope.exe"))
        self.assertEqual(code, 2)
        self.assertIn("找不到安装器", out)

    def test_a_malformed_budget_is_rejected(self):
        for bad in ({"installer_max_bytes": 0}, {"installer_max_bytes": True}, {"installer_max_bytes": "46000000"},
                    {"installer_max_bytes": 10, "warn_fraction": 1.5}):
            self.budget.write_text(json.dumps(bad), encoding="utf-8")
            code, _ = self.run_tool()
            self.assertEqual(code, 2, bad)

    def test_messages_print_on_a_console_that_is_not_utf8(self):
        raw = io.BytesIO()
        console = io.TextIOWrapper(raw, encoding="cp1252", errors="strict")
        argv = ["--budget", str(self.budget), "--summary", str(self.summary), "--installer", self.file("setup.exe", 500)]
        saved = budget_tool.sys.stdout
        budget_tool.sys.stdout = console
        try:
            code = budget_tool.main(argv)
            console.flush()
        finally:
            budget_tool.sys.stdout = saved
        self.assertEqual(code, 0)
        self.assertIn("体积门禁".encode("utf-8"), raw.getvalue())

    def test_the_shipped_budget_is_valid_and_documents_the_approval_rule(self):
        path = HERE / "size-budget.json"
        limit, fraction = budget_tool.load_budget(path)
        self.assertEqual(limit, 46_000_000)
        self.assertEqual(fraction, 0.9)
        self.assertIn("车主批准", json.loads(path.read_text(encoding="utf-8"))["说明"])


if __name__ == "__main__":
    unittest.main()
