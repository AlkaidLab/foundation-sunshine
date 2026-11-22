#!/usr/bin/env python3
"""Breakpad symbols for the PE modules of a Windows package.

usage: make-symbols.py --staging DIR --build DIR --dump-syms EXE --output ZIP [--commit SHA] [--tag TAG]
                       [--source-root DIR ...] [--require NAME ...] [--strip-staged]

The MinGW build has no PDB. `-Wl,--build-id` (cmake/compile_definitions/windows.cmake) puts a CodeView GUID into every
module, and the GUID survives `strip` and Authenticode signing, so it is the key between a crash dump and the symbols:
for every PE under --staging that carries a GUID, the unstripped PE with the same GUID is looked up under --staging
and --build, `dump_syms` reads its DWARF (functions, lines, CFI from .pdata), and the result is stored as
<name>/<DEBUG ID>/<name>.sym, the layout minidump-stackwalk and Breakpad tools read. The name is the file name in the
package, because that is the module name a crash dump records.

--source-root rewrites the build machine's source directory prefix in FILE records to a repo-relative path, so a tool
can link a frame to the exact file and line of the commit.

--strip-staged removes, from the staged copies only, both the DWARF sections and the COFF symbol table of every unsigned
module. The symbol zip is written first and is read from the unstripped twin with the same debug id; neither kind of
symbol table is needed at run time. An Authenticode-signed module keeps its bytes (changing them would invalidate the
signature) and is listed in symbols-manifest.json.

Sources: fs-ms-symbols-pdb-20261122 (symbol files and the module key) and
fs-ms-pe-coff-symbols-20261122 (the COFF symbol table).
"""

import argparse
import hashlib
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import pe_symbols

IMAGE_DEBUG_TYPE_CODEVIEW = 2


def pe_debug_id(data):
    """Breakpad debug id (GUID in dump order + age, upper case) from the CodeView record of a PE image, or None."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    (pe,) = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    ddir = opt + (112 if magic == 0x20B else 96)
    if magic not in (0x10B, 0x20B):
        return None
    debug_rva, debug_size = struct.unpack_from("<II", data, ddir + 6 * 8)
    if not debug_rva or not debug_size:
        return None
    sections = []
    for i in range(nsec):
        off = opt + optsize + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def to_offset(rva):
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rawptr + (rva - vaddr)
        return None

    start = to_offset(debug_rva)
    if start is None:
        return None
    for i in range(debug_size // 28):
        e = start + i * 28
        if e + 28 > len(data):
            break
        kind, = struct.unpack_from("<I", data, e + 12)
        rawptr, = struct.unpack_from("<I", data, e + 24)
        if kind == IMAGE_DEBUG_TYPE_CODEVIEW and data[rawptr:rawptr + 4] == b"RSDS":
            d1, d2, d3 = struct.unpack_from("<IHH", data, rawptr + 4)
            d4 = data[rawptr + 12:rawptr + 20]
            age, = struct.unpack_from("<I", data, rawptr + 20)
            return f"{d1:08X}{d2:04X}{d3:04X}{d4.hex().upper()}{age:X}"
    return None


def _coff(data):
    """(offset of the section table, number of sections, optional header offset, optional header size) of a PE image, or None."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    (pe,) = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    return pe, nsec, pe + 24, optsize


def section_names(data):
    """Section names of a PE image; a long name ("/4": an offset into the COFF string table) is resolved."""
    c = _coff(data)
    if not c:
        return []
    pe, nsec, opt, optsize = c
    symptr, nsyms = struct.unpack_from("<II", data, pe + 8 + 4)
    names = []
    for i in range(nsec):
        raw = data[opt + optsize + i * 40: opt + optsize + i * 40 + 8].rstrip(b"\0").decode("latin1")
        if raw.startswith("/") and raw[1:].isdigit() and symptr:
            at = symptr + nsyms * 18 + int(raw[1:])
            end = data.find(b"\0", at, at + 256)
            if end > at:
                raw = data[at:end].decode("latin1")
        names.append(raw)
    return names


def has_dwarf(data):
    return any(n.startswith(".debug_") for n in section_names(data))


