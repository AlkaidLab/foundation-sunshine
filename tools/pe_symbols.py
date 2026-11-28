#!/usr/bin/env python3
"""Drop the COFF symbol table of linked Windows images without touching a section.

The linker writes the COFF symbol table after the last section of an exe or DLL.
It is not loaded by Windows and is not the debug information a crash decoder
uses: that is the CodeView record (its GUID/age is the module debug id) and, for
this project's MinGW builds, the DWARF sections. Release packages therefore
carry a private symbol zip (Symbols-x64.zip) and can ship images without the
table. Removing it costs nothing at run time and keeps the debug id.

usage: pe_symbols.py FILE... [--json REPORT] [--force-signed]
       pe_symbols.py --recursive DIR [--json REPORT]

Only the symbol table and its string table are removed. The file is truncated
at PointerToSymbolTable and the two COFF header fields are zeroed; section
data, the certificate table and any overlay are left as they are. A file with
data after the symbol table (an overlay, for example an installer with an
appended payload) is skipped, because truncating it would remove that data.
An Authenticode-signed file is skipped too: changing any byte invalidates the
signature, and the package is signed later.

The layout is the one Microsoft documents on the PE Format page
(fs-ms-pe-coff-symbols-20261122, sections "COFF Symbol Table" and "String
Table"); the CodeView record that stays is the symbol key described by
fs-ms-symbols-pdb-20261122, and fs-ms-signtool-20261122 covers the signature.
"""

import argparse
import json
import pathlib
import struct
import sys


IMAGE_FILE_DEBUG_STRIPPED = 0x0200
DIRECTORY_CERTIFICATE_TABLE = 4


class PEError(ValueError):
    """The file is not a PE image this tool understands."""


def _u16(data, offset):
    if offset < 0 or offset + 2 > len(data):
        raise PEError(f"truncated PE at 0x{offset:x}")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise PEError(f"truncated PE at 0x{offset:x}")
    return struct.unpack_from("<I", data, offset)[0]


