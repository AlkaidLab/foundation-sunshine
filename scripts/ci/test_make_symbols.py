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


def fake_pe(guid=GUID, age=1, with_debug=True, dwarf=None, signed=False, symbols=0):
    """The smallest PE32+ image that carries one CodeView debug directory entry in a section.
    dwarf: None, "short" (a section named .debug_a, 8 characters at most) or "long" (/4, resolved through the COFF string table); signed: an Authenticode entry in the data directory; symbols: COFF symbol records appended at the end."""
    nsections = 2 if dwarf else 1
    headers = bytearray(0x400)
    headers[:2] = b"MZ"
    struct.pack_into("<I", headers, 0x3C, 0x80)
    headers[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", headers, 0x84, 0x8664, nsections, 0, 0x700 if dwarf == "long" else 0, 0, 240, 0x22)
    opt = 0x98
    struct.pack_into("<H", headers, opt, 0x20B)
    struct.pack_into("<I", headers, opt + 108, 16)  # NumberOfRvaAndSizes
    ddir = opt + 112
    if with_debug:
        struct.pack_into("<II", headers, ddir + 6 * 8, 0x2000, 28)
    if signed:
        struct.pack_into("<II", headers, ddir + 4 * 8, 0x800, 0x40)
    sec = opt + 240
    headers[sec:sec + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", headers, sec + 8, 0x200, 0x2000, 0x200, 0x400)
    if dwarf:
        headers[sec + 40:sec + 48] = (b"/4\0\0\0\0\0\0" if dwarf == "long" else b".debug_a")
        struct.pack_into("<IIII", headers, sec + 40 + 8, 0x100, 0x3000, 0x100, 0x600)
    body = bytearray(0x200)
    d1, d2, d3 = struct.unpack(">IHH", guid[:8])  # the debug id prints the fields of the GUID record in this order
    if with_debug:
        struct.pack_into("<IIHHIIII", body, 0, 0, 0, 0, 0, 2, 0x19, 0x2040, 0x440)
        body[0x40:0x44] = b"RSDS"
        struct.pack_into("<IHH", body, 0x44, d1, d2, d3)
        body[0x4C:0x54] = guid[8:]
        struct.pack_into("<I", body, 0x54, age)
    out = bytes(headers + body)
    if dwarf:
        out += b"\0" * 0x100  # the raw data of the debug section at 0x600
        if dwarf == "long":
            names = b".debug_line\0"
            out += struct.pack("<I", 4 + len(names)) + names  # COFF string table at 0x700 (no symbols: it follows the section data)
    if symbols:
        pointer = len(out)
        out += b"\0" * (18 * symbols)  # symbol records: the content is irrelevant, only the table range is read
        out += struct.pack("<I", 4)  # empty COFF string table
        out = bytearray(out)
        struct.pack_into("<II", out, 0x84 + 8, pointer, symbols)
        out = bytes(out)
    return out


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


class DebugSections(unittest.TestCase):
    def test_short_and_long_section_names(self):
        self.assertTrue(make_symbols.has_dwarf(fake_pe(dwarf="short")))
        self.assertTrue(make_symbols.has_dwarf(fake_pe(dwarf="long")))
        self.assertEqual(make_symbols.section_names(fake_pe(dwarf="long")), [".rdata", ".debug_line"])
        self.assertFalse(make_symbols.has_dwarf(fake_pe()))

    def test_signature(self):
        self.assertTrue(make_symbols.is_signed(fake_pe(signed=True)))
        self.assertFalse(make_symbols.is_signed(fake_pe()))


class Rewrite(unittest.TestCase):
    def test_source_root_becomes_repo_relative(self):
        out = make_symbols.rewrite_files(SYM, ["D:/a/fs/fs"])
        self.assertIn("FILE 0 src/video.cpp\n", out)
        self.assertIn("FILE 1 C:/msys64/ucrt64/include/c++/vector\n", out)

    def test_drive_letter_case_is_ignored(self):
        self.assertIn("FILE 0 src/video.cpp", make_symbols.rewrite_files(SYM, ["d:/a/fs/fs/"]))


class StripPolicy(unittest.TestCase):
    """No debug information ships that the symbol zip does not have, and a signature is never broken."""

    def run_policy(self, files):
        tmp = pathlib.Path(tempfile.mkdtemp())
        staging = tmp / "staging"
        staging.mkdir()
        for name, data in files.items():
            (staging / name).write_bytes(data)
        tool, strip, log = tmp / "dump_syms", tmp / "strip", tmp / "strip.log"
        write_fake_dump_syms(tool)
        strip.write_text(f"#!{sys.executable}\nimport sys\nopen({str(log)!r}, 'a').write(sys.argv[-1].rsplit('/', 1)[-1] + '\\n')\n")
        strip.chmod(strip.stat().st_mode | stat.S_IEXEC)
        out = tmp / "Symbols.zip"
        code = make_symbols.main(["--staging", str(staging), "--build", str(tmp), "--dump-syms", str(tool), "--output", str(out), "--strip-staged", "--strip-tool", str(strip)])
        stripped = sorted(log.read_text().split()) if log.exists() else []
        with zipfile.ZipFile(out) as z:
            manifest = json.loads(z.read("symbols-manifest.json"))
        return code, stripped, manifest, staging

    def test_a_module_with_symbols_is_stripped_a_signed_one_is_not(self):
        code, stripped, m, _ = self.run_policy({"Sunshine.exe": fake_pe(dwarf="short"), "Driver.dll": fake_pe(guid=bytes(range(16)), dwarf="short", signed=True)})
        self.assertEqual((code, stripped), (0, ["Sunshine.exe"]))
        self.assertEqual(m["kept_signed_with_debug_info"], ["Driver.dll"])
        self.assertEqual(m["stripped_without_symbols"], [])

    def test_unsymbolised_modules_with_dwarf_are_stripped_too_and_listed(self):
        code, stripped, m, _ = self.run_policy({"Sunshine.exe": fake_pe(dwarf="short"), "noguid.dll": fake_pe(with_debug=False, dwarf="long"), "clean.dll": fake_pe(with_debug=False)})
        self.assertEqual(stripped, ["Sunshine.exe", "noguid.dll"])
        self.assertEqual(m["stripped_without_symbols"], ["noguid.dll"])

    def test_a_coff_symbol_table_without_dwarf_is_removed(self):
        code, stripped, m, staging = self.run_policy({"Sunshine.exe": fake_pe(symbols=3), "clean.dll": fake_pe()})
        self.assertEqual(code, 0)
        self.assertEqual(stripped, [])  # no DWARF, so the strip tool is not needed
        self.assertEqual(m["stripped_coff_symbols"], [{"name": "Sunshine.exe", "bytes": 18 * 3 + 4}])
        self.assertEqual((staging / "Sunshine.exe").stat().st_size, len(fake_pe()))

    def test_a_signed_coff_symbol_table_is_kept_and_listed(self):
        code, stripped, m, staging = self.run_policy({"Driver.dll": fake_pe(signed=True, symbols=2)})
        self.assertEqual(code, 0)
        self.assertEqual(stripped, [])
        self.assertEqual(m["stripped_coff_symbols"], [])
        self.assertEqual(m["kept_signed_with_coff_symbols"], ["Driver.dll"])
        self.assertEqual((staging / "Driver.dll").stat().st_size, len(fake_pe(signed=True, symbols=2)))

    def test_the_dwarf_strip_runs_before_the_coff_table_is_removed(self):
        # GNU strip resolves long section names through the COFF string table; the table must still be there when it
        # runs. The fake strip records the size it was handed.
        tmp = pathlib.Path(tempfile.mkdtemp())
        staging = tmp / "staging"
        staging.mkdir()
        image = fake_pe(dwarf="short", symbols=2)
        (staging / "Sunshine.exe").write_bytes(image)
        tool, strip, seen = tmp / "dump_syms", tmp / "strip", tmp / "seen"
        write_fake_dump_syms(tool)
        strip.write_text(f"#!{sys.executable}\nimport os, sys\nopen({str(seen)!r}, 'w').write(str(os.path.getsize(sys.argv[-1])))\n")
        strip.chmod(strip.stat().st_mode | stat.S_IEXEC)
        code = make_symbols.main(["--staging", str(staging), "--build", str(tmp), "--dump-syms", str(tool), "--output", str(tmp / "o.zip"),
                                  "--strip-staged", "--strip-tool", str(strip)])
        self.assertEqual(code, 0)
        self.assertEqual(int(seen.read_text()), len(image))

    def test_nothing_is_stripped_when_a_required_module_is_missing(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        (tmp / "s").mkdir()
        (tmp / "s" / "a.dll").write_bytes(fake_pe(dwarf="short"))
        tool = tmp / "dump_syms"
        write_fake_dump_syms(tool)
        code = make_symbols.main(["--staging", str(tmp / "s"), "--build", str(tmp), "--dump-syms", str(tool), "--output", str(tmp / "o.zip"), "--require", "nope.exe", "--strip-staged", "--strip-tool", str(tmp / "no-such-strip")])
        self.assertEqual(code, 1)


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
        (staging / "Sunshine.exe").write_bytes(fake_pe(dwarf="short"))
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