def is_signed(data):
    """An Authenticode signature is in the file: stripping it would invalidate the signature."""
    c = _coff(data)
    if not c:
        return False
    _, _, opt, _ = c
    magic, = struct.unpack_from("<H", data, opt)
    ddir = opt + (112 if magic == 0x20B else 96)
    if magic not in (0x10B, 0x20B):
        return False
    _, size = struct.unpack_from("<II", data, ddir + 4 * 8)
    return size > 0


def normalise(path):
    return path.replace("\\", "/")


def rewrite_files(sym_text, roots):
    """FILE records with a source root prefix become repo-relative; everything else is kept as written."""
    prefixes = [normalise(r).rstrip("/") + "/" for r in roots if r]
    out = []
    for line in sym_text.splitlines():
        if line.startswith("FILE "):
            num, _, path = line[5:].partition(" ")
            norm = normalise(path)
            for p in prefixes:
                if norm.lower().startswith(p.lower()):
                    norm = norm[len(p):]
                    break
            line = f"FILE {num} {norm}"
        out.append(line)
    return "\n".join(out) + "\n"


def run_dump_syms(tool, pe):
    res = subprocess.run([tool, str(pe)], capture_output=True, check=False)
    if res.returncode != 0:
        raise RuntimeError(f"dump_syms failed for {pe.name}: {res.stderr.decode('utf-8', 'replace')[-400:]}")
    return res.stdout.decode("utf-8", "replace")


def read_head(path, n=1 << 20):
    with open(path, "rb") as f:
        return f.read(n)


def collect(dirs):
    """debug id -> [paths] for every PE under the directories."""
    found = {}
    for d in dirs:
        for p in sorted(pathlib.Path(d).rglob("*")):
            if p.suffix.lower() not in (".exe", ".dll") or not p.is_file():
                continue
            did = pe_debug_id(p.read_bytes()) if p.stat().st_size < (1 << 31) else None
            if did:
                found.setdefault(did, []).append(p)
    return found