def _layout(data):
    """(PE offset, section table offset, number of sections, optional header offset, optional header size)."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise PEError("not a DOS PE image")
    pe = _u32(data, 0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise PEError("missing PE signature")
    sections = _u16(data, pe + 6)
    optional_size = _u16(data, pe + 20)
    optional = pe + 24
    magic = _u16(data, optional)
    if magic not in (0x10B, 0x20B):
        raise PEError(f"unsupported optional header magic 0x{magic:x}")
    table = optional + optional_size
    if table + sections * 40 > len(data):
        raise PEError("section table past the end of the file")
    return pe, table, sections, optional, optional_size, magic


def is_signed(data):
    """An Authenticode certificate table is present."""
    _, _, _, optional, optional_size, magic = _layout(data)
    count_offset, directories_offset = (108, 112) if magic == 0x20B else (92, 96)
    if optional_size < directories_offset:
        return False
    # NumberOfRvaAndSizes bounds the data directories (PE Format, "Optional
    # Header Data Directories", fs-ms-pe-coff-symbols-20261122). A header that
    # declares four directories ends before the certificate entry: the bytes
    # at that position belong to the first section header, not to a signature.
    if _u32(data, optional + count_offset) <= DIRECTORY_CERTIFICATE_TABLE:
        return False
    entry = optional + directories_offset + DIRECTORY_CERTIFICATE_TABLE * 8
    if entry + 8 > optional + optional_size:
        raise PEError("certificate table directory is outside the optional header")
    _u32(data, entry)  # VirtualAddress; only the size means "present" here
    return _u32(data, entry + 4) > 0


def symbol_table(data):
    """(start, end) of the COFF symbol table and its string table, or None."""
    pe, table, sections, _, _, _ = _layout(data)
    pointer = _u32(data, pe + 12)
    count = _u32(data, pe + 16)
    if pointer == 0 or count == 0:
        return None
    if count > (1 << 24):
        raise PEError(f"implausible symbol count {count}")
    end_of_sections = 0
    for index in range(sections):
        header = table + index * 40
        raw_size = _u32(data, header + 16)
        raw_offset = _u32(data, header + 20)
        # A section whose raw data lies past the end of the file contributes no
        # bytes to it (the linker can leave such a header for data that is not
        # stored). Only the stored ranges bound the symbol table.
        if raw_offset and raw_offset + raw_size <= len(data):
            end_of_sections = max(end_of_sections, raw_offset + raw_size)
    if pointer < end_of_sections or pointer >= len(data):
        raise PEError(f"symbol table at 0x{pointer:x} is not after the section data")
    end = pointer + count * 18
    if end > len(data):
        raise PEError("symbol table past the end of the file")
    # A string table starts with its own size (including those four bytes) and
    # follows the symbol records when a section name or a long symbol name
    # needs it. A zero size means there is no table.
    if end + 4 <= len(data):
        string_size = _u32(data, end)
        if string_size >= 4 and end + string_size <= len(data):
            end += string_size
    return pointer, end


def strip_symbols(data, force_signed=False):
    """Return (new bytes, bytes saved, action).

    action is "stripped", "no-symbols", "signed" or "overlay"; only the first
    changes the bytes. Raises PEError for a malformed image.
    """
    span = symbol_table(data)
    if span is None:
        return data, 0, "no-symbols"
    if is_signed(data) and not force_signed:
        return data, 0, "signed"
    pointer, end = span
    if end != len(data):
        # Alignment padding after the string table is common; anything that is
        # not zero is an overlay (an appended payload) and must be kept.
        if any(data[end:]):
            return data, 0, "overlay"
    out = bytearray(data[:pointer])
    struct.pack_into("<I", out, _layout(data)[0] + 12, 0)
    struct.pack_into("<I", out, _layout(data)[0] + 16, 0)
    # The linker sets this flag when the image has no symbol table; keep the
    # header consistent for tools that read it.
    characteristics_at = _layout(data)[0] + 22
    characteristics = struct.unpack_from("<H", out, characteristics_at)[0]
    struct.pack_into("<H", out, characteristics_at, characteristics | IMAGE_FILE_DEBUG_STRIPPED)
    return bytes(out), len(data) - len(out), "stripped"


def strip_file(path, force_signed=False):
    path = pathlib.Path(path)
    data = path.read_bytes()
    out, saved, action = strip_symbols(data, force_signed)
    if saved:
        temporary = path.with_name(path.name + ".strip-tmp")
        temporary.write_bytes(out)
        temporary.replace(path)
    return {"file": str(path), "before": len(data), "after": len(out), "saved": saved, "action": action}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", help="PE files to strip")
    parser.add_argument("--recursive", metavar="DIR", action="append", default=[],
                        help="strip every .exe and .dll under DIR (repeatable)")
    parser.add_argument("--json", metavar="PATH", help="write the per-file report here")
    parser.add_argument("--force-signed", action="store_true", help="also strip images with an Authenticode signature")
    parser.add_argument("--check", action="store_true",
                        help="report unsigned PEs that still carry a COFF symbol table; do not modify")
    args = parser.parse_args(argv)

    targets = [pathlib.Path(p) for p in args.files]
    for directory in args.recursive:
        targets += sorted(p for p in pathlib.Path(directory).rglob("*") if p.suffix.lower() in (".exe", ".dll") and p.is_file())
    targets = [p for p in targets if p.exists()]
    if not targets:
        print("pe_symbols: no PE files given", file=sys.stderr)
        return 2

    report = []
    failed = False
    for path in targets:
        if args.check:
            try:
                data = path.read_bytes()
                # The check accepts exactly the files strip mode leaves
                # unchanged: no symbol table, a signature, or an overlay.
                _, _, action = strip_symbols(data, args.force_signed)
            except (PEError, OSError) as error:
                print(f"pe_symbols: {path}: {error}", file=sys.stderr)
                report.append({"file": str(path), "error": str(error)})
                failed = True
                continue
            report.append({"file": str(path), "action": "has-symbols" if action == "stripped" else action})
            if action == "signed":
                print(f"::warning title=COFF symbols::{path} is signed; its symbol table is kept")
            elif action == "overlay":
                print(f"::warning title=COFF symbols::{path} has data after its symbol table; left unchanged")
            elif action == "stripped":
                print(f"::error title=COFF symbols::{path} still has a COFF symbol table")
                failed = True
            continue
        try:
            entry = strip_file(path, args.force_signed)
        except (PEError, OSError) as error:
            print(f"pe_symbols: {path}: {error}", file=sys.stderr)
            report.append({"file": str(path), "error": str(error)})
            failed = True
            continue
        report.append(entry)
        if entry["action"] == "stripped":
            print(f"{path}: removed {entry['saved']:,} symbol bytes ({entry['before']:,} -> {entry['after']:,})")
        elif entry["action"] == "signed":
            print(f"::warning title=COFF symbols::{path} is signed; its symbol table is kept")
        elif entry["action"] == "overlay":
            print(f"::warning title=COFF symbols::{path} has data after its symbol table; left unchanged")
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
