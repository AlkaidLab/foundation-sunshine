#!/bin/sh
# Regenerates every PNG from the SVG sources in ./svg (deterministic; rsvg-convert + Pillow optimize).
# Usage: sh render.sh   (run from anywhere; final PNGs are written next to this script)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/png"  # raw intermediates; finals are written to $HERE
render() { # name width height
  rsvg-convert -w "$2" -h "$3" "$HERE/svg/$1.svg" -o "$HERE/png/$1.raw.png"
}
render steam 600 900
render box 600 900
render steam-hero 1920 1080
render box-hero 1920 1080
render desktop-hero 1920 1080
render bigscreen-hero 1920 1080
python3 -I - "$HERE/png" <<'PY'
import sys, os, glob
from PIL import Image
d = sys.argv[1]
for raw in sorted(glob.glob(os.path.join(d, '*.raw.png'))):
    im = Image.open(raw).convert('RGB')
    out = os.path.join(os.path.dirname(d), os.path.basename(raw).replace('.raw.png', '.png'))
    im.save(out, optimize=True, compress_level=9)
    os.remove(raw)
    print(os.path.basename(out), im.size, os.path.getsize(out))
PY
rmdir "$HERE/png" 2>/dev/null || true