def build(args):
    staged_all = []  # every PE of the package: (path, debug id or None, has DWARF, signed)
    for p in sorted(pathlib.Path(args.staging).rglob("*")):
        if p.suffix.lower() in (".exe", ".dll") and p.is_file():
            data = p.read_bytes()
            staged_all.append((p, pe_debug_id(data), has_dwarf(data), is_signed(data)))
    staged = [(p, did) for p, did, _, _ in staged_all if did]
    candidates = collect([args.staging, args.build])
    modules, skipped = [], []
    entries = {}
    done = []
    for p, did in staged:
        name = p.name
        best = max(candidates.get(did, [p]), key=lambda c: c.stat().st_size)
        text = run_dump_syms(args.dump_syms, best)
        funcs = sum(1 for l in text.splitlines() if l.startswith("FUNC "))
        if funcs == 0:
            skipped.append({"name": name, "debug_id": did, "reason": "no DWARF functions in " + best.name})
            continue
        text = re.sub(r"^MODULE windows x86_64 \S+ ?.*$", f"MODULE windows x86_64 {did} {name}", text, count=1, flags=re.M)
        text = rewrite_files(text, args.source_root)
        entries[f"{name}/{did}/{name}.sym"] = text.encode("utf-8")
        modules.append({"name": name, "debug_id": did, "functions": funcs, "bytes": len(entries[f"{name}/{did}/{name}.sym"]),
                        "sha256": hashlib.sha256(entries[f"{name}/{did}/{name}.sym"]).hexdigest()})
        done.append(p)
    # What still carries DWARF or a COFF symbol table in the package: stripped if it is not signed (a signed file keeps
    # its signature and its symbol data, and is listed), with or without symbols; the package must not ship debug
    # information the symbol zip does not have.
    to_strip, kept_signed, without_symbols = [], [], []
    for p, did, dwarf, signed in staged_all:
        if not dwarf:
            continue
        if signed:
            kept_signed.append(p.name)
        else:
            to_strip.append(p)
            if p not in done:
                without_symbols.append(p.name)
    missing = [r for r in args.require if not any(m["name"].lower() == r.lower() for m in modules)]
    manifest = {"schema": 1, "tag": args.tag, "commit": args.commit, "repository": args.repository, "modules": modules, "skipped": skipped,
                "stripped_without_symbols": without_symbols, "kept_signed_with_debug_info": kept_signed,
                "stripped_coff_symbols": [], "kept_signed_with_coff_symbols": [], "coff_symbol_errors": []}
    out = pathlib.Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, blob in sorted(entries.items()):
            z.writestr(name, blob)
        z.writestr("symbols-manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False))
    if args.strip_staged and not missing:
        # After the symbols are safe in the zip: what ships carries no DWARF and no COFF symbol table (the build-id
        # stays). The DWARF strip runs first because GNU strip reads the COFF string table to resolve long section
        # names; truncating that table first makes strip fail on an image with debug sections. A signed file is left
        # byte for byte as it is.
        for p in to_strip:
            subprocess.run([args.strip_tool, "--strip-debug", str(p)], check=True)
        for p, did, dwarf, signed in staged_all:
            try:
                data = p.read_bytes()
                span = pe_symbols.symbol_table(data)
            except (OSError, pe_symbols.PEError) as error:
                manifest["coff_symbol_errors"].append({"name": p.name, "reason": str(error)})
                continue
            if span is None:
                continue
            if signed:
                manifest["kept_signed_with_coff_symbols"].append(p.name)
                continue
            try:
                cleaned, saved, action = pe_symbols.strip_symbols(data)
            except pe_symbols.PEError as error:
                manifest["coff_symbol_errors"].append({"name": p.name, "reason": str(error)})
                continue
            if saved:
                p.write_bytes(cleaned)
                manifest["stripped_coff_symbols"].append({"name": p.name, "bytes": saved})
        # The first zip was written before stripping so a failure cannot lose the symbols; write it again with the
        # actual cleanup result in symbols-manifest.json, replacing the first copy only after the new one is complete.
        if manifest["stripped_coff_symbols"] or manifest["kept_signed_with_coff_symbols"] or manifest["coff_symbol_errors"]:
            temporary = out.with_name(out.name + ".rewrite")
            with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                for name, blob in sorted(entries.items()):
                    z.writestr(name, blob)
                z.writestr("symbols-manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False))
            os.replace(temporary, out)
    return manifest, missing


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--dump-syms", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--commit", default="")
    ap.add_argument("--tag", default="")
    ap.add_argument("--repository", default="")
    ap.add_argument("--source-root", action="append", default=[])
    ap.add_argument("--require", action="append", default=[])
    ap.add_argument("--strip-staged", action="store_true",
                    help="remove the COFF symbol table of every unsigned staged module and strip the DWARF sections")
    ap.add_argument("--strip-tool", default="strip")
    args = ap.parse_args(argv)
    manifest, missing = build(args)
    for m in manifest["modules"]:
        print(f"{m['name']}  {m['debug_id']}  {m['functions']} functions  {m['bytes']} bytes")
    for s in manifest["skipped"]:
        print(f"skipped {s['name']} {s['debug_id']}: {s['reason']}")
    if manifest["stripped_without_symbols"]:
        print("stripped, no symbols could be made: " + ", ".join(manifest["stripped_without_symbols"]))
    if manifest["kept_signed_with_debug_info"]:
        print("::warning title=Symbols::signed, so kept with their debug sections: " + ", ".join(manifest["kept_signed_with_debug_info"]))
    for entry in manifest["stripped_coff_symbols"]:
        print(f"COFF symbols removed from {entry['name']}: {entry['bytes']:,} bytes")
    if manifest["kept_signed_with_coff_symbols"]:
        print("::warning title=COFF symbols::signed, so kept with their COFF symbol table: " + ", ".join(manifest["kept_signed_with_coff_symbols"]))
    for entry in manifest["coff_symbol_errors"]:
        print(f"::warning title=COFF symbols::{entry['name']}: {entry['reason']}")
    if missing:
        print(f"::error title=Symbols::no symbols for required module(s): {', '.join(missing)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
