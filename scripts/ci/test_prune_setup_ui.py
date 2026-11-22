"""Unit tests for prune-setup-ui.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import importlib.util
import json
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("prune_setup_ui", pathlib.Path(__file__).with_name("prune-setup-ui.py"))
prune_setup_ui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prune_setup_ui)


class Prune(unittest.TestCase):
    def make_dist(self, clock_referenced=False, css=None):
        root = pathlib.Path(tempfile.mkdtemp())
        dist = root / "dist"
        assets = dist / "assets"
        assets.mkdir(parents=True)
        default_css = (
            "@font-face{font-family:Clock Roboto Flex;src:url(./clock-a.woff2)}"
            "@font-face{font-family:Roboto Flex;src:url(./roboto.woff2)}"
            ".clock-face{color:red}.ok{color:blue}"
            "@media (min-width:600px){.clock-tall{width:1px}.ok2{width:2px}}"
        )
        (assets / "app.css").write_text(css if css is not None else default_css, encoding="utf-8")
        script = "const font = './roboto.woff2';" + ("const x = './clock-a.woff2';" if clock_referenced else "")
        (assets / "app.js").write_text(script, encoding="utf-8")
        (dist / "index.html").write_text('<link href="./assets/app.css"><script src="./assets/app.js"></script>', encoding="utf-8")
        (assets / "clock-a.woff2").write_bytes(b"clock")
        (assets / "roboto.woff2").write_bytes(b"roboto")
        (assets / "app.css.map").write_text("{}", encoding="utf-8")
        return dist, assets

    def test_clock_rules_fonts_and_maps_are_removed(self):
        dist, assets = self.make_dist()
        report = dist.parent / "report.json"
        result = prune_setup_ui.prune(dist, report)
        css = (assets / "app.css").read_text(encoding="utf-8")
        self.assertNotIn("clock", css.lower())
        self.assertIn(".ok{color:blue}", css)
        self.assertIn(".ok2{width:2px}", css)
        self.assertFalse((assets / "clock-a.woff2").exists())
        self.assertTrue((assets / "roboto.woff2").exists())
        self.assertFalse((assets / "app.css.map").exists())
        self.assertEqual(result["locales_removed"], 0)
        self.assertGreater(result["removed_bytes"], 0)
        self.assertGreater(result["rewritten_bytes"], 0)
        self.assertEqual(json.loads(report.read_text())["removed_bytes"], result["removed_bytes"])

    def test_a_clock_font_the_script_still_uses_is_kept(self):
        dist, assets = self.make_dist(clock_referenced=True)
        with self.assertRaises(SystemExit):
            prune_setup_ui.prune(dist)
        self.assertTrue((assets / "clock-a.woff2").exists())

    def test_charset_statement_is_kept_and_the_font_face_is_not_glued_to_it(self):
        css = '@charset "UTF-8";@font-face{font-family:Clock Roboto Flex;src:url(./clock-a.woff2)}.ok{color:blue}'
        dist, assets = self.make_dist(css=css)
        prune_setup_ui.prune(dist)
        after = (assets / "app.css").read_text(encoding="utf-8")
        self.assertTrue(after.startswith('@charset "UTF-8";'))
        self.assertNotIn("@font-face", after)
        self.assertIn(".ok{color:blue}", after)
        self.assertFalse((assets / "clock-a.woff2").exists())

    def test_import_statement_is_kept_before_a_clock_rule(self):
        css = '@import url(./base.css);.clock-face{color:red}.ok{color:blue}'
        dist, assets = self.make_dist(css=css)
        prune_setup_ui.prune(dist)
        after = (assets / "app.css").read_text(encoding="utf-8")
        self.assertTrue(after.startswith("@import url(./base.css);"))
        self.assertNotIn(".clock-face", after)
        self.assertIn(".ok{color:blue}", after)

    def test_nested_group_keeps_its_other_rules(self):
        css = "@media (min-width:600px){.clock-tall{width:1px}.ok2{width:2px}}"
        dist, assets = self.make_dist(css=css)
        prune_setup_ui.prune(dist)
        after = (assets / "app.css").read_text(encoding="utf-8")
        self.assertIn("@media (min-width:600px){.ok2{width:2px}}", after)
        self.assertNotIn(".clock-tall", after)

    def test_an_emptied_layer_keeps_its_position_in_the_cascade(self):
        # The first declaration sets a layer's position in the global order; an
        # empty block still declares it. Dropping the emptied theme block would
        # put override first and change .ok from red to blue.
        css = (
            "@layer theme{.clock-face{color:black}}"
            "@layer override{.ok{color:red}}"
            "@layer theme{.ok{color:blue}}"
        )
        dist, assets = self.make_dist(css=css)
        prune_setup_ui.prune(dist)
        after = (assets / "app.css").read_text(encoding="utf-8")
        self.assertIn("@layer theme{}", after)
        self.assertLess(after.index("@layer theme{}"), after.index("@layer override"))
        self.assertIn("@layer override{.ok{color:red}}", after)
        self.assertIn("@layer theme{.ok{color:blue}}", after)

    def test_an_emptied_media_group_is_still_removed(self):
        css = "@media (min-width:600px){.clock-tall{width:1px}}.ok{color:blue}"
        dist, assets = self.make_dist(css=css)
        prune_setup_ui.prune(dist)
        after = (assets / "app.css").read_text(encoding="utf-8")
        self.assertNotIn("@media", after)
        self.assertIn(".ok{color:blue}", after)

    def test_comments_and_property_rules_are_kept(self):
        css = '/* banner */@property --m-clock-inset{syntax:"<length>";inherits:false;initial-value:16px}.ok{color:blue}'
        dist, assets = self.make_dist(css=css)
        prune_setup_ui.prune(dist)
        after = (assets / "app.css").read_text(encoding="utf-8")
        self.assertIn("/* banner */", after)
        self.assertIn("@property --m-clock-inset", after)
        self.assertIn(".ok{color:blue}", after)

    def test_clock_keyframes_are_removed_and_other_keyframes_kept(self):
        css = "@keyframes clock-tick{from{opacity:1}to{opacity:0}}@keyframes fade{from{opacity:0}to{opacity:1}}.ok{color:blue}"
        dist, assets = self.make_dist(css=css)
        prune_setup_ui.prune(dist)
        after = (assets / "app.css").read_text(encoding="utf-8")
        self.assertNotIn("clock-tick", after)
        self.assertIn("@keyframes fade", after)
        self.assertIn(".ok{color:blue}", after)

    def test_unsupported_css_is_left_unchanged(self):
        css = '@charset "UTF-8";@unknown test{color:red}.clock-face{color:red}.ok{color:blue}'
        dist, assets = self.make_dist(css=css)
        result = prune_setup_ui.prune(dist)
        self.assertEqual((assets / "app.css").read_text(encoding="utf-8"), css)
        self.assertEqual(result["unsupported_css"], ["assets/app.css"])
        self.assertEqual(result["removed"], [])
        self.assertTrue((assets / "clock-a.woff2").exists())


if __name__ == "__main__":
    unittest.main()
