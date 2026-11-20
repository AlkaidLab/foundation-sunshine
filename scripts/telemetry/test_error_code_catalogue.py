import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import error_code_catalogue as cat  # noqa: E402


class Head(unittest.TestCase):
    # the same vectors as tests/unit/test_error_code.cpp (ErrorCode*)
    def test_vectors(self):
        self.assertEqual(cat.error_head("Couldn't read packet: end of file"), "couldn't read packet")
        self.assertEqual(cat.error_head("Couldn't accept incoming connections: aborted"), "couldn't accept incoming connections")
        self.assertEqual(cat.error_head("Client 'Alice' refused"), "client")
        self.assertEqual(cat.error_head("NvEnc: NvEncOpenEncodeSessionEx() failed: 8"), "nvenc nvencopenencodesessionex")
        self.assertEqual(cat.error_head("Audio capture: failed to initialize microphone"), "audio capture failed to initialize microphone")
        self.assertEqual(cat.error_head("Failed to apply the profile: access denied"), "failed to apply the profile")
        self.assertEqual(cat.error_head("[Display Init] Configured adapter [0x1] differs"), "display init configured adapter")
        self.assertEqual(cat.error_head("[vdd_capture] failed to query IDXGIDevice for adapter LUID: 0x80070005"), "vdd_capture failed to query idxgidevice for adapter luid")
        self.assertEqual(cat.error_head("[0x7ff] oops"), "")
        self.assertEqual(cat.error_head("[user@example] oops"), "")
        self.assertLessEqual(len(cat.error_head("a" * 300 + " " + "b" * 300)), 96)
        self.assertEqual(cat.error_code("Download failed or rejected [https://a/b.jpg, code:23]"), cat.error_code("Download failed or rejected [https://c/d.png, code:35]"))
        self.assertEqual(len(cat.error_code("x")), 9)

    def test_hash_is_fnv1a(self):
        self.assertEqual(cat.fnv1a(""), 2166136261)  # the code of an empty head is e811c9dc5
        self.assertEqual(cat.error_code(""), "e811c9dc5")

    def test_scan(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "src"))
            with open(os.path.join(d, "src", "a.cpp"), "w") as f:
                f.write('void f() {\n  BOOST_LOG(error) << "NvEnc: NvEncOpenEncodeSessionEx() failed: "sv << status;\n  BOOST_LOG(fatal) << "Couldn\'t start " << port;\n  BOOST_LOG(info) << "ignored";\n}\n')
            table = cat.scan(d)
            self.assertEqual(len(table), 2)
            code = cat.error_code("NvEnc: NvEncOpenEncodeSessionEx() failed: ")
            self.assertEqual(table[code]["sites"][0]["line"], 2)
            self.assertEqual(table[code]["head"], "nvenc nvencopenencodesessionex")


class Bytes(unittest.TestCase):
    def test_long_multi_byte_messages_are_cut_at_bytes_like_the_host(self):
        # the same two messages and codes as tests/unit/test_error_code.cpp (ErrorCode.ALongMessageWithMultiByteCharactersIsCutAtBytesNotCharacters); the codes were taken from the C++ function
        no_space = "x" + "错" * 40
        spaced = "加载失败" + " 加载失败" * 12
        self.assertEqual(len(cat.error_head_bytes(no_space)), 96)  # cut in the middle of a character
        self.assertEqual(cat.error_code(no_space), "ec2d441a4")
        self.assertLessEqual(len(cat.error_head_bytes(spaced)), 96)
        self.assertEqual(cat.error_code(spaced), "ed4859417")
        # a cut by characters would be longer than 96 bytes and give another code
        self.assertNotEqual(cat.fnv1a(("x" + "错" * 95).encode()), cat.fnv1a(cat.error_head_bytes(no_space)))


