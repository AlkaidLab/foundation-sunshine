"""Actual AudioStream receive/decoder threads; generated carrier bytes, no hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def wire(kind, carrier, fragment=0):
    packet = bytearray(1104)
    packet[:7] = b'SMAT' + bytes([1, kind, 80])
    frame = int(kind == 3)
    struct.pack_into('<IIQQQQI', packet, 8, 1, 1, 1, 1, frame, frame * 960, 48000)
    if kind == 1:
        struct.pack_into('<II', packet, 52, 960, 61440)
        packet[60], packet[62] = fragment, 60
        struct.pack_into('<I', packet, 64, 1024)
        packet[80:] = carrier[fragment * 1024:(fragment + 1) * 1024]
    if kind != 2:
        struct.pack_into('<I', packet, 68, fnv(carrier))
    struct.pack_into('<I', packet, 72, fnv(packet))
    return packet


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    carrier = bytes.fromhex('72f81f4e16000200') + bytes(61432)
    sha = hashlib.sha256(carrier).hexdigest()
    env = dict(os.environ)
    env['PATH'] = 'C:/msys64/ucrt64/bin;' + env.get('PATH', '')
    results = []
    for mode in ('complete', 'missing-end', 'missing-tail'):
        output = args.output / (mode + '.raw')
        with (args.output / (mode + '.stderr')).open('w', encoding='utf8') as error:
            child = subprocess.Popen([str(args.client.resolve()), str(output.resolve()), sha, '1', 'plain'],
                                     stdout=subprocess.PIPE, stderr=error, text=True, env=env,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            ready = child.stdout.readline().strip()
            assert ready.startswith('READY '), (mode, child.pid, ready)
            started = time.monotonic()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                seq = 0
                packets = [wire(2, carrier)] * 4
                packets += [wire(1, carrier, i) for i in range(59 if mode == 'missing-tail' else 60)]
                if mode == 'complete':
                    packets += [wire(3, carrier)] * 4
                for payload in packets:
                    sender.sendto(struct.pack('!BBHII', 0x80, 97, seq, seq, 1) + payload,
                                  ('127.0.0.1', int(ready.split()[1])))
                    seq += 1
                    time.sleep(0.001)
            stdout, _ = child.communicate(timeout=6)
        elapsed = time.monotonic() - started
        (args.output / (mode + '.stdout')).write_text(ready + '\n' + stdout, encoding='utf8')
        status = json.loads(Path(str(output) + '.json').read_text())
        if mode == 'complete':
            assert child.returncode == 0 and status['published'] and status['completeEOS']
            assert hashlib.sha256(output.read_bytes()).hexdigest() == sha
        else:
            assert child.returncode == 1 and elapsed < 5 and not output.exists()
            assert status['failed'] and not status['published'] and not status['completeEOS']
            assert 'MAT validated input timed out' in (args.output / (mode + '.stderr')).read_text()
        results.append(dict(mode=mode, exitCode=child.returncode, seconds=elapsed, passed=True))
    (args.output / 'proof.json').write_text(json.dumps(dict(result='PASS', cases=results,
                                                        nativeCapture=False, hardware=False), indent=2))
    print('PASS actual AudioStream UDP threads: clean EOS, missing END and missing final data terminate within deadline')


if __name__ == '__main__':
    main()
