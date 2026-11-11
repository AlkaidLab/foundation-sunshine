#!/usr/bin/env python3
"""Turns the module + offset frames of a crash report into function and file:line (telemetry v1 symbolisation, offline).

  symbolize.py <crash meta.json | -> --symbols <dir with the unstripped binaries of that version> [--tool llvm-symbolizer|addr2line]

Reads the meta of POST /v1/crash (or `-` for stdin). For every frame whose module file is in the symbols directory (the folder the release
recipe fills: sunshine.exe, sunshinesvc.exe, sunshine-gui.exe(+pdb), sparxie-launch.exe) it prints the function and file:line.
 * PE built with MinGW (DWARF): the offset is the address minus the image base, so VMA = ImageBase + offset (ImageBase from `objdump -p`);
   frames after the first are return addresses, so one is subtracted before the lookup.
 * ELF (Linux host): the offset is the address minus the module's lowest mapping; for a PIE that is the file address.
 * PDB next to a PE (the Rust panel): llvm-symbolizer is given the offset with --relative-address.
 * .NET (sparxie-launch.exe): a frame's offset is (metadata token << 32 | IL offset); it is printed as such, the method is named with
   ILSpy/ildasm from the same archived exe (a managed frame carries no native address).
 * Frames of Windows' own modules, the runtime or a synthetic frame (`panic:<file>`, `webview2:<kind>`, `hang-<slot>`) are printed as they are.
"""
import argparse, json, os, re, shutil, subprocess, sys

def pe_image_base(path):
    out = subprocess.run(['objdump', '-p', path], capture_output=True, text=True).stdout
    m = re.search(r'^ImageBase\s+([0-9a-fA-F]+)', out, re.M)
    return int(m.group(1), 16) if m else None

def kind_of(path):
    with open(path, 'rb') as f:
        head = f.read(4)
    return 'elf' if head == b'\x7fELF' else 'pe' if head[:2] == b'MZ' else 'other'

def run_tool(tool, path, address, relative):
    if tool == 'llvm-symbolizer':
        cmd = ['llvm-symbolizer', '--obj=' + path, '--inlining', '--demangle'] + (['--relative-address'] if relative else [])
        r = subprocess.run(cmd, input=hex(address) + '\n', capture_output=True, text=True)
        lines = [l for l in r.stdout.splitlines() if l.strip()]
        return [(lines[i], lines[i + 1] if i + 1 < len(lines) else '') for i in range(0, len(lines) - 1, 2)]
    r = subprocess.run(['addr2line', '-e', path, '-f', '-C', '-i', hex(address)], capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    return [(lines[i], lines[i + 1] if i + 1 < len(lines) else '') for i in range(0, len(lines) - 1, 2)]

def symbolize_frame(frame, index, symbols, tool):
    module, offset = frame['module'], int(frame['offset'], 16)
    path = os.path.join(symbols, module)
    if not os.path.isfile(path):
        for name in os.listdir(symbols):
            if name.lower() == module.lower():
                path = os.path.join(symbols, name)
                break
        else:
            return None
    kind = kind_of(path)
    if module.lower() == 'sparxie-launch.exe' and offset >> 32:
        return [('managed method token 0x%08x, IL offset 0x%x' % (offset >> 32, offset & 0xffffffff), '')]
    if kind == 'other':
        return None
    lookup = offset - 1 if index > 0 else offset  # a return address points after the call
    if kind == 'pe':
        pdb = os.path.splitext(path)[0] + '.pdb'
        if os.path.isfile(pdb) and shutil.which('llvm-symbolizer'):
            return run_tool('llvm-symbolizer', path, lookup, True)
        base = pe_image_base(path)
        if base is None:
            return None
        return run_tool(tool, path, base + lookup, False)
    return run_tool(tool, path, lookup, False)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('meta')
    ap.add_argument('--symbols', required=True)
    ap.add_argument('--tool', choices=['llvm-symbolizer', 'addr2line'], default='llvm-symbolizer' if shutil.which('llvm-symbolizer') else 'addr2line')
    a = ap.parse_args()
    meta = json.load(sys.stdin if a.meta == '-' else open(a.meta, encoding='utf-8'))
    print('%s %s  kind=%s  code=%s  signature=%s' % (meta.get('product'), meta.get('version'), meta.get('kind'), meta.get('exception_code'), str(meta.get('signature'))[:16]))
    for i, frame in enumerate(meta.get('frames', [])):
        resolved = None
        try:
            resolved = symbolize_frame(frame, i, a.symbols, a.tool)
        except (OSError, ValueError):
            resolved = None
        head = '#%-2d %s+%s' % (i, frame['module'], frame['offset'])
        if not resolved:
            print(head)
            continue
        for n, (func, where) in enumerate(resolved):
            print('%s  %s  %s' % (head if n == 0 else ' ' * len(head), func, where))

if __name__ == '__main__':
    main()
