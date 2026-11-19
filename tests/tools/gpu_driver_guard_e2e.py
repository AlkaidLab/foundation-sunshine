#!/usr/bin/env python3
"""End-to-end check of the graphics driver mark and the crash-loop guard with a real crash in a real process.

  gpu_driver_guard_e2e.py --exe <crash_mark_probe | sunshine> [--arg X ...] --config-dir <dir> [--host] [--expect-dump]

The program is started with SUNSHINE_TEST_CRASH=<mode> and SUNSHINE_TEST_DRIVER_MODULE=<its own file name>=<vendor>, which stands the program's own code in for a driver
module, so the mark that the crash handler writes can be checked without a graphics driver:

  * a crash or an abort whose top frame lies in a "driver" leaves <config>/crashes/gpu_driver_mark.json (vendor, module, driver version, phase, kind),
  * the same crash without the stand-in leaves none,
  * a person who turned error reports off still gets the mark, and no report is kept,
  * three crashes in a row put the next start into the safe mode (<config>/startup_guard.json, `safe_mode`), a clean run ends the count.

`--host` runs the real host: the guard decides before SUNSHINE_TEST_CRASH is read there too, and the fourth run is `upload-only` (it ends by itself).
Never touches an installed service: it only runs the program it is given, with the config directory it is given.
"""
import argparse, json, os, shutil, subprocess, sys, time

p = argparse.ArgumentParser()
p.add_argument('--exe', required=True)
p.add_argument('--config-dir', required=True)
p.add_argument('--arg', action='append', default=[])
p.add_argument('--host', action='store_true', help='the program is the host (the config directory is passed as it is, the fourth run is upload-only)')
p.add_argument('--modes', default='null-deref,abort')
p.add_argument('--vendor', default='intel')
p.add_argument('--timeout', type=int, default=90)
a = p.parse_args()

failures = []
def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond: failures.append(what)

exe_name = os.path.basename(a.exe)
mark_path = os.path.join(a.config_dir, 'crashes', 'gpu_driver_mark.json')
guard_path = os.path.join(a.config_dir, 'startup_guard.json')
pending = os.path.join(a.config_dir, 'crashes', 'pending')

def run(mode, stand_in=True, phase=None):
    env = dict(os.environ)
    env['SUNSHINE_CRASH_UPLOAD_DELAY_S'] = '100000'
    if mode: env['SUNSHINE_TEST_CRASH'] = mode
    else: env.pop('SUNSHINE_TEST_CRASH', None)
    if stand_in: env['SUNSHINE_TEST_DRIVER_MODULE'] = f'{exe_name}={a.vendor}'
    else: env.pop('SUNSHINE_TEST_DRIVER_MODULE', None)
    cmd = [a.exe] + a.arg if a.host else [a.exe, a.config_dir] + ([phase] if phase else [])
    t0 = time.time()
    try:
        r = subprocess.run(cmd, env=env, timeout=a.timeout, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        return r.returncode, r.stdout, time.time() - t0
    except subprocess.TimeoutExpired as e:
        return None, '', time.time() - t0

SENTINEL = '.gpu_driver_guard_e2e'

def fresh():
    # only a directory this script made (or an empty one) is cleared: a wrong --config-dir must not take a real configuration with it
    if os.path.isdir(a.config_dir) and os.listdir(a.config_dir) and not os.path.exists(os.path.join(a.config_dir, SENTINEL)):
        sys.exit(f'refusing to clear {a.config_dir}: it is not empty and was not made by this script')
    shutil.rmtree(a.config_dir, ignore_errors=True)
    os.makedirs(a.config_dir, exist_ok=True)
    open(os.path.join(a.config_dir, SENTINEL), 'w').close()

def mark():
    try:
        return json.load(open(mark_path, encoding='utf-8'))
    except Exception:
        return None

def guard():
    try:
        return json.load(open(guard_path, encoding='utf-8'))
    except Exception:
        return None

for mode in a.modes.split(','):
    print(f'== {mode}: the top frame lies in a driver')
    fresh()
    rc, out, took = run(mode, True, 'probe')
    check(rc not in (0, None), f'{mode}: the process failed ({rc}) after {took:.1f}s')
    m = mark()
    check(m is not None, f'{mode}: a mark was written')
    if m:
        check(m.get('vendor') == a.vendor, f'{mode}: vendor {m.get("vendor")}')
        check(m.get('module', '').lower() == exe_name.lower(), f'{mode}: module is the file name only ({m.get("module")})')
        check(m.get('kind') == 'crash', f'{mode}: kind {m.get("kind")} (an abort is a crash here)')
        check(m.get('phase') == 'probe' or a.host, f'{mode}: phase {m.get("phase")}')
        check(a.config_dir not in json.dumps(m) and '/' not in m.get('module', '') and '\\' not in m.get('module', ''), f'{mode}: no path in the mark')
        check(isinstance(m.get('time'), int) and m['time'] > 1700000000, f'{mode}: time {m.get("time")}')
        if not a.host: check(m.get('driver') == '32.0.101.6325', f'{mode}: driver version {m.get("driver")}')

    print(f'== {mode}: no stand-in, no mark')
    fresh()
    rc, out, took = run(mode, False)
    check(mark() is None, f'{mode}: a crash outside a driver leaves no mark')

    print(f'== {mode}: error reports off, the mark stays')
    fresh()
    open(os.path.join(a.config_dir, 'sunshine.conf'), 'w', encoding='utf-8').write('telemetry_errors = off\n')
    rc, out, took = run(mode, True)
    check(mark() is not None, f'{mode}: the mark is written although reports are off')
    kept = [f for f in os.listdir(pending)] if os.path.isdir(pending) else []
    check(not kept, f'{mode}: no report is kept ({kept})')

print('== crash loop')
fresh()
safe_seen = []
for i in range(3):
    rc, out, took = run('null-deref', True)
    safe_seen.append('safe=1' in out)
g = guard()
check(g is not None and not g.get('safe'), 'three failed starts on record, the guard is not in safe mode yet')
rc, out, took = run('upload-only' if a.host else None, False)
g = guard()
check(g is not None and g.get('safe') is True, 'the fourth start is in the safe mode')
if not a.host: check('safe=1' in out, 'the fourth start says so')

print('== a clean run ends the count')
fresh()
for i in range(2):
    run('null-deref', True)
if not a.host:
    run(None, False)  # a clean exit
    rc, out, took = run('null-deref', True)
    rc, out, took = run(None, False)
    g = guard()
    check(g is not None and g.get('consecutive') in (0, 1), f'the count after a clean run: {g and g.get("consecutive")}')
    check(g is not None and not g.get('safe'), 'no safe mode after a clean run in between')

print('RESULT', 'FAIL' if failures else 'PASS', len(failures), 'failures')
sys.exit(1 if failures else 0)
