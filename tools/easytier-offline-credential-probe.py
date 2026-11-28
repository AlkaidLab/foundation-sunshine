import argparse, contextlib, json, os, secrets, socket, subprocess, tempfile, time
from pathlib import Path

parser = argparse.ArgumentParser(description='Test public-only EasyTier credential storage using disposable local instances (Windows).')
parser.add_argument('--core', required=True, type=Path, help='Path to the verified easytier-core.exe')
parser.add_argument('--cli', required=True, type=Path, help='Path to the matching verified easytier-cli.exe')
args = parser.parse_args()
if os.name != 'nt': parser.error('This diagnostic currently requires Windows')
for executable in (args.core, args.cli):
    if not executable.is_file(): parser.error(f'Executable not found: {executable}')
core, cli = str(args.core.resolve()), str(args.cli.resolve())
env = {k: v for k, v in os.environ.items() if not k.upper().startswith('ET_')}
adminenv = dict(env, ET_NETWORK_SECRET=secrets.token_hex(32))
net = 'offline-test-' + secrets.token_hex(8)
procs = []
def port():
    s = socket.socket(); s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close(); return p
def start(args, child_env, cwd):
    p = subprocess.Popen([core, *args], env=child_env, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
    procs.append(p); return p
def stop(p):
    if p.poll() is None:
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
def call(rpc, *args, timeout=3):
    return subprocess.run([cli, '--rpc-portal', f'127.0.0.1:{rpc}', '--output', 'json', *args], env=env, capture_output=True, text=True, timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW)
def wait_rpc(rpc):
    for _ in range(60):
        try:
            if call(rpc, 'peer', timeout=1).returncode == 0: return
        except subprocess.TimeoutExpired: pass
        time.sleep(.25)
    raise RuntimeError('RPC did not become available')
def peer_visible(rpc, address):
    try:
        result = call(rpc, 'peer', timeout=2)
        return result.returncode == 0 and any(x.get('ipv4', '') == address for x in json.loads(result.stdout))
    except (subprocess.TimeoutExpired, json.JSONDecodeError, TypeError):
        return False
def wait_peer(rpc, addr, wanted, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if peer_visible(rpc, addr) == wanted: return True
        time.sleep(.5)
    return False

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(('8.8.8.8', 53)); ip = s.getsockname()[0]; s.close()
adminrpc, clientrpc, listen = port(), port(), port()
try:
    with contextlib.ExitStack() as stack:
        temp = stack.enter_context(tempfile.TemporaryDirectory(prefix='easytier-offline-probe-'))
        stack.callback(lambda: [stop(p) for p in reversed(procs)])
        credpath = Path(temp) / 'credentials.json'
        def host_args(deny=False):
            args = ['--network-name', net, '--secure-mode', 'true', '--private-mode', 'true', '--no-tun', 'true', '--ipv4', '10.234.91.1', '--listeners', f'tcp://{ip}:{listen}', '--rpc-portal', f'127.0.0.1:{adminrpc}', '--disable-upnp', 'true', '--disable-ipv6', 'true', '--credential-file', str(credpath)]
            if deny: args += ['--rpc-portal-whitelist', '192.0.2.0/24']
            return args
        admin = start(host_args(), adminenv, temp); wait_rpc(adminrpc)
        result = call(adminrpc, 'credential', 'generate', '--ttl', '300', '--reusable', 'false')
        if result.returncode != 0: raise RuntimeError('credential generation failed')
        credential = json.loads(result.stdout); clientenv = dict(env, ET_CREDENTIAL=credential['credential_secret'])
        stop(admin)
        entries = json.loads(credpath.read_text(encoding='utf8'))
        if credential['credential_id'] not in entries: raise RuntimeError('generated credential was not persisted')
        for entry in entries.values(): entry.pop('secret', None)
        credpath.write_text(json.dumps(entries), encoding='utf8')
        admin = start(host_args(deny=True), adminenv, temp)
        time.sleep(1)
        if admin.poll() is not None: raise RuntimeError('host failed to start with public-only credential file')
        client = start(['--network-name', net, '--secure-mode', 'true', '--no-tun', 'true', '--ipv4', '10.234.91.2', '--no-listener', '--peers', f'tcp://{ip}:{listen}', '--rpc-portal', f'127.0.0.1:{clientrpc}', '--disable-upnp', 'true', '--disable-ipv6', 'true'], clientenv, temp)
        wait_rpc(clientrpc)
        if not wait_peer(clientrpc, '10.234.91.1', True, 25): raise RuntimeError('client could not join with public-only host file')
        print('Public-only credential file: client joined')
        try:
            call(adminrpc, 'peer', timeout=2)
            raise RuntimeError('host RPC did not time out as expected')
        except subprocess.TimeoutExpired: pass
        print('Host RPC: local CLI timed out')
        stop(admin)
        credpath.write_text('{}', encoding='utf8')
        admin = start(host_args(deny=True), adminenv, temp)
        time.sleep(2)
        if admin.poll() is not None: raise RuntimeError('host failed to restart after credential removal')
        if not wait_peer(clientrpc, '10.234.91.1', False, 30): raise RuntimeError('client remained connected after offline removal and host restart')
        time.sleep(5)
        if admin.poll() is not None: raise RuntimeError('host exited during revocation observation')
        if peer_visible(clientrpc, '10.234.91.1'): raise RuntimeError('client rejoined after removal')
        print('Credential removed offline: client disconnected and did not rejoin during observation')
finally:
    for p in reversed(procs): stop(p)
