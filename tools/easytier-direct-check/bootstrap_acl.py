"""Exercise a loopback-only, shared-secret EasyTier bootstrap ACL.

Requires separately supplied EasyTier 2.6.4 Windows executables. Starts only
isolated, temporary no-TUN processes with random test credentials. Tests one
allowed TCP service and denies unrelated TCP and UDP. Also checks that stopping
one process leaves a separate management process running. This does not test
multi-instance JNI isolation, a relay path, internet connectivity, NAT traversal,
mobile devices, Sunshine streaming, or access outside the EasyTier overlay.
No system firewall or routing changes and no download are performed.
"""
import argparse, json, os, secrets, socket, subprocess, sys, tempfile, threading, time
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--core", required=True, type=Path, help="Path to EasyTier 2.6.4 easytier-core.exe")
parser.add_argument("--cli", required=True, type=Path, help="Path to matching easytier-cli.exe")
args=parser.parse_args()
for executable in (args.core,args.cli):
    if not executable.is_file():parser.error(f"Executable does not exist: {executable}")
CORE=str(args.core.resolve());CLI=str(args.cli.resolve())
HIDDEN=getattr(subprocess,"CREATE_NO_WINDOW",0)
BASE={k:v for k,v in os.environ.items() if not k.upper().startswith("ET_")}
processes=[]; servers=[]; reserved=set()

def port(proto="tcp"):
    for _ in range(100):
        with socket.socket(socket.AF_INET,socket.SOCK_STREAM if proto=="tcp" else socket.SOCK_DGRAM) as s:
            s.bind(("127.0.0.1",0)); p=s.getsockname()[1]
        if p not in reserved: reserved.add(p);return p
    raise RuntimeError("Cannot choose test port")

