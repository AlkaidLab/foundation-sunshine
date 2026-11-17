#!/usr/bin/env python3
"""Extract a fixed, hash-bound distribution; never execute either installer.

Sources: usbip-slim-innounp-20261117 (Usage), usbip-slim-ms-expand-20261117
(/f), usbip-slim-catalog-20261117 (unchanged package members).
The cabinet offsets below describe ONLY the pinned redistributable, not a
general-purpose Burn parser. Changing the pin requires a new payload review.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile


INNOUNP_URL = ('https://github.com/jrathlev/InnoUnpacker-Windows-GUI/'
               'releases/download/ui_2_2_13/innounp-2.zip')
INNOUNP_SHA256 = 'f8e45c9676bebc7d7570108ab572673b541fcf7a7d3d24a04319ac6b5315b4e8'
REDIST_SHA256 = '843068991daaa1f73ad9f6239bce4d0f6a07a51f18c37ea2a867e9beca71295c'


def checked(path, digest):
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError('SHA-256 mismatch: ' + path.name)
    return path


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def download(url, target, digest):
    if not target.exists():
        with urllib.request.urlopen(url, timeout=60) as response:
            target.write_bytes(response.read())
    return checked(target, digest)


def verify_payload(root, manifest):
    for name, record in manifest['files'].items():
        checked(root / name, record['sha256'])
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    allowed = set(manifest['files']) | {'payload.json', 'LICENSE'}
    if actual - allowed:
        raise ValueError('Unexpected payload files: ' + ', '.join(sorted(actual - allowed)))


def extract(installer, output, scratch, wine=None):
    here = Path(__file__).resolve().parent
    manifest_path = here / 'usbip-payload.json'
    manifest = json.loads(manifest_path.read_text())
    checked(installer, manifest['installer_sha256'])
    archive = download(INNOUNP_URL, scratch / 'innounp.zip', INNOUNP_SHA256)
    unpacker = scratch / 'innounp.exe'
    with zipfile.ZipFile(archive) as z:
        unpacker.write_bytes(z.read('innounp.exe'))
    source = scratch / 'source'
    source.mkdir()
    if wine:
        os.environ['WINEPREFIX'] = str(scratch.parent / 'wine')
        os.environ['WINEDEBUG'] = '-all'
        os.environ['WINEDLLOVERRIDES'] = 'mscoree,mshtml='
        def winpath(p):
            return 'Z:' + str(p.resolve()).replace('/', '\\')
        run(wine, unpacker, '-x', '-y', '-d' + winpath(source), winpath(installer))
    else:
        run(unpacker, '-x', '-y', '-d' + str(source), installer)
    # All six files in the upstream driver package are preserved byte-for-byte.
    members = {p.name for p in (source / '{tmp}').iterdir() if p.name != 'vc_redist.x64.exe'}
    if members != {Path(k).name for k in manifest['files'] if k.startswith('driver/')}:
        raise ValueError('The upstream driver package has changed')
    redist = checked(source / '{tmp}' / 'vc_redist.x64.exe', REDIST_SHA256)
    attached = scratch / 'attached.cab'
    # Fixed offsets in the hash-verified upstream vc_redist.x64.exe (14.51).
    attached.write_bytes(redist.read_bytes()[630000:630000 + 18091661])
    minimum = scratch / 'minimum'
    runtime = scratch / 'runtime'
    minimum.mkdir()
    runtime.mkdir()
    if os.name == 'nt':
        expand = Path(os.environ['SystemRoot']) / 'System32' / 'expand.exe'
        run(expand, '-F:a4', attached, minimum)
        run(expand, '-F:*', minimum / 'a4', runtime)
    else:
        run('7z', 'x', '-y', attached, '-o' + str(minimum), 'a4')
        run('7z', 'x', '-y', minimum / 'a4', '-o' + str(runtime))
    output.mkdir(parents=True, exist_ok=True)
    for name, record in manifest['files'].items():
        origin = record['source']
        src = runtime / origin[8:] if origin.startswith('runtime/') else source / origin
        checked(src, record['sha256'])
        dst = output / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
    shutil.copyfile(manifest_path, output / 'payload.json')
    shutil.copyfile(here.parents[1] / 'third_party/licenses/usbip-win2/LICENSE', output / 'LICENSE')
    verify_payload(output, manifest)
    if os.name == 'nt':
        run('powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            here / 'verify-usbip-payload.ps1', '-Payload', output)
    print(json.dumps({'files': len(manifest['files']),
                      'payload_bytes': sum(p.stat().st_size for p in output.rglob('*') if p.is_file()),
                      'installer_bytes': installer.stat().st_size}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    parser.add_argument('--wine', help='Disposable Linux extraction only; signatures require Windows')
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='usbip-', dir=args.scratch) as temp:
        extract(args.installer.resolve(), args.output.resolve(), Path(temp), args.wine)


if __name__ == '__main__':
    main()
