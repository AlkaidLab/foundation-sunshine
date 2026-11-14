#!/usr/bin/env python3
"""End-to-end check of crash capture against a local mock of POST /v1/crash.

  crash_e2e.py --exe <program> --config-dir <dir> [--arg X ...] [--expect-dump] [--modes null-deref,abort,throw,terminate,hang]

For each mode the program is started with SUNSHINE_TEST_CRASH=<mode> (it must fail or, for hang, report and exit), the pending folder
<config-dir>/crashes/pending is checked for a meta (and a minidump on Windows), then the program is started again with the uploader
pointed at an in-process mock endpoint and the pending folder must empty out, with the mock having received meta (+ dump) for it.
Never touches an installed service: it only runs the program it is given, with the config directory it is given.
"""
import argparse, http.server, json, os, shutil, subprocess, sys, threading, time

p = argparse.ArgumentParser()
p.add_argument('--exe', required=True)
p.add_argument('--config-dir', required=True)
p.add_argument('--arg', action='append', default=[])
p.add_argument('--expect-dump', action='store_true')
p.add_argument('--modes', default='null-deref,abort,throw,terminate,hang')
p.add_argument('--pending-dir', default='', help='pending folder when it is not <config-dir>/crashes/pending (the panel)')
p.add_argument('--env', action='append', default=[], help='K=V added to the environment of every run')
p.add_argument('--create-install-id', action='store_true', help='write <config-dir>/telemetry/install_id first (the panel only reads it)')
p.add_argument('--kinds', default='', help='mode=kind,... overrides of the expected meta kind (sparxie-launch: throw=unhandled-dotnet,task=unhandled-dotnet)')
p.add_argument('--no-dump-modes', default='panic', help='modes that write no dump')
p.add_argument('--live-modes', default='hang', help='modes after which the process may exit 0')
p.add_argument('--second-mode', default='', help='SUNSHINE_TEST_CRASH value of the upload run (upload-only for the real host)')
p.add_argument('--server-error-mode', default='abort', help='crash mode used for the "server answers 500" check (sparxie-launch has no abort: throw)')
p.add_argument('--stay', type=int, default=14, help='seconds the second run stays alive for the upload pass')
p.add_argument('--crash-timeout', type=int, default=90)
a = p.parse_args()

received = []
status = [200]

class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(n)
        received.append((self.path, body))
        self.send_response(status[0]); self.send_header('Content-Length', '0'); self.end_headers()
    def log_message(self, *x): pass

srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()
endpoint = f'http://127.0.0.1:{port}'
pending = a.pending_dir or os.path.join(a.config_dir, 'crashes', 'pending')
if a.create_install_id:
    os.makedirs(os.path.join(a.config_dir, 'telemetry'), exist_ok=True)
    open(os.path.join(a.config_dir, 'telemetry', 'install_id'), 'w').write('11111111-2222-4333-8444-555555555555\n')
failures = []

def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond: failures.append(what)

