#!/usr/bin/env python3
"""Check the PE properties required of a Windows release executable.

The RVA and optional-header offsets follow fs-ms-pe-format-20261116.
"""

import argparse
import pathlib
import struct
import sys


MIN_STACK_RESERVE = 2 * 1024 * 1024

# These are Windows platform DLLs.  API-set names are handled by prefix because
# Microsoft adds API-set contracts independently of the OS release number.
SYSTEM_DLLS = {
    "advapi32.dll",
    "avrt.dll",
    "bcrypt.dll",
    "cfgmgr32.dll",
    "combase.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "d2d1.dll",
    "d3d11.dll",
    "d3d12.dll",
    "d3dcompiler_47.dll",
    "dbghelp.dll",
    "dinput8.dll",
    "dwmapi.dll",
    "dwrite.dll",
    "dxgi.dll",
    "gdi32.dll",
    "hid.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "kernelbase.dll",
    "msimg32.dll",
    "mswsock.dll",
    "ncrypt.dll",
    "netapi32.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "opengl32.dll",
    "pdh.dll",
    "powrprof.dll",
    "psapi.dll",
    "rpcrt4.dll",
    "runtimeobject.dll",
    "sechost.dll",
    "secur32.dll",
    "setupapi.dll",
    "shell32.dll",
    "shcore.dll",
    "shlwapi.dll",
    "user32.dll",
    "userenv.dll",
    "uxtheme.dll",
    "version.dll",
    "wevtapi.dll",
    "winhttp.dll",
    "wininet.dll",
    "winmm.dll",
    "windowsapp.dll",
    "windowscodecs.dll",
    "winsta.dll",
    "wlanapi.dll",
    "wldap32.dll",
    "wintrust.dll",
    "wtsapi32.dll",
    "ws2_32.dll",
    "wsock32.dll",
    "ucrtbase.dll",
}
SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-win-")


class PEError(ValueError):
    pass


def _u16(data, offset):
    if offset < 0 or offset + 2 > len(data):
        raise PEError(f"truncated PE at 0x{offset:x}")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise PEError(f"truncated PE at 0x{offset:x}")
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data, offset):
    if offset < 0 or offset + 8 > len(data):
        raise PEError(f"truncated PE at 0x{offset:x}")
    return struct.unpack_from("<Q", data, offset)[0]


