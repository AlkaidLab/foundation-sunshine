"""Cross-check Python observations with upstream's unchanged C++ word/field parser."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inspection',type=Path)
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.exists():raise ValueError('Output must be new')
    child=subprocess.run([str(args.executable.resolve()),str((args.inspection/'carrier.iec61937').resolve()),
                          str(args.output.resolve())],capture_output=True,timeout=20)
    if child.returncode:raise RuntimeError(child.stderr.decode(errors='replace'))
    before=list(csv.DictReader((args.inspection/'object-fields.csv').open(newline='')))
    after=list(csv.DictReader((args.output/'fields.csv').open(newline='')))
    keys=['half','object','f0','f1','f2','f3','f4','f5']
    same_fields=len(before)==len(after) and all([int(a[k]) for k in keys]==[int(b[k]) for k in keys] for a,b in zip(before,after))
    same_pcm=sha(args.inspection/'fullband-31ch-48k.pcm16')==sha(args.output/'fullband-31ch-48k.pcm16')
    proof=dict(reference_commit='5c689815680f3ea5b09fb35ea4dbe63a3dc5bfb7',
        upstream_stdout=child.stdout.decode(),pcm_bytes_identical=same_pcm,
        pcm_sha256=sha(args.output/'fullband-31ch-48k.pcm16'),raw_field_rows_identical=same_fields,
        field_rows=len(after),passed=bool(same_pcm and same_fields),
        scope='Independent language implementations of the same observed layout. Not Dolby conformance certification.')
    (args.output/'proof.json').write_text(json.dumps(proof,indent=2),encoding='utf8')
    print(json.dumps(proof,indent=2));return 0 if proof['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