def run(mode, upload_delay, endpoint_url, timeout):
    env = dict(os.environ)
    for kv in a.env:
        k, v = kv.split('=', 1); env[k] = v
    env['SUNSHINE_CRASH_UPLOAD_DELAY_S'] = str(upload_delay)
    env['SUNSHINE_TELEMETRY_ENDPOINT'] = endpoint_url
    if mode: env['SUNSHINE_TEST_CRASH'] = mode
    else: env.pop('SUNSHINE_TEST_CRASH', None)
    t0 = time.time()
    try:
        r = subprocess.run([a.exe] + a.arg, env=env, timeout=timeout, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode, time.time() - t0
    except subprocess.TimeoutExpired:
        return None, time.time() - t0

def reports():
    if not os.path.isdir(pending): return []
    return sorted(f for f in os.listdir(pending) if f.endswith('.json'))

for mode in a.modes.split(','):
    print(f'== {mode}')
    shutil.rmtree(pending, ignore_errors=True)
    del received[:]
    # the crashing run must not upload itself: the delay is far away
    rc, took = run(mode, 100000, endpoint, a.crash_timeout)
    check(rc is not None, f'{mode}: the process ended ({rc}) after {took:.1f}s')
    if mode not in a.live_modes.split(','): check(rc not in (0, None), f'{mode}: non-zero exit {rc}')
    names = reports()
    check(len(names) == 1, f'{mode}: one meta in pending ({names})')
    if not names: continue
    meta = json.load(open(os.path.join(pending, names[0]), encoding='utf-8'))
    want_kind = {'null-deref': 'crash', 'abort': 'abort', 'throw': 'abort', 'terminate': 'abort', 'hang': 'hang', 'panic': 'panic', 'task': 'unhandled-dotnet'}[mode]
    want_kind = dict(kv.split('=') for kv in a.kinds.split(',') if kv).get(mode, want_kind)
    check(meta.get('kind') == want_kind, f'{mode}: kind {meta.get("kind")} == {want_kind}')
    check(meta.get('v') == 1 and meta.get('product') and meta.get('version') is not None, f'{mode}: v/product/version present')
    check(len(meta.get('signature', '')) == 64, f'{mode}: signature {meta.get("signature", "")[:12]}...')
    check(bool(meta.get('frames')) and 'module' in meta and 'module_offset' in meta, f'{mode}: {len(meta.get("frames", []))} frames, module {meta.get("module")} {meta.get("module_offset")}')
    if mode == 'null-deref' and a.expect_dump: check(meta.get('exception_code') == '0xC0000005', f'null-deref: exception_code {meta.get("exception_code")}')
    dump = os.path.join(pending, names[0][:-5] + '.dmp')
    if a.expect_dump and mode not in a.no_dump_modes.split(','):
        ok = os.path.isfile(dump) and open(dump, 'rb').read(4) == b'MDMP'
        check(ok, f'{mode}: minidump present and starts with MDMP ({os.path.getsize(dump) if os.path.isfile(dump) else 0} bytes)')
        if ok: check(os.path.getsize(dump) <= 20 * 1024 * 1024, f'{mode}: dump within 20 MB')
    # a normal start uploads what is there
    rc, took = run(a.second_mode or None, 1, endpoint, a.stay + 40)
    time.sleep(1)
    check(not reports(), f'{mode}: pending empty after the upload pass')
    posts = [b for (path, b) in received if path == '/v1/crash']
    check(len(posts) == 1, f'{mode}: the mock got {len(posts)} POST /v1/crash')
    if posts:
        b = posts[0]
        check(b'name="meta"' in b and b'"signature"' in b, f'{mode}: multipart meta part with signature')
        check(b'"install_id"' in b, f'{mode}: install_id added at upload')
        if a.expect_dump and mode not in a.no_dump_modes.split(','): check(b'name="dump"' in b and b'MDMP' in b, f'{mode}: multipart dump part')

# consent off: nothing is kept and nothing is sent
print('== consent off')
shutil.rmtree(pending, ignore_errors=True); del received[:]
conf = os.path.join(a.config_dir, 'sunshine.conf')
old = open(conf, encoding='utf-8').read() if os.path.exists(conf) else None
os.makedirs(a.config_dir, exist_ok=True)
open(conf, 'w', encoding='utf-8').write('telemetry_errors = off\n')
rc, took = run('abort', 100000, endpoint, a.crash_timeout)
check(not reports(), 'consent off: a crash leaves nothing on disk')
check(not [1 for (pth, _) in received], 'consent off: nothing sent')
if old is None: os.remove(conf)
else: open(conf, 'w', encoding='utf-8').write(old)

# the mock answering 500 leaves the report
print('== server error keeps the report')
shutil.rmtree(pending, ignore_errors=True); del received[:]
run(a.server_error_mode, 100000, endpoint, a.crash_timeout)
status[0] = 500
run(a.second_mode or None, 1, endpoint, a.stay + 40)
check(len(reports()) == 1, 'HTTP 500: the report stays for the next pass')
status[0] = 200
run(a.second_mode or None, 1, endpoint, a.stay + 40)
time.sleep(1)
check(not reports(), 'next pass after the server recovers: delivered')

print('RESULT', 'FAIL' if failures else 'PASS', len(failures), 'failures')
sys.exit(1 if failures else 0)
