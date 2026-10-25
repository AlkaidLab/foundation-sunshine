"""Manual paced UDP isolation: host loopback, guest loopback, emulator NAT.

Generated encrypted carrier only. No audiodg attachment, RTSP identity bypass,
silence insertion, skipped MAT cycles, device output or APK installation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import uuid


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--adb', type=Path, required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    build, out = args.build.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, PATH='C:/msys64/ucrt64/bin;' + os.environ['PATH'])

    def device(*cmd):
        return subprocess.check_output([str(args.adb), '-s', args.serial, *cmd], text=True, timeout=30)

    if not args.serial.startswith('emulator-') or device('shell', 'getprop', 'ro.kernel.qemu').strip() != '1':
        raise RuntimeError('Explicit emulator required')

    def udp():
        lines = [line.split()[1:] for line in device('shell', 'cat', '/proc/net/snmp').splitlines() if line.startswith('Udp:')]
        return dict(zip(lines[0], map(int, lines[1])))

    digest = hashlib.sha256()
    for cycle in range(250):
        digest.update(bytes.fromhex('72f81f4e16000200') + bytes((j*7+cycle)&255 for j in range(8,61440)))
    expected_sha = digest.hexdigest()
    remote = '/data/local/tmp/sunshine-mat-network-' + uuid.uuid4().hex
    results = []
    proof = dict(cases=results, expectedSha256=expected_sha, nativeCapture=False,
                 hardwareSink=False, fullRtsp=False, jni=False, passed=False)
    root = Path(__file__).resolve().parents[3]
    proof['hostHead'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    proof['commonHead'] = subprocess.check_output(['git', '-C', 'third-party/moonlight-common-c', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    try:
        device('shell', 'mkdir', remote)
        for target, source in (('probe', 'mat-udp-probe'), ('sender', 'mat-paced-sender')):
            device('push', str(build / 'android' / source), remote + '/' + target)
            device('shell', 'chmod', '700', remote + '/' + target)
        for path in ('windows', 'guest-local', 'guest-nat'):
            for mode in ('raw', 'decode'):
                label = path + '-' + mode
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.bind(('127.0.0.1', 0)); port = str(sock.getsockname()[1])
                before = udp()
                sender_cmd = [str(build / 'paced-sender.exe'), port] if path != 'guest-local' else [str(args.adb), '-s', args.serial, 'shell', remote + '/sender', port]
                receiver_cmd = [str(build / 'windows-probe.exe'), mode, '127.0.0.1', port] if path == 'windows' else [str(args.adb), '-s', args.serial, 'shell', remote + '/probe', mode, '10.0.2.2' if path == 'guest-nat' else '127.0.0.1', port]
                with (out / (label + '-send.log')).open('w') as sl, (out / (label + '-recv.log')).open('w') as rl:
                    sender = receiver = None
                    try:
                        sender = subprocess.Popen(sender_cmd, stdout=subprocess.PIPE, stderr=sl, text=True, env=env)
                        assert sender.stdout.readline().strip() == 'READY'
                        receiver = subprocess.Popen(receiver_cmd, stdout=rl, stderr=subprocess.STDOUT, text=True, env=env)
                        text, _ = sender.communicate(timeout=30); sl.write(text)
                        receiver.wait(timeout=12)
                    finally:
                        # Both actors have their own deadlines; never abandon a child.
                        for actor in (sender, receiver):
                            if actor and actor.poll() is None:
                                actor.wait(timeout=30)
                sent = dict(re.findall(r'(\w+)=(-?\w+)', text))
                log = (out / (label + '-recv.log')).read_text()
                received = dict(re.findall(r'(\w+)=(-?\w+)', log.split('RESULT ')[-1]))
                after = udp()
                complete = sender.returncode == receiver.returncode == 0 and sent['datagrams'] == received['received'] and sent['checksum'] == received['checksum'] and received['duplicate'] == received['malformed'] == '0'
                decoded = None if mode == 'raw' else (received['cycles'] == '250' and received['ended'] == '1' and received['failed'] == '0' and received['sha256'] == expected_sha)
                item = dict(path=path, mode=mode, complete=complete, decoded=decoded, sent=sent, received=received,
                            senderExitCode=sender.returncode, receiverExitCode=receiver.returncode,
                            receiveCountScope='through terminal shutdown' if received['failed'] == '1' else 'full bounded send',
                            kernelUdpDelta={key:after[key]-value for key,value in before.items()})
                first_error = int(received['firstFailureRcvbufErrors'])
                item['kernelRcvbufErrorsBeforeFirstFailure'] = None if first_error < 0 else first_error-before['RcvbufErrors']
                results.append(item)
                (out / 'proof.json').write_text(json.dumps(proof, indent=2))
                print(label, 'complete=' + str(complete), 'decoded=' + str(decoded), flush=True)
        proof['passed'] = all(item['complete'] and item['decoded'] is not False for item in results)
        # A measured NAT failure remains a failed run, not a green acceptance.
        if not proof['passed']:
            raise SystemExit(1)
    finally:
        for name in ('probe', 'sender'):
            device('shell', 'rm', remote + '/' + name)
        device('shell', 'rmdir', remote)
        (out / 'proof.json').write_text(json.dumps(proof, indent=2))


if __name__ == '__main__':
    main()