def _coff(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise PEError("not a DOS PE image")
    pe = _u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise PEError("missing PE signature")
    sections = _u16(data, pe + 6)
    optional_size = _u16(data, pe + 20)
    optional = pe + 24
    magic = _u16(data, optional)
    if magic not in (0x10B, 0x20B):
        raise PEError(f"unsupported optional header magic 0x{magic:x}")
    section_table = optional + optional_size
    entries = []
    for index in range(sections):
        header = section_table + index * 40
        virtual_size = _u32(data, header + 8)
        virtual_address = _u32(data, header + 12)
        raw_size = _u32(data, header + 16)
        raw_offset = _u32(data, header + 20)
        entries.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_to_offset(rva):
        for virtual_address, size, raw_offset, raw_size in entries:
            if virtual_address <= rva < virtual_address + size:
                delta = rva - virtual_address
                if delta >= raw_size:
                    return None
                return raw_offset + delta
        return None

    return pe, optional, magic, rva_to_offset


def _cstring(data, offset):
    if offset is None or offset < 0 or offset >= len(data):
        raise PEError("invalid string RVA")
    end = data.find(b"\0", offset)
    if end < 0:
        raise PEError("unterminated PE string")
    return data[offset:end].decode("ascii").lower()


def imported_dlls(data):
    """Return names from the PE import directory."""
    pe, optional, magic, rva_to_offset = _coff(data)
    directory_offset = optional + (96 if magic == 0x10B else 112)
    number_of_directories = _u32(data, optional + (92 if magic == 0x10B else 108))
    if number_of_directories <= 1:
        return []
    import_rva = _u32(data, directory_offset + 8)
    import_size = _u32(data, directory_offset + 12)
    if not import_rva or not import_size:
        return []
    descriptor = rva_to_offset(import_rva)
    if descriptor is None:
        raise PEError("invalid import directory RVA")
    result = []
    for offset in range(descriptor, descriptor + import_size, 20):
        if offset + 20 > len(data):
            raise PEError("truncated import descriptor")
        original_thunk, _, _, name_rva, _ = struct.unpack_from("<IIIII", data, offset)
        if not (original_thunk or name_rva):
            break
        name_offset = rva_to_offset(name_rva)
        result.append(_cstring(data, name_offset))
    return result


def delay_imported_dlls(data):
    """Return names from the PE delay-import directory (loaded on first call)."""
    pe, optional, magic, rva_to_offset = _coff(data)
    directory_offset = optional + (96 if magic == 0x10B else 112)
    number_of_directories = _u32(data, optional + (92 if magic == 0x10B else 108))
    if number_of_directories <= 13:
        return []
    delay_rva = _u32(data, directory_offset + 13 * 8)
    delay_size = _u32(data, directory_offset + 13 * 8 + 4)
    if not delay_rva or not delay_size:
        return []
    descriptor = rva_to_offset(delay_rva)
    if descriptor is None:
        raise PEError("invalid delay-import directory RVA")
    image_base = _u32(data, optional + 28) if magic == 0x10B else _u64(data, optional + 24)
    result = []
    # ImgDelayDescr is 32 bytes: grAttrs, rvaDLLName, rvaHmod, rvaIAT, rvaINT, rvaBoundIAT, rvaUnloadIAT, dwTimeStamp.
    for offset in range(descriptor, descriptor + max(delay_size, 32), 32):
        if offset + 32 > len(data):
            raise PEError("truncated delay-import descriptor")
        attributes, name_rva = struct.unpack_from("<II", data, offset)
        if not (attributes or name_rva):
            break
        if not attributes & 1:
            # Old descriptors hold virtual addresses instead of RVAs.
            name_rva = (name_rva - image_base) & 0xFFFFFFFF
        result.append(_cstring(data, rva_to_offset(name_rva)))
    return result


def stack_reserve(data):
    """Read SizeOfStackReserve from the PE optional header."""
    _, optional, magic, _ = _coff(data)
    # Both PE32 and PE32+ place SizeOfStackReserve at offset 0x48. The field
    # width follows the optional-header magic.
    return _u32(data, optional + 0x48) if magic == 0x10B else _u64(data, optional + 0x48)


def code_size(data):
    """Read SizeOfCode (the sum of the code sections) from the PE optional header."""
    _, optional, _, _ = _coff(data)
    return _u32(data, optional + 4)


def check_data(data, max_code_bytes=None, stack_only=False):
    # stack_only: the installer shell links Direct2D/DirectWrite statically-imported system DLLs that the helper list does not carry;
    # for it only the stack reserve is a release property (issue #1326: LLD gave it 1 MiB and its worker frame was 1 MiB).
    dlls = [] if stack_only else imported_dlls(data)
    delayed = [] if stack_only else delay_imported_dlls(data)
    non_system = [dll for dll in dlls + delayed if dll not in SYSTEM_DLLS and not dll.startswith(SYSTEM_PREFIXES)]
    reserve = stack_reserve(data)
    errors = []
    if non_system:
        errors.append("non-system imports: " + ", ".join(sorted(set(non_system))))
    if reserve < MIN_STACK_RESERVE:
        errors.append(f"SizeOfStackReserve is {reserve} bytes, expected at least {MIN_STACK_RESERVE}")
    # fs-tools-no-boost-separate-20261117: a helper linked with the wrong library (a Clang/LLD build once loaded the
    # Asio/Beast/OpenSSL object into the 1 MB display tools) keeps its imports but grows five-fold in code.
    if max_code_bytes is not None and code_size(data) > max_code_bytes:
        errors.append(f"SizeOfCode is {code_size(data)} bytes, expected at most {max_code_bytes}")
    return dlls + delayed, reserve, errors


def check(path, max_code_bytes=None, stack_only=False):
    return check_data(pathlib.Path(path).read_bytes(), max_code_bytes, stack_only)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=pathlib.Path)
    parser.add_argument("--max-code-bytes", type=int, default=None, help="fail when SizeOfCode exceeds this many bytes")
    parser.add_argument("--stack-only", action="store_true", help="check only SizeOfStackReserve (the installer shell)")
    args = parser.parse_args(argv)
    try:
        dlls, reserve, errors = check(args.executable, args.max_code_bytes, args.stack_only)
    except (OSError, PEError, UnicodeError) as error:
        print(f"release PE check failed: {error}", file=sys.stderr)
        return 1
    print(f"imports: {', '.join(dlls) if dlls else '(none)'}")
    print(f"SizeOfStackReserve: {reserve}")
    print(f"SizeOfCode: {code_size(args.executable.read_bytes())}")
    if errors:
        for error in errors:
            print(f"release PE check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