class Controls(unittest.TestCase):
    def test_drop_controls_matches_the_host_rule(self):
        self.assertEqual(cat._drop_controls(b"a\tb\rc\nd"), b"a b c d")
        self.assertEqual(cat._drop_controls(b"a\x00\x01\x1f\x7fb"), b"ab")
        self.assertEqual(cat._drop_controls(b"a\xc2\x80b\xc2\x9fc"), b"abc")
        self.assertEqual(cat._drop_controls(b"a\xe2\x80\xa8b\xe2\x80\xaec\xe2\x81\xa6d\xe2\x81\xa9e"), b"abcde")
        self.assertEqual(cat._drop_controls(b"a\xef\xbb\xbfb"), b"ab")
        # a truncated sequence cannot be judged and stays as it is
        self.assertEqual(cat._drop_controls(b"a\xe2\x80"), b"a\xe2\x80")


class Statements(unittest.TestCase):
    def scan_text(self, source):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a.cpp"), "w") as f:
                f.write(source)
            return cat.scan(d)

    def test_a_semicolon_inside_the_message_does_not_end_the_statement(self):
        table = self.scan_text('void f() { BOOST_LOG(error) << "Failed; retry aborted: "sv << code; }\n')
        self.assertEqual(list(table), [cat.error_code("Failed; retry aborted")])
        self.assertEqual(cat.error_head("Failed; retry aborted: x"), "failed; retry aborted")

    def test_adjacent_literals_are_one_message(self):
        table = self.scan_text('void f() { BOOST_LOG(error) << "Audio capture: " "failed to initialize microphone"; }\n')
        self.assertEqual(list(table), [cat.error_code("Audio capture: failed to initialize microphone")])
        # across << and with a suffix, as the host writes them
        table = self.scan_text('void f() { BOOST_LOG(fatal) << "Couldn\'t start "sv << "the server"sv << port; }\n')
        self.assertEqual(list(table), [cat.error_code("Couldn't start the server")])

    def test_a_chain_that_starts_with_a_value_has_the_empty_code(self):
        table = self.scan_text('void f() { BOOST_LOG(error) << name << " failed"; }\n')
        self.assertEqual(list(table), ["e811c9dc5"])

    def test_numeric_escapes_have_the_same_bytes_as_a_cpp_literal(self):
        table = self.scan_text(
            r'void f() { BOOST_LOG(error) << "Failed \x41" << " \101"; }\n'
        )
        code = cat.error_code("Failed A A")
        self.assertEqual(list(table), [code])
        self.assertEqual(table[code]["sites"][0]["text"], "Failed A A")

    def test_numeric_escapes_preserve_utf8_bytes(self):
        table = self.scan_text(r'void f() { BOOST_LOG(error) << "Prefix \xe4\xb8\xad"; }\n')
        self.assertEqual(list(table), [cat.error_code("Prefix 中")])

    def test_an_empty_literal_operand_does_not_end_the_message(self):
        table = self.scan_text('void f() { BOOST_LOG(error) << "" << "Failed"; }\n')
        self.assertEqual(list(table), [cat.error_code("Failed")])

    def test_digit_separators_are_not_character_literals(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a.cpp"), "w") as f:
                f.write("constexpr int timeout = 1'000;\n/* BOOST_LOG(error) << \"Ignored\"; */\nconstexpr auto mask = 0xFF'FF'u;\nvoid f() { BOOST_LOG(error) << \"Real failure\"; }\n")
            table = cat.scan(d)
            self.assertEqual(sorted(e["head"] for e in table.values()), ["real failure"])
            self.assertEqual(next(iter(table.values()))["sites"][0]["line"], 4)

    def test_scan_hashes_the_message_after_dropping_controls(self):
        table = self.scan_text('void f() { BOOST_LOG(error) << "\\t"sv << name; }\nvoid g() { BOOST_LOG(error) << "Fail\\ted\\x01: here" << v; }\n')
        self.assertEqual(list(table), [cat.error_code(""), cat.error_code("Fail ed: here")])
        self.assertEqual(table[cat.error_code("Fail ed: here")]["sites"][0]["text"], "Fail ed: here")

    def test_a_digit_separator_in_a_hex_float_is_not_a_character_literal(self):
        table = self.scan_text("constexpr double x = 0x1.a'bp0; // BOOST_LOG(error) << \"Ignored\";\nvoid f() { BOOST_LOG(error) << \"Real failure\"; }\n")
        self.assertEqual(list(table), [cat.error_code("Real failure")])
        self.assertEqual(next(iter(table.values()))["sites"][0]["line"], 2)

    def test_raw_strings_hide_quotes_and_comment_markers(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a.cpp"), "w") as f:
                f.write('const char *p = R"("/*)";\nvoid f() { BOOST_LOG(error) << "After raw"; }\nconst char *q = R"x(*/ ")x";\nvoid g() { BOOST_LOG(error) << R"(Raw "quoted" \\n)" << v; }\n')
            table = cat.scan(d)
            self.assertIn(cat.error_code("After raw"), table)
            raw = cat.error_code('Raw "quoted" \\n')
            self.assertIn(raw, table)
            self.assertEqual(table[raw]["sites"][0]["line"], 4)
            self.assertEqual(len(table), 2)

    def test_a_double_quote_in_a_raw_delimiter_keeps_the_next_line(self):
        # R""(/*)"" has the delimiter " and the content /*: without reading the
        # raw literal the /* would swallow the rest of the file, including the
        # real log line below
        table = self.scan_text('const char *p = R""(/*)"";\nvoid f() { BOOST_LOG(error) << "After raw delimiter"; }\n')
        self.assertEqual(list(table), [cat.error_code("After raw delimiter")])
        self.assertEqual(next(iter(table.values()))["sites"][0]["line"], 2)

    def test_a_line_comment_continues_after_a_backslash_newline(self):
        table = self.scan_text('// disabled \\\nBOOST_LOG(error) << "Fake";\nvoid f() { BOOST_LOG(error) << "Real"; }\n')
        self.assertEqual(list(table), [cat.error_code("Real")])
        self.assertEqual(table[cat.error_code("Real")]["sites"][0]["line"], 3)

    def test_a_plain_literal_ends_at_nul_and_the_runtime_drops_the_nul_of_a_suffixed_one(self):
        table = self.scan_text(r'void f() { BOOST_LOG(error) << "Failed\000retry"; }' + "\n")
        self.assertEqual(list(table), [cat.error_code("Failed")])
        table = self.scan_text(r'void f() { BOOST_LOG(error) << "Failed\000retry"sv; }' + "\n")
        self.assertEqual(list(table), [cat.error_code("Failedretry")])

    def test_prefixed_character_literals_are_still_literals(self):
        self.assertEqual(cat.statement_end("x = u8';' + L';'; y", 0), 16)

    def test_comments_outside_literals_are_not_code_or_message_text(self):
        table = self.scan_text(
            r'''// BOOST_LOG(error) << "ignored";
/* BOOST_LOG(fatal) << "ignored;"; */
void f() {
  BOOST_LOG(error) << "Failed // still text;" /* ; << "fake" */ // << "fake"
                  << " retry";
}
'''
        )
        code = cat.error_code("Failed ")
        self.assertEqual(list(table), [code])
        self.assertEqual(table[code]["sites"][0]["text"], "Failed // still text; retry")


class Templates(unittest.TestCase):
    def test_static_suffix_survives_a_value_first_operand(self):
        self.assertEqual(cat.message_template('get_error_string(x << 1) << " failed to set HDR"sv'), b"{} failed to set HDR")
        self.assertEqual(cat.message_template('err << " Setup" "Di failed" << priv'), b"{} SetupDi failed{}")
        self.assertEqual(cat.message_template('err << some_user_value'), b"")

    def test_generation_skips_comments_strings_and_opaque_calls(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            src = root / "src"
            src.mkdir()
            src.joinpath("logger.cpp").write_text(
                '// BOOST_LOG(error) << retired;\n'
                'const auto text = R"(BOOST_LOG(error) << "ignored";)";\n'
                'BOOST_LOG(error) << ([] { return 1; })() << " failed";\n'
                'BOOST_LOG(fatal) << only_value;\n',
                encoding="utf-8",
            )
            out = root / "templates.inc"
            cat.write_templates(root, out)
            self.assertEqual(out.read_text().splitlines()[1:], ['  { "src/logger.cpp", 3, { "{} failed", 9 } },'])


if __name__ == "__main__":
    unittest.main()
