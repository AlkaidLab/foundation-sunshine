"""Own-process loopback only. Never kills a process, elevates or touches audio.
Generated evidence goes in a NEW output directory; timeouts stop orchestration
and report PIDs rather than terminate/retry a potentially still-running process.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
FROZEN_SHA = '79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6'

def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build', type=pathlib.Path, required=True)
    p.add_argument('--out', type=pathlib.Path, required=True)
    p.add_argument('--suite', choices=['baseline', 'positive', 'negative', 'all'], default='all')
    a = p.parse_args()
    build, out = a.build.resolve(), a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for rel, expected in json.loads((build/'manifest.json').read_text()).items():
        assert sha(ROOT/rel).upper() == expected, f'Source drift: {rel}'
    source = ROOT/'spatial-object-spike/mat-spike/mat-native-session-run02/capture/encoder-payload.raw'
    assert source.stat().st_size == 10936320 and sha(source) == FROZEN_SHA
    cycles = source.stat().st_size // 61440
    unit = subprocess.Popen([str(build/'mat-wire-unit.exe')], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        unit_stdout, unit_stderr = unit.communicate(timeout=10)
    except subprocess.TimeoutExpired as e:
        raise RuntimeError(f'Stopped without killing unit PID {unit.pid}') from e
    (out/'unit.log').write_text(unit_stdout + unit_stderr)
    assert unit.returncode == 0, unit_stdout + unit_stderr
    positive = [(enc, fault) for enc in ('plain', 'aes') for fault in ('none', 'reorder', 'duplicate', 'drop1', 'drop2')]
    negative = [(enc, fault) for enc in ('plain', 'aes') for fault in ('drop3', 'dropframe', 'noend', 'corrupt', 'wrongsha')]
    cases = {'baseline': [('plain','none'), ('aes','none')], 'positive': positive, 'negative': negative, 'all': positive+negative}[a.suite]
    proof = {'input': str(source), 'inputBytes': source.stat().st_size, 'inputSha256': FROZEN_SHA,
             'cycles': cycles, 'unit': unit_stdout.strip(), 'cases': [],
             'senderSha256': sha(build/'sunshine-audio-replay.exe'),
             'receiverSha256': sha(build/'moonlight-audio-dump.exe'),
             'scope': 'Native Moonlight dump on Windows loopback; no Android APK, RTSP, decode or audiodg test'}
    for enc, fault in cases:
        case = out/f'{enc}-{fault}'
        case.mkdir()
        dest, stats = case/'received.raw', case/'stats.json'
        trusted = '0'*64 if fault == 'wrongsha' else FROZEN_SHA
        receiver = subprocess.Popen([str(build/'moonlight-audio-dump.exe'), str(dest), str(stats), str(cycles),
                                     trusted, enc, 'none' if fault == 'wrongsha' else fault],
                                    stdout=subprocess.PIPE, stderr=(case/'receiver.stderr').open('w'),
                                    text=True, creationflags=subprocess.CREATE_NO_WINDOW)
        # Receiver has its own bounded deadline, and exits if setup fails.
        ready = receiver.stdout.readline().strip()
        assert ready.startswith('READY '), (receiver.pid, ready)
        with (case/'sender.log').open('w') as log:
            sender = subprocess.Popen([str(build/'sunshine-audio-replay.exe'), '--mat-rtp-replay',
                                       str(source), ready.split()[1], enc], stdout=log, stderr=log,
                                      creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                sender.communicate(timeout=20)
                receiver.communicate(timeout=15)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f'Stopped without killing: sender PID {sender.pid}, receiver PID {receiver.pid}') from e
        assert sender.returncode == 0, (enc, fault, sender.returncode)
        result = json.loads(stats.read_text())
        good = fault in ('none','reorder','duplicate','drop1','drop2')
        if good:
            assert receiver.returncode == 0 and result['published'] and dest.exists(), result
            assert sha(dest) == FROZEN_SHA and dest.stat().st_size == source.stat().st_size
            assert result['sha256'] == FROZEN_SHA and result['cycles'] == cycles and result['completeEOS']
            assert result['errors'] == result['concealed'] == result['fecInvalid'] == result['invalid'] == 0
            if fault.startswith('drop'):
                assert result['fecRecovered'] >= result['injectedDrops'] > 0
        else:
            assert receiver.returncode == 1 and not result['published'] and not dest.exists(), result
            assert pathlib.Path(str(dest)+'.partial').exists()
        assert result['wirePackets'] == result['wireExpected'] == 16032, result
        assert result['maxDatagramBytes'] == (1144 if enc == 'aes' else 1128) and not result['fecDisabled'], result
        entry = {'encryption': enc, 'fault': fault, 'expectedSuccess': good, 'passed': True, **result}
        proof['cases'].append(entry)
        (out/'proof.json').write_text(json.dumps(proof, indent=2))
        print(f'PASS {enc}-{fault}: cycles={result["cycles"]}, FEC recovered={result["fecRecovered"]}, published={result["published"]}', flush=True)
    proof['passed'] = True
    (out/'proof.json').write_text(json.dumps(proof, indent=2))
    print(f'PASS {len(cases)} cases; {FROZEN_SHA}', flush=True)

if __name__ == '__main__':
    main()
