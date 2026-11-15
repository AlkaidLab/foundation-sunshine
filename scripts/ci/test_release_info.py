"""Unit tests for release-info.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("release_info", pathlib.Path(__file__).with_name("release-info.py"))
release_info = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release_info)


class ReleaseInfoTest(unittest.TestCase):
    def test_november_afternoon(self):
        self.assertEqual(release_info.compute("2026-11-14T13:16:17Z"), ("2026.1114.131617", "v2026.1114.131617", "v2026.1114"))

    def test_leading_zero_clock_is_a_plain_integer(self):
        # 09:05:07 -> "090507" -> 90507, exactly as the action does
        self.assertEqual(release_info.compute("2026-11-14T09:05:07Z")[0], "2026.1114.90507")

    def test_single_digit_month_and_day(self):
        version, tag, asset = release_info.compute("2026-01-05T00:00:09Z")
        self.assertEqual(version, "2026.105.9")
        self.assertEqual(asset, "v2026.0105")

    def test_offset_dates_are_converted_to_utc(self):
        self.assertEqual(release_info.compute("2026-11-15T01:30:00+08:00")[0], "2026.1114.173000")

    def test_probe_commit_from_the_event_payload_experiment(self):
        # the committer date of the fork experiment commit (author date was 2026-01-02T03:04:05)
        self.assertEqual(release_info.compute("2026-01-03T06:07:08Z")[0], "2026.103.60708")


if __name__ == "__main__":
    unittest.main()
