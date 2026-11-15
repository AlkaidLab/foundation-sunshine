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
"""

import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
import zipfile

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
    staged = []
    for p in sorted(pathlib.Path(args.staging).rglob("*")):
        if p.suffix.lower() in (".exe", ".dll") and p.is_file():
            did = pe_debug_id(p.read_bytes())
            if did:
                staged.append((p, did))
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
    missing = [r for r in args.require if not any(m["name"].lower() == r.lower() for m in modules)]
    manifest = {"schema": 1, "tag": args.tag, "commit": args.commit, "repository": args.repository, "modules": modules, "skipped": skipped}
    out = pathlib.Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, blob in sorted(entries.items()):
            z.writestr(name, blob)
        z.writestr("symbols-manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False))
    if args.strip_staged and not missing:
        # after the symbols are safe in the zip: what ships carries no DWARF (the build-id stays)
        for p in done:
            subprocess.run([args.strip_tool, "--strip-debug", str(p)], check=True)
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
    ap.add_argument("--strip-staged", action="store_true", help="run strip --strip-debug on the staged modules that got symbols")
    ap.add_argument("--strip-tool", default="strip")
    args = ap.parse_args(argv)
    manifest, missing = build(args)
    for m in manifest["modules"]:
        print(f"{m['name']}  {m['debug_id']}  {m['functions']} functions  {m['bytes']} bytes")
    for s in manifest["skipped"]:
        print(f"skipped {s['name']} {s['debug_id']}: {s['reason']}")
    if missing:
        print(f"::error title=Symbols::no symbols for required module(s): {', '.join(missing)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
