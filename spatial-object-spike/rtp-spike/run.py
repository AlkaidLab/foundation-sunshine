"""Recorded objects -> experimental JOC -> REAL Sunshine UDP RTP -> Moonlight queue -> disk.
No live capture, pairing, device changes, Android playback, or Atmos certification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
JOC = ROOT.parent / 'joc-spike'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def split_frames(data):
    result = []
    offset = 0
    while offset < len(data):
        h = data[offset:offset+6]
        if len(h) != 6 or h[:2] != b'\x0b\x77' or h[4] & 0xf0 != 0x30 or h[5] >> 3 != 16 or h[2] >> 6:
            raise ValueError(f'invalid 48k/1536 E-AC3 header at {offset}')
        size = ((((h[2] & 7) << 8) | h[3]) + 1) * 2
        frame = data[offset:offset+size]
        if size < 6 or len(frame) != size:
            raise ValueError('truncated E-AC3')
        result.append(frame)
        offset += size
    if not result:
        raise ValueError('empty input')
    return result


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x8005 if crc & 0x8000 else 0)) & 0xffff
    return crc


def fixed_frame(frame, size):
    """Add E-AC3 in-frame padding, NOT RTP trailers or object payload changes.
    Only the already-verified unsigned skip-field candidate (auxdatae=0).
    Keep all pre-tail audio/metadata bits; relocate tail flags, frmsiz and CRC.
    Independent FFmpeg/Cavern comparisons are required after this transform.
    """
    if size == 0:
        return frame
    if size < len(frame) or size > 3904 or size % 2:
        raise ValueError(f'frame {len(frame)} exceeds fixed test budget {size}')
    if frame[-3] & 2:
        raise ValueError('auxdatae=1 is not supported by this research padder')
    if crc16(frame[2:]) != 0:
        raise ValueError('source E-AC3 CRC failed')
    if size == len(frame):
        return frame
    body = bytearray(frame[:-2])
    flags = body[-1] & 3
    body[-1] &= 0xfc
    body.extend(bytes(size - len(frame)))
    body[-1] |= flags
    words = size // 2 - 1
    body[2] = (body[2] & 0xf8) | (words >> 8)
    body[3] = words & 0xff
    output = bytes(body) + struct.pack('>H', crc16(body[2:]))
    assert len(output) == size and crc16(output[2:]) == 0
    return output


def command(args, log, timeout=90):
    with log.open('xb') as stream:
        result = subprocess.run([str(a) for a in args], stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {log}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT/'build07')
    parser.add_argument('--fixture', choices=['game2', 'tone20'], default='game2')
    parser.add_argument('--frames', type=int)
    parser.add_argument('--encoded', type=Path, help='reuse an existing stream_bench output')
    parser.add_argument('--frame-bytes', type=int, default=0, help='0: baseline variable frames; 3360: tested fixed FEC budget')
    parser.add_argument('--modes', nargs='+', choices=['plain','aes'], default=['plain','aes'])
    parser.add_argument('--faults', nargs='+', choices=['none','reorder','drop1','drop2'], default=['none'])
    parser.add_argument('--decode', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    build = args.build.resolve()
    encoded = args.encoded.resolve() if args.encoded else out/'encoded'
    if not args.encoded:
        source = JOC/('rust02/game-2' if args.fixture == 'game2' else 'rust01/synthetic-20')
        count = args.frames or (93 if args.fixture == 'game2' else 312)
        command([sys.executable,'-B','-u',JOC/'stream_bench.py',source,'--output',encoded,
                 '--frames',count,'--paced','--packetized-input'],out/'encode.log',count*.032+50)
    raw = encoded/'objects.eac3'
    frames = split_frames(raw.read_bytes())
    if len(frames) > 1996:
        raise ValueError('bounded replay allows 1996 content frames plus four preroll frames')
    sent = out/'sent.eac3'
    sent.write_bytes(b''.join(fixed_frame(frame,args.frame_bytes) for frame in frames))
    # Native Moonlight queue discards its first four-packet FEC group while
    # synchronizing. Preserve native behavior and supply a separate preroll;
    # the entire requested recorded clip must then arrive byte-for-byte.
    wire_source = out/'wire-source.eac3'
    wire_source.write_bytes(fixed_frame(frames[0],args.frame_bytes)*4 + sent.read_bytes())
    lengths = [len(frame) for frame in frames]
    results = []
    for mode in args.modes:
        for fault in args.faults:
            case = out/f'{mode}-{fault}'; case.mkdir()
            with (case/'receiver.log').open('xb') as rlog, (case/'sender.log').open('xb') as slog:
                receiver = subprocess.Popen([str(build/'moonlight-audio-dump.exe'),str(case/'received.eac3'),
                    str(case/'frames.csv'),str(case/'stats.json'),str(len(frames)),mode,fault],
                    stdout=subprocess.PIPE,stderr=rlog)
                try:
                    # OS process watchdog; no sleep or indefinite child wait.
                    import threading
                    watchdog = threading.Timer(len(frames)*.032+30,receiver.kill)
                    watchdog.daemon=True; watchdog.start()
                    ready = receiver.stdout.readline().decode('ascii').strip()
                    if not ready.startswith('READY '):
                        raise RuntimeError(f'receiver startup failed: {ready}; {case}')
                    sender = subprocess.run([str(build/'sunshine-audio-replay.exe'),'--audio-rtp-replay',
                        str(wire_source),ready.split()[1],mode],stdout=slog,stderr=subprocess.STDOUT,
                        timeout=len(frames)*.032+15)
                    rc = receiver.wait(timeout=10)
                finally:
                    watchdog.cancel()
                    if receiver.poll() is None: receiver.kill(); receiver.wait(timeout=5)
                stats = json.loads((case/'stats.json').read_text())
                received = case/'received.eac3'
                result = dict(mode=mode,fault=fault,sender_returncode=sender.returncode,receiver_returncode=rc,
                    source_sha256=sha(sent),received_sha256=sha(received),bytes_identical=sent.read_bytes()==received.read_bytes(),
                    stats=stats)
                result['passed'] = bool(sender.returncode==0 and rc==0 and result['bytes_identical'])
                results.append(result)
                print(json.dumps(result),flush=True)
    decode = None
    if args.decode and all(r['passed'] for r in results):
        target = out/f'{args.modes[0]}-{args.faults[0]}'/'received.eac3'
        command([sys.executable,'-B','-u',JOC/'verify_core.py',raw,sent,target,'--output',out/'core'],out/'core.log')
        # Cavern has lookahead; avoid forcing its tail beyond the bounded clip.
        requested = len(frames)-3
        for label,source in [('original',raw),('received',target)]:
            command(['C:/Program Files/dotnet/dotnet.exe',JOC/'CavernProbe/latest/CavernProbe.dll',
                     source,out/f'decoded-{label}',requested],out/f'decode-{label}.log')
        before = out/'decoded-original'; after = out/'decoded-received'
        files = ['positions.csv'] + sorted(p.name for p in before.glob('object-*.f32'))
        same = {name:sha(before/name)==sha(after/name) for name in files}
        decode = dict(requested_frames=requested,files_identical=same,
            before=json.loads((before/'summary.json').read_text()),after=json.loads((after/'summary.json').read_text()),
            core=json.loads((out/'core/summary.json').read_text()))
        decode['core_pcm_identical']=len({x['raw_pcm_sha256'] for x in decode['core']})==1
        decode['passed']=all(same.values()) and decode['core_pcm_identical']
        command([sys.executable,'-B','-u',JOC/'measure_objects.py',encoded,after,'--output',out/'object-measurements.json',
                 '--kind','game' if args.fixture=='game2' else 'synthetic'],out/'object-measurements.log')
    summary = dict(scope='Windows localhost; mechanically extracted verbatim Sunshine audioBroadcastThread/platf::send with state-only frontend and unmodified Moonlight RTP queue/crypto; no full RTSP/Android playback',
        fixture=args.fixture,frames=len(frames),seconds=len(frames)*.032,preroll_frames=4,original_frame_min=min(lengths),original_frame_max=max(lengths),
        original_unique_lengths=len(set(lengths)),fixed_frame_bytes=args.frame_bytes,original_sha256=sha(raw),sent_sha256=sha(sent),
        network=results,decode=decode,
        passed=all(r['passed'] for r in results) and (decode is None or decode['passed']))
    (out/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf8')
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
