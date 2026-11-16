"""Tests for the Windows release PE gate."""

import importlib.util
import pathlib
import struct
import unittest


HERE = pathlib.Path(__file__).parent
SPEC = importlib.util.spec_from_file_location("check_windows_release", HERE / "check-windows-release.py")
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


def pe_with_imports(names, stack_reserve=2 * 1024 * 1024):
    """Build a minimal PE32+ image containing one import descriptor per name."""
    pe_offset = 0x80
    optional_offset = pe_offset + 24
    section_offset = optional_offset + 0xF0
    raw_offset = 0x400
    image = bytearray(raw_offset + 0x400)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, pe_offset)
    image[pe_offset:pe_offset + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, pe_offset + 4, 0x8664, 1, 0, 0, 0, 0xF0, 0x2022)
    struct.pack_into("<H", image, optional_offset, 0x20B)
    struct.pack_into("<Q", image, optional_offset + 0x48, stack_reserve)
    struct.pack_into("<I", image, optional_offset + 108, 16)
    # Import directory: RVA 0x1000, 20-byte descriptors, then a null descriptor.
    struct.pack_into("<II", image, optional_offset + 112 + 8, 0x1000, 20 * (len(names) + 1))
    struct.pack_into("<8sIIIIIIHHI", image, section_offset, b".idata\0", 0x400, 0x1000, 0x400, raw_offset, 0, 0, 0, 0, 0xC0000040)
    cursor = raw_offset + 20 * (len(names) + 1)
    for index, name in enumerate(names):
        name_rva = 0x1000 + cursor - raw_offset
        image[cursor:cursor + len(name) + 1] = name.encode() + b"\0"
        struct.pack_into("<IIIII", image, raw_offset + index * 20, 0, 0, 0, name_rva, 0)
        cursor += len(name) + 1
    return bytes(image)


class WindowsReleaseGateTests(unittest.TestCase):
    def test_system_imports_and_stack_pass(self):
        data = pe_with_imports(["KERNEL32.dll", "api-ms-win-core-file-l1-2-0.dll"])
        self.assertEqual(CHECK.imported_dlls(data), ["kernel32.dll", "api-ms-win-core-file-l1-2-0.dll"])
        self.assertEqual(CHECK.stack_reserve(data), 2 * 1024 * 1024)
        self.assertEqual(CHECK.check.__name__, "check")

    def test_non_system_import_fails(self):
        data = pe_with_imports(["libiconv-2.dll"])
        self.assertTrue(any("libiconv-2.dll" in error for error in CHECK.check_data(data)[2]))