class Echo:
    def __init__(self,protocol):
        self.protocol=protocol; self.count=0; self.active=True
        self.sock=socket.socket(socket.AF_INET,socket.SOCK_STREAM if protocol=="tcp" else socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1",0));self.port=self.sock.getsockname()[1];reserved.add(self.port)
        self.sock.settimeout(.25)
        if protocol=="tcp":self.sock.listen()
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
        servers.append(self)
    def run(self):
        while self.active:
            try:
                if self.protocol=="tcp":
                    c,_=self.sock.accept()
                    with c:
                        c.settimeout(2);payload=c.recv(256)
                        if payload:self.count+=1;c.sendall(b"reply:"+payload)
                else:
                    payload,src=self.sock.recvfrom(256);self.count+=1
                    self.sock.sendto(b"reply:"+payload,src)
            except socket.timeout:continue
            except OSError:
                if not self.active:return
    def close(self):
        self.active=False;self.sock.close();self.thread.join(timeout=2)

def probe(proto,p,timeout=1):
    typ=socket.SOCK_STREAM if proto=="tcp" else socket.SOCK_DGRAM
    with socket.socket(socket.AF_INET,typ) as s:
        s.settimeout(timeout); token=secrets.token_bytes(16)
        try:
            if proto=="tcp":
                s.connect(("127.0.0.1",p));s.sendall(token);data=s.recv(256)
            else:
                s.sendto(token,("127.0.0.1",p));data,_=s.recvfrom(256)
            return data==b"reply:"+token
        except OSError:return False

def start(name,args,env):
    p=subprocess.Popen([CORE,*args],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=HIDDEN)
    processes.append((name,p));return p
def run_cli(rpc,*args,timeout=5):
    try:
        return subprocess.run([CLI,"-p",f"127.0.0.1:{rpc}",*args],env=BASE,capture_output=True,text=True,
                              encoding="utf-8",errors="replace",timeout=timeout,creationflags=HIDDEN)
    except subprocess.TimeoutExpired:return None
def wait(fn,seconds=20):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        for name,proc in processes:
            if proc.poll() is not None:raise RuntimeError(f"{name} exited with code {proc.returncode}")
        if fn():return True
        time.sleep(.35)
    return False
def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
def require(x,message):
    if not x:raise RuntimeError(message)

try:
    with tempfile.TemporaryDirectory(prefix="et-bootstrap-acl-") as tmp:
        allowed=Echo("tcp"); denied_tcp=Echo("tcp"); denied_udp=Echo("udp")
        require(probe("tcp",allowed.port) and probe("tcp",denied_tcp.port) and probe("udp",denied_udp.port),
                "Test services are not locally available")
        counts_before=(allowed.count,denied_tcp.count,denied_udp.count)
        host_listener,host_rpc,client_rpc,guard_rpc=port(),port(),port(),port()
        allow_fwd,denied_tcp_fwd,denied_udp_fwd=port(),port(),port("udp")
        tenant="bootstrap-test-"+secrets.token_hex(8); network_secret=secrets.token_hex(32)
        policy=Path(tmp)/"host.toml"
        policy.write_text(f'''[flags]
disable_relay_data = true
relay_network_whitelist = ""
[[acl.acl_v1.chains]]
name = "bootstrap-inbound"
chain_type = 1
description = ""
enabled = true
default_action = 2
[[acl.acl_v1.chains.rules]]
name = "temporary-enrollment"
description = ""
priority = 1000
enabled = true
protocol = 1
ports = ["{allowed.port}"]
source_ips = []
destination_ips = []
source_ports = []
action = 1
rate_limit = 0
burst_limit = 0
stateful = true
source_groups = []
destination_groups = []
[[acl.acl_v1.chains]]
name = "deny-forward"
chain_type = 3
description = ""
enabled = true
default_action = 2
rules = []
''',encoding="utf-8")
        common=["--no-tun","--disable-ipv6","--disable-p2p","--disable-udp-hole-punching","--disable-tcp-hole-punching","--disable-upnp","--bind-device","false","--stun-servers","--stun-servers-v6","--file-log-level","off","--console-log-level","off"]
        host_env={**BASE,"ET_NETWORK_SECRET":network_secret}
        host=start("bootstrap-host",[*common,"--config-file",str(policy),"--hostname","bootstrap-host","--instance-name","bootstrap-host","--network-name",tenant,
              "--ipv4","10.199.82.1/24","--p2p-only",
              "--rpc-portal",f"127.0.0.1:{host_rpc}","--rpc-portal-whitelist","192.0.2.0/24",
              "--listeners",f"tcp://127.0.0.1:{host_listener}"],host_env)
        guard_env={**BASE,"ET_NETWORK_SECRET":secrets.token_hex(32)}
        guard=start("unrelated-guard",[*common,"--hostname","guard","--instance-name","guard","--network-name","guard-"+secrets.token_hex(8),
                    "--ipv4","10.199.83.1/24","--no-listener","--secure-mode","--rpc-portal",f"127.0.0.1:{guard_rpc}"],guard_env)
        client_env={**BASE,"ET_NETWORK_SECRET":network_secret}
        client=start("bootstrap-client",[*common,"--hostname","bootstrap-client","--instance-name","bootstrap-client","--network-name",tenant,
               "--ipv4","10.199.82.2/24","--p2p-only","--no-listener",
               "--rpc-portal",f"127.0.0.1:{client_rpc}","--peers",f"tcp://127.0.0.1:{host_listener}",
               "--port-forward",f"tcp://127.0.0.1:{allow_fwd}/10.199.82.1:{allowed.port}",
               "--port-forward",f"tcp://127.0.0.1:{denied_tcp_fwd}/10.199.82.1:{denied_tcp.port}",
               "--port-forward",f"udp://127.0.0.1:{denied_udp_fwd}/10.199.82.1:{denied_udp.port}"],client_env)
        def guard_healthy():
            r=run_cli(guard_rpc,"node","config");return r is not None and r.returncode==0 and "guard" in r.stdout
        require(wait(guard_healthy), "Unrelated management instance failed")
        def direct_route():
            r=run_cli(client_rpc,"route")
            return r is not None and r.returncode==0 and any("10.199.82.1/24" in x and "DIRECT" in x for x in r.stdout.splitlines())
        require(wait(direct_route), "Shared-secret direct route not established")
        require(wait(lambda:probe("tcp",allow_fwd)), "Allowed enrollment TCP did not succeed")
        print("Shared-secret direct bootstrap TCP: allowed",flush=True)
        before_tcp,before_udp=denied_tcp.count,denied_udp.count
        require(not probe("tcp",denied_tcp_fwd,2),"Other TCP unexpectedly succeeded")
        require(not probe("udp",denied_udp_fwd,2),"Other UDP unexpectedly succeeded")
        time.sleep(.5)
        require((denied_tcp.count,denied_udp.count)==(before_tcp,before_udp),"Denied service received data")
        require(allowed.count>counts_before[0],"Enrollment service did not receive data")
        print("Other TCP and UDP: rejected; destination counters unchanged",flush=True)
        # Stop only our host, then verify the independent manager remains accessible.
        stop(host)
        processes[:]=[(name,p) for name,p in processes if p is not host]
        require(guard.poll() is None and guard_healthy(),"Stopping host affected unrelated manager")
        print("Independent management process: unchanged after host stop",flush=True)
        print("PASS: no TUN, direct route, random shared secret, loopback-only setup",flush=True)
finally:
    for _,proc in processes:stop(proc)
    for server in servers:server.close()
