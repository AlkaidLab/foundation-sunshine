"""Run the production v2 enhancement analyzer against controlled PCM; no physical output."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

def main(fixtures, analyzer, out):
    out.mkdir(parents=True, exist_ok=True)
    specs = json.loads((fixtures / 'cases.json').read_text())['cases']
    results = []
    for spec in specs:
        name = spec['name']
        target = out / (name + '.irextbin')
        subprocess.run([str(analyzer.resolve()), str(fixtures / (name + '.pcm')), str(target)], check=True)
        data = target.read_bytes()
        events, seen, frames, body, low, high = [], set(), 0, 0, 0.0, 0.0
        at = 0
        while at < len(data):
            size = 72 + struct.unpack_from('<H', data, at + 74)[0]
            time = struct.unpack_from('<Q', data, at + 80)[0] / 1e6
            event_count = data[at + 73]
            assert size == 104 + event_count * 20
            l = struct.unpack_from('<2f', data, at + 88)
            r = struct.unpack_from('<2f', data, at + 96)
            if spec['steady_start_s'] <= time <= spec['steady_end_s']:
                frames += 1
                low += max(l[0], r[0]); high += max(l[1], r[1])
                body += max(l[0], r[0], l[1], r[1]) > 0.005
            for i in range(event_count):
                off = at + 104 + i * 20
                eid = struct.unpack_from('<I', data, off)[0]
                if eid not in seen:
                    seen.add(eid)
                    events.append(struct.unpack_from('<Q', data, off + 4)[0] / 1e6)
            at += size
        unmatched = list(events)
        matched = 0
        for event in spec['events_s']:
            candidate = next((t for t in unmatched if event - .005 <= t <= event + .020), None)
            if candidate is not None:
                matched += 1; unmatched.remove(candidate)
        row = dict(name=name, expected=len(spec['events_s']), matched=matched, events=len(events),
                   steady_events=sum(spec['steady_start_s'] <= t <= spec['steady_end_s'] for t in events),
                   body_coverage=body/frames if frames else 0,
                   body_low=low/frames if frames else 0, body_high=high/frames if frames else 0,
                   event_times_s=events)
        results.append(row)
        print(f"{name:36s} {matched:3}/{row['expected']:3} events={len(events):3} steady={row['steady_events']:3} body={row['body_coverage']:.2f} level={row['body_low']:.4f}/{row['body_high']:.4f}")
    (out / 'audit.json').write_text(json.dumps(dict(cases=results), indent=2))

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--fixtures', type=Path, required=True)
    p.add_argument('--analyzer', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    main(a.fixtures, a.analyzer, a.out)
