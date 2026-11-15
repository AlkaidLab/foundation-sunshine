"""Unit tests for make-symbols.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import importlib.util
import json
import pathlib
import stat
import struct
import sys
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("make_symbols", pathlib.Path(__file__).with_name("make-symbols.py"))
make_symbols = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_symbols)

GUID = bytes.fromhex("1fed61c55aad5ca89204343923e044e1")
SYM = """MODULE windows x86_64 000000000000000000000000000000000 Sunshine.exe
INFO CODE_ID 6AF9696A1B000 sunshine.exe
FILE 0 D:\\a\\fs\\fs\\src\\video.cpp
FILE 1 C:/msys64/ucrt64/include/c++/vector
FUNC 1000 10 0 encode_frame(int)
1000 10 42 0
STACK CFI INIT 1000 10 .cfa: $rsp 8 + .ra: .cfa 8 - ^
"""


def fake_pe(guid=GUID, age=1, with_debug=True):
    """The smallest PE32+ image that carries one CodeView debug directory entry in a section."""
    headers = bytearray(0x400)
    headers[:2] = b"MZ"
    struct.pack_into("<I", headers, 0x3C, 0x80)
    headers[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", headers, 0x84, 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = 0x98
    struct.pack_into("<H", headers, opt, 0x20B)
    ddir = opt + 112
    if with_debug:
        struct.pack_into("<II", headers, ddir + 6 * 8, 0x2000, 28)
    sec = opt + 240
    headers[sec:sec + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", headers, sec + 8, 0x200, 0x2000, 0x200, 0x400)
    body = bytearray(0x200)
    d1, d2, d3 = struct.unpack(">IHH", guid[:8])  # the debug id prints the fields of the GUID record in this order
    if with_debug:
        struct.pack_into("<IIHHIIII", body, 0, 0, 0, 0, 0, 2, 0x19, 0x2040, 0x440)
        body[0x40:0x44] = b"RSDS"
        struct.pack_into("<IHH", body, 0x44, d1, d2, d3)
        body[0x4C:0x54] = guid[8:]
        struct.pack_into("<I", body, 0x54, age)
    return bytes(headers + body)


def write_fake_dump_syms(path):
    path.write_text(f"#!{sys.executable}\nimport sys\nprint({SYM!r}, end='')\n")
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


class PeDebugId(unittest.TestCase):
    def test_guid_and_age(self):
        self.assertEqual(make_symbols.pe_debug_id(fake_pe()), "1FED61C55AAD5CA89204343923E044E11")

    def test_age_is_hex_without_padding(self):
        self.assertTrue(make_symbols.pe_debug_id(fake_pe(age=0x1A)).endswith("1A"))

    def test_no_debug_directory(self):
        self.assertIsNone(make_symbols.pe_debug_id(fake_pe(with_debug=False)))

    def test_not_a_pe(self):
        self.assertIsNone(make_symbols.pe_debug_id(b"hello"))


class Rewrite(unittest.TestCase):
    def test_source_root_becomes_repo_relative(self):
        out = make_symbols.rewrite_files(SYM, ["D:/a/fs/fs"])
        self.assertIn("FILE 0 src/video.cpp\n", out)
        self.assertIn("FILE 1 C:/msys64/ucrt64/include/c++/vector\n", out)

    def test_drive_letter_case_is_ignored(self):
        self.assertIn("FILE 0 src/video.cpp", make_symbols.rewrite_files(SYM, ["d:/a/fs/fs/"]))


class Build(unittest.TestCase):
    def run_build(self, require=()):
        tmp = pathlib.Path(tempfile.mkdtemp())
        staging, build = tmp / "staging", tmp / "build"
        (staging / "tools").mkdir(parents=True)
        build.mkdir()
        (staging / "Sunshine.exe").write_bytes(fake_pe())          # the shipped (stripped) image
        (build / "sunshine.exe").write_bytes(fake_pe() + b"\0" * 4096)  # the unstripped twin: same GUID, bigger
        (staging / "tools" / "other.dll").write_bytes(fake_pe(guid=bytes(range(16))))  # no twin, and dump_syms output has FUNCs anyway
        (staging / "tools" / "plain.dll").write_bytes(fake_pe(with_debug=False))      # not ours: no GUID
        tool = tmp / "dump_syms"
        write_fake_dump_syms(tool)
        out = tmp / "out" / "Symbols.zip"
        argv = ["--staging", str(staging), "--build", str(build), "--dump-syms", str(tool), "--output", str(out), "--tag", "v1", "--commit", "abc",
                "--source-root", "D:\\a\\fs\\fs"]
        for r in require:
            argv += ["--require", r]
        code = make_symbols.main(argv)
        return code, out

    def test_layout_and_manifest(self):
        code, out = self.run_build(require=["Sunshine.exe"])
        self.assertEqual(code, 0)
        with zipfile.ZipFile(out) as z:
            names = set(z.namelist())
            self.assertIn("Sunshine.exe/1FED61C55AAD5CA89204343923E044E11/Sunshine.exe.sym", names)
            self.assertNotIn("plain.dll", " ".join(names))
            sym = z.read("Sunshine.exe/1FED61C55AAD5CA89204343923E044E11/Sunshine.exe.sym").decode()
            manifest = json.loads(z.read("symbols-manifest.json"))
        self.assertTrue(sym.startswith("MODULE windows x86_64 1FED61C55AAD5CA89204343923E044E11 Sunshine.exe\n"))
        self.assertIn("FILE 0 src/video.cpp", sym)
        self.assertEqual([m["name"] for m in manifest["modules"]], ["Sunshine.exe", "other.dll"])
        self.assertEqual(manifest["commit"], "abc")

    def test_strip_staged_runs_after_the_zip_is_written(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        staging = tmp / "staging"
        staging.mkdir()
        (staging / "Sunshine.exe").write_bytes(fake_pe())
        tool, strip, log = tmp / "dump_syms", tmp / "strip", tmp / "strip.log"
        write_fake_dump_syms(tool)
        strip.write_text(f"#!{sys.executable}\nimport sys\nopen({str(log)!r}, 'a').write(' '.join(sys.argv[1:]) + '\\n')\n")
        strip.chmod(strip.stat().st_mode | stat.S_IEXEC)
        out = tmp / "Symbols.zip"
        code = make_symbols.main(["--staging", str(staging), "--build", str(tmp), "--dump-syms", str(tool), "--output", str(out),
                                  "--strip-staged", "--strip-tool", str(strip)])
        self.assertEqual(code, 0)
        self.assertTrue(out.exists())
        self.assertEqual(log.read_text().strip(), f"--strip-debug {staging / 'Sunshine.exe'}")

    def test_nothing_is_stripped_when_a_required_module_is_missing(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        staging = tmp / "staging"
        staging.mkdir()
        (staging / "Sunshine.exe").write_bytes(fake_pe())
        tool, log = tmp / "dump_syms", tmp / "strip.log"
        write_fake_dump_syms(tool)
        code = make_symbols.main(["--staging", str(staging), "--build", str(tmp), "--dump-syms", str(tool), "--output", str(tmp / "o.zip"),
                                  "--require", "nope.exe", "--strip-staged", "--strip-tool", str(tmp / "no-such-strip")])
        self.assertEqual(code, 1)
        self.assertFalse(log.exists())

    def test_a_required_module_without_symbols_fails_the_build(self):
        code, _ = self.run_build(require=["sunshinesvc.exe"])
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
