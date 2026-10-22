"""Independent numeric validation of recorded buffers and API metadata."""
import array
import csv
import hashlib
import json
import math
import pathlib
import struct
import sys

root = pathlib.Path(__file__).resolve().parent
rows = list(csv.DictReader((root / 'objects.csv').open(encoding='utf-8')))
assert rows, 'No captured metadata'
assert [int(r['sequence']) for r in rows] == list(range(len(rows))), 'Sequence gap'
assert len({r['pid'] for r in rows}) == 1, 'Unexpected source processes'
results = []
for object_id in sorted({int(r['object_id']) for r in rows}):
    group = [r for r in rows if int(r['object_id']) == object_id]
    file = root / f'object-{object_id}.wav'
    raw = file.read_bytes()
    assert raw[:4] == b'RIFF' and raw[8:16] == b'WAVEfmt '
    fmt, channels, rate = struct.unpack_from('<HHI', raw, 20)
    data_size = struct.unpack_from('<I', raw, 40)[0]
    assert fmt == 3 and channels == 1 and rate == 48000
    assert data_size == len(raw) - 44
    samples = array.array('f')
    samples.frombytes(raw[44:])
    if sys.byteorder != 'little':
        samples.byteswap()
    offset = 0
    max_position_error = 0.0
    for row in group:
        frames = int(row['frames'])
        assert int(row['frame_offset']) == offset, 'Object frame gap'
        assert int(row['wav_offset']) == offset * 4
        assert int(row['sample_rate']) == rate
        assert int(row['object_type']) == 1, 'Not a dynamic spatial object'
        t = offset / rate
        phase = object_id - 1
        expected = (math.sin(t * 1.1 + phase), 0.5 * math.sin(t * 0.7 + phase), -math.cos(t * 1.1 + phase))
        actual = tuple(float(row[key]) for key in ('x', 'y', 'z'))
        max_position_error = max(max_position_error, *(abs(a - b) for a, b in zip(actual, expected)))
        expected_volume = 0.1 if object_id == 1 else 0.07
        assert abs(float(row['volume']) - expected_volume) < 1e-6
        offset += frames
    assert offset == len(samples) and offset >= 5 * rate
    frequency = 523.25 if object_id == 1 else 783.99
    max_sample_error = max(abs(float(v) - 0.2 * math.sin(2 * math.pi * frequency * i / rate)) for i, v in enumerate(samples))
    assert max_sample_error < 1e-6, 'Audio sample mismatch'
    assert max_position_error < 1e-6, 'Position mismatch'
    rms = math.sqrt(sum(v * v for v in samples) / len(samples))
    assert rms > 0.1, 'Silent audio'
    transitions = [i for i in range(1, len(samples)) if samples[i - 1] <= 0 < samples[i]]
    measured_hz = (len(transitions) - 1) * rate / (transitions[-1] - transitions[0])
    results.append({
        'object_id': object_id, 'blocks': len(group), 'frames': offset,
        'sample_rate': rate, 'duration_seconds': offset / rate,
        'max_sample_absolute_error': max_sample_error,
        'max_xyz_absolute_error': max_position_error,
        'rms': rms, 'measured_frequency_hz': measured_hz,
        'xyz_ranges': {key: [min(float(r[key]) for r in group), max(float(r[key]) for r in group)] for key in ('x', 'y', 'z')},
        'sha256': hashlib.sha256(raw).hexdigest(),
    })

assert len(results) == 2, 'Expected two independently captured objects'
assert results[0]['sha256'] != results[1]['sha256'], 'Objects were mixed or duplicated'
assert abs(results[0]['measured_frequency_hz'] - results[1]['measured_frequency_hz']) > 200
print(json.dumps({'status': 'PASS', 'source_pid': int(rows[0]['pid']), 'objects': results}, indent=2))
