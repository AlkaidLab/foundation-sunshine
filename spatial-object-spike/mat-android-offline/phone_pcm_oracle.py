"""Fixed-recording mono audition oracle using the pre-existing research extractor."""
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mat-spike"))
from analyze_mat import inspect_burst, PERIOD
import numpy as np

source, output = map(Path, sys.argv[1:])
raw = source.read_bytes()
if hashlib.sha256(raw).hexdigest() != "79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6":
    raise ValueError("Fixed fixture SHA mismatch")
pcm = np.zeros((178, 960, 2), dtype="<i2")
for cycle in range(178):
    _, _, audio, low, _ = inspect_burst(raw[cycle*PERIOD:(cycle+1)*PERIOD])
    if cycle < 3:
        if audio is not None:
            raise ValueError("Unexpected bootstrap layout")
        continue
    if audio is None or np.any(low):
        raise ValueError("Unexpected fixed-fixture layout or LFE")
    sums = audio.astype(np.int32).sum(axis=1).reshape(960)
    mixed = np.trunc(sums / 4).astype(np.int32)
    if np.any(mixed < -32768) or np.any(mixed > 32767):
        raise ValueError("Mix clips")
    pcm[cycle, :, 0] = pcm[cycle, :, 1] = mixed
data = pcm.tobytes()
with output.open("xb") as stream:
    stream.write(data)
print(json.dumps({"scope": "fixed-fixture mono audition; no spatial rendering", "frames": 178*960,
                  "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}))
