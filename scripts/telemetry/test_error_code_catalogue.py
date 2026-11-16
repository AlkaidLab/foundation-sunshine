import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import error_code_catalogue as cat  # noqa: E402


class Head(unittest.TestCase):
    # the same vectors as tests/unit/test_telemetry.cpp (ErrorCode*)
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
        # the same two messages and codes as tests/unit/test_telemetry.cpp (ErrorCode.ALongMessageWithMultiByteCharactersIsCutAtBytesNotCharacters); the codes were taken from the C++ function
        no_space = "x" + "错" * 40
        spaced = "加载失败" + " 加载失败" * 12
        self.assertEqual(len(cat.error_head_bytes(no_space)), 96)  # cut in the middle of a character
        self.assertEqual(cat.error_code(no_space), "ec2d441a4")
        self.assertLessEqual(len(cat.error_head_bytes(spaced)), 96)
        self.assertEqual(cat.error_code(spaced), "ed4859417")
        # a cut by characters would be longer than 96 bytes and give another code
        self.assertNotEqual(cat.fnv1a(("x" + "错" * 95).encode()), cat.fnv1a(cat.error_head_bytes(no_space)))


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


if __name__ == "__main__":
    unittest.main()
