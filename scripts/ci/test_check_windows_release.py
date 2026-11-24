"""Tests for the Windows release PE gate."""

import importlib.util
import pathlib
import struct
import unittest


HERE = pathlib.Path(__file__).parent
SPEC = importlib.util.spec_from_file_location("check_windows_release", HERE / "check-windows-release.py")
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


def pe_with_imports(names, stack_reserve=2 * 1024 * 1024, delay_names=(), size_of_code=0):
    """Build a minimal PE32+ image containing one import descriptor per name
    (and one delay-import descriptor per delay_names entry)."""
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
    struct.pack_into("<I", image, optional_offset + 4, size_of_code)
    struct.pack_into("<Q", image, optional_offset + 0x48, stack_reserve)
    struct.pack_into("<I", image, optional_offset + 108, 16)
    # Import directory: RVA 0x1000, 20-byte descriptors, then a null descriptor.
    struct.pack_into("<II", image, optional_offset + 112 + 8, 0x1000, 20 * (len(names) + 1))
    struct.pack_into("<8sIIIIIIHHI", image, section_offset, b".idata\0", 0x400, 0x1000, 0x400, raw_offset, 0, 0, 0, 0, 0xC0000040)
    delay_table = raw_offset + 20 * (len(names) + 1)
    if delay_names:
        struct.pack_into("<II", image, optional_offset + 112 + 13 * 8, 0x1000 + delay_table - raw_offset, 32 * (len(delay_names) + 1))
    cursor = delay_table + 32 * (len(delay_names) + 1)
    for index, name in enumerate(delay_names):
        name_rva = 0x1000 + cursor - raw_offset
        image[cursor:cursor + len(name) + 1] = name.encode() + b"\0"
        struct.pack_into("<II", image, delay_table + index * 32, 1, name_rva)
        cursor += len(name) + 1
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

    def test_delay_imports_are_read(self):
        data = pe_with_imports(["KERNEL32.dll"], delay_names=["d3d11.dll", "Libiconv-2.dll"])
        self.assertEqual(CHECK.imported_dlls(data), ["kernel32.dll"])
        self.assertEqual(CHECK.delay_imported_dlls(data), ["d3d11.dll", "libiconv-2.dll"])

    def test_system_delay_import_passes(self):
        data = pe_with_imports(["KERNEL32.dll"], delay_names=["d3d11.dll", "api-ms-win-core-file-l1-2-0.dll"])
        self.assertEqual(CHECK.check_data(data)[2], [])

    def test_non_system_delay_import_fails_when_regular_imports_pass(self):
        self.assertEqual(CHECK.check_data(pe_with_imports(["KERNEL32.dll"]))[2], [])
        errors = CHECK.check_data(pe_with_imports(["KERNEL32.dll"], delay_names=["libiconv-2.dll"]))[2]
        self.assertTrue(any("libiconv-2.dll" in error for error in errors), errors)

    def test_stack_only_checks_the_reserve_alone(self):
        # the installer shell: imports are not judged, a 1 MiB reserve (LLD default) fails, 4 MiB passes
        small = pe_with_imports(["libwinpthread-1.dll"], stack_reserve=1024 * 1024)
        large = pe_with_imports(["libwinpthread-1.dll"], stack_reserve=4 * 1024 * 1024)
        self.assertTrue(any("SizeOfStackReserve" in error for error in CHECK.check_data(small, None, True)[2]))
        self.assertEqual(CHECK.check_data(large, None, True)[2], [])
        self.assertTrue(any("libwinpthread-1.dll" in error for error in CHECK.check_data(large)[2]))

    def test_code_size_limit(self):
        small = pe_with_imports(["KERNEL32.dll"], size_of_code=900_000)
        large = pe_with_imports(["KERNEL32.dll"], size_of_code=5_300_000)
        self.assertEqual(CHECK.code_size(large), 5_300_000)
        self.assertEqual(CHECK.check_data(small, 1_500_000)[2], [])
        self.assertEqual(CHECK.check_data(large)[2], [])
        errors = CHECK.check_data(large, 1_500_000)[2]
        self.assertTrue(any("SizeOfCode" in error for error in errors), errors)

