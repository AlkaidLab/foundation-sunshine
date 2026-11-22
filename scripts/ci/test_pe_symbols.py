"""Unit tests for tools/pe_symbols.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import importlib.util
import json
import pathlib
import struct
import unittest

from test_make_symbols import fake_pe

spec = importlib.util.spec_from_file_location("pe_symbols", pathlib.Path(__file__).parents[2] / "tools" / "pe_symbols.py")
pe_symbols = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pe_symbols)

symbols_spec = importlib.util.spec_from_file_location("make_symbols", pathlib.Path(__file__).with_name("make-symbols.py"))
make_symbols = importlib.util.module_from_spec(symbols_spec)
symbols_spec.loader.exec_module(make_symbols)


def without_characteristics(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    return data[:pe + 22] + b"\0\0" + data[pe + 24:]


class Strip(unittest.TestCase):
    def test_symbol_table_is_removed_and_the_debug_id_stays(self):
        before = fake_pe(symbols=4)
        after, saved, action = pe_symbols.strip_symbols(before)
        self.assertEqual((saved, action), (18 * 4 + 4, "stripped"))
        self.assertEqual(len(after), len(fake_pe()))
        self.assertEqual(without_characteristics(after), without_characteristics(fake_pe()))
        self.assertEqual(make_symbols.pe_debug_id(before), make_symbols.pe_debug_id(after))

    def test_header_fields_and_flag(self):
        after, _, _ = pe_symbols.strip_symbols(fake_pe(symbols=2))
        pe = struct.unpack_from("<I", after, 0x3C)[0]
        self.assertEqual(struct.unpack_from("<II", after, pe + 12), (0, 0))
        characteristics = struct.unpack_from("<H", after, pe + 22)[0]
        self.assertTrue(characteristics & 0x0200)

    def test_no_symbol_table_is_left_alone(self):
        before = fake_pe()
        self.assertEqual(pe_symbols.strip_symbols(before), (before, 0, "no-symbols"))
        self.assertEqual(pe_symbols.strip_symbols(fake_pe(dwarf="short")), (fake_pe(dwarf="short"), 0, "no-symbols"))

    def test_a_signed_image_is_left_alone_unless_forced(self):
        before = fake_pe(signed=True, symbols=2)
        self.assertEqual(pe_symbols.strip_symbols(before), (before, 0, "signed"))
        after, saved, action = pe_symbols.strip_symbols(before, force_signed=True)
        self.assertEqual(action, "stripped")
        self.assertEqual(saved, 18 * 2 + 4)

    def test_four_directories_do_not_make_a_certificate_table(self):
        # The bytes at the certificate-directory position can be the first
        # section header; NumberOfRvaAndSizes keeps them from reading as one.
        data = bytearray(fake_pe(signed=True))
        struct.pack_into("<I", data, 0x98 + 108, 4)
        self.assertFalse(pe_symbols.is_signed(bytes(data)))

    def test_the_declared_certificate_directory_is_checked_against_the_header(self):
        data = bytearray(fake_pe(signed=True))
        struct.pack_into("<H", data, 0x94, 144)  # optional header ends after four directories
        struct.pack_into("<I", data, 0x98 + 108, 5)  # but the count claims a fifth
        with self.assertRaises(pe_symbols.PEError):
            pe_symbols.is_signed(bytes(data))

    def test_an_overlay_is_kept(self):
        before = fake_pe(symbols=2) + b"payload"
        self.assertEqual(pe_symbols.strip_symbols(before), (before, 0, "overlay"))

    def test_alignment_padding_after_the_string_table_is_removed(self):
        before = fake_pe(symbols=2) + b"\0" * 16
        after, saved, action = pe_symbols.strip_symbols(before)
        self.assertEqual((action, saved), ("stripped", 18 * 2 + 4 + 16))
        self.assertEqual(without_characteristics(after), without_characteristics(fake_pe()))

    def test_a_damaged_pe_is_rejected(self):
        with self.assertRaises(pe_symbols.PEError):
            pe_symbols.strip_symbols(b"not a PE")

    def test_check_mode_fails_on_an_unsigned_symbol_table_and_allows_a_signed_one(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            with_symbols = pathlib.Path(directory) / "with.exe"
            with_symbols.write_bytes(fake_pe(symbols=1))
            stripped = pathlib.Path(directory) / "stripped.exe"
            stripped.write_bytes(fake_pe())
            signed = pathlib.Path(directory) / "signed.exe"
            signed.write_bytes(fake_pe(signed=True, symbols=1))
            self.assertEqual(pe_symbols.main(["--check", str(with_symbols)]), 1)
            self.assertEqual(pe_symbols.main(["--check", str(stripped)]), 0)
            self.assertEqual(pe_symbols.main(["--check", str(signed)]), 0)

    def test_check_mode_allows_an_unsigned_overlay_and_reports_it(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            overlay = pathlib.Path(directory) / "overlay.exe"
            overlay.write_bytes(fake_pe(symbols=1) + b"payload")
            report = pathlib.Path(directory) / "report.json"
            self.assertEqual(pe_symbols.main(["--check", "--json", str(report), str(overlay)]), 0)
            self.assertEqual(json.loads(report.read_text())[0]["action"], "overlay")


if __name__ == "__main__":
    unittest.main()
