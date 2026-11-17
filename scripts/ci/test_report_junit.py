"""Unit tests for report-junit.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import importlib.util
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("report_junit", pathlib.Path(__file__).with_name("report-junit.py"))
report_junit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report_junit)


def load(xml):
    with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False, encoding="utf-8") as handle:
        handle.write(xml)
    return report_junit.load([handle.name])


class LoadTest(unittest.TestCase):
    def test_statuses(self):
        cases = load("""<testsuite>
          <testcase name="ok" classname="a" status="run"/>
          <testcase name="bad" classname="a" status="fail"><failure message="boom"/></testcase>
          <testcase name="skipped_child" classname="a"><skipped/></testcase>
          <testcase name="not_run" classname="a" status="notrun"/>
          <testcase name="disabled" classname="a" status="disabled"/>
        </testsuite>""")
        self.assertEqual([c["status"] for c in cases], ["pass", "fail", "skip", "skip", "skip"])


if __name__ == "__main__":
    unittest.main()
