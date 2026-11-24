#!/usr/bin/env python3
"""Test EasyTier 2.6.4's P2P-only policy on an isolated, local three-node topology.

The A-B connection intentionally passes through a local TCP fault injector. It is
used only to simulate disconnects; this is not proof of a physical direct path.
No TUN device, firewall rules, system routes, public peer, STUN or UPnP is used.
"""

import argparse
import contextlib
import os
import re
import secrets
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


class CheckFailed(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise CheckFailed(message)


class EchoServer:
    def __init__(self, protocol):
        self.protocol = protocol
        self.sock = socket.socket(
            socket.AF_INET,
            socket.SOCK_STREAM if protocol == "tcp" else socket.SOCK_DGRAM,
        )
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        if protocol == "tcp":
            self.sock.listen()
        self.port = self.sock.getsockname()[1]
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while self.running:
            try:
                if self.protocol == "tcp":
                    conn, _ = self.sock.accept()
                    with conn:
                        conn.settimeout(2)
                        data = conn.recv(1024)
                        if data:
                            conn.sendall(b"echo:" + data)
                else:
                    data, peer = self.sock.recvfrom(1024)
                    self.sock.sendto(b"echo:" + data, peer)
            except socket.timeout:
                continue
            except OSError:
                if not self.running:
                    return

    def close(self):
        self.running = False
        self.sock.close()
        self.thread.join(timeout=2)


class FaultInjector:
    """Local TCP splice to close the A-B transport without touching the OS firewall."""

    def __init__(self, destination_port):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.destination_port = destination_port
        self.running = True
        self.connections = []
        self.lock = threading.Lock()
        self.byte_count = 0
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        while self.running:
            try:
                incoming, _ = self.listener.accept()
                outgoing = socket.create_connection(("127.0.0.1", self.destination_port), 2)
                with self.lock:
                    if not self.running:
                        incoming.close()
                        outgoing.close()
                        return
                    self.connections.extend((incoming, outgoing))
                threading.Thread(target=self._forward, args=(incoming, outgoing), daemon=True).start()
            except socket.timeout:
                continue
            except OSError:
                return

    def _forward(self, a, b):
        try:
            while self.running:
                readable, _, _ = select.select((a, b), (), (), 0.2)
                for source in readable:
                    data = source.recv(65536)
                    if not data:
                        return
                    (b if source is a else a).sendall(data)
                    with self.lock:
                        self.byte_count += len(data)
        except OSError:
            pass
        finally:
            for connection in (a, b):
                with contextlib.suppress(OSError):
                    connection.close()

    def close(self):
        with self.lock:
            self.running = False
            connections = list(self.connections)
        self.listener.close()
        for connection in connections:
            with contextlib.suppress(OSError):
                connection.shutdown(socket.SHUT_RDWR)
            with contextlib.suppress(OSError):
                connection.close()
        self.thread.join(timeout=2)


class Lab:
    def __init__(self, core, cli, p2p_only):
        self.core, self.cli, self.p2p_only = str(core), str(cli), p2p_only
        self.env = {k: v for k, v in os.environ.items() if not k.upper().startswith("ET_")}
        self.creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.temp = tempfile.TemporaryDirectory(prefix="easytier-three-node-")
        self.network = "local-check-" + secrets.token_hex(8)
        self.secret = secrets.token_hex(32)
        self.processes = []
        self.servers = []
        self.injector = None
        self.allocated_ports = set()

    def allocate_port(self, protocol="tcp"):
        for _ in range(100):
            socktype = socket.SOCK_STREAM if protocol == "tcp" else socket.SOCK_DGRAM
            with socket.socket(socket.AF_INET, socktype) as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            if port not in self.allocated_ports:
                self.allocated_ports.add(port)
                return port
        raise CheckFailed("Could not allocate unique test ports")

    def run_cli(self, port, *args):
        try:
            result = subprocess.run(
                [self.cli, "-p", f"127.0.0.1:{port}", *args],
                env=self.env, capture_output=True, text=True, timeout=6,
                creationflags=self.creationflags,
            )
        except subprocess.TimeoutExpired as exc:
            raise CheckFailed("The test instance's RPC did not respond") from exc
        if result.returncode:
            raise CheckFailed(f"EasyTier CLI failed for {' '.join(args)}")
        return result.stdout

    def start(self, name, ip, rpc_port, listen_port, peer_port=None, forwards=()):
        args = [
            self.core, "--instance-name", name, "--network-name", self.network,
            "--network-secret", self.secret, "--ipv4", f"{ip}/24", "--no-tun",
            "--disable-ipv6", "--disable-p2p", "--disable-udp-hole-punching",
            "--disable-tcp-hole-punching", "--disable-upnp", "--bind-device", "false",
            "--stun-servers", "--stun-servers-v6", "--file-log-level", "off",
            "--rpc-portal", f"127.0.0.1:{rpc_port}",
            "--listeners", f"tcp://127.0.0.1:{listen_port}",
        ]
        if peer_port:
            args.extend(("--peers", f"tcp://127.0.0.1:{peer_port}"))
        if self.p2p_only and name != "center":
            args.append("--p2p-only")
        for protocol, port, target, destination_port in forwards:
            args.extend(("--port-forward", f"{protocol}://127.0.0.1:{port}/{target}:{destination_port}"))
        logfile = open(Path(self.temp.name) / f"{name}.log", "w", encoding="utf-8")
        try:
            proc = subprocess.Popen(
                args, stdout=logfile, stderr=subprocess.STDOUT, env=self.env,
                creationflags=self.creationflags,
            )
        except BaseException:
            logfile.close()
            raise
        self.processes.append((name, proc, logfile))

    def wait(self, predicate, message, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for name, proc, _ in self.processes:
                if proc.poll() is not None:
                    raise CheckFailed(f"Test instance {name} exited with code {proc.returncode}")
            try:
                if predicate():
                    return
            except CheckFailed:
                pass
            time.sleep(0.4)
        raise CheckFailed(message)

    def route_is(self, rpc_port, target_ip, hop):
        output = self.run_cli(rpc_port, "route")
        for line in output.splitlines():
            if target_ip + "/24" in line:
                fields = [field.strip() for field in line.split("|")]
                return len(fields) > 4 and fields[4] == hop
        return False

    def counters(self, rpc_port):
        output = self.run_cli(rpc_port, "stats", "prometheus")
        result = {}
        for line in output.splitlines():
            match = re.fullmatch(r"(traffic_(?:bytes|packets)_forwarded)\{[^}]*\}\s+([0-9.eE+-]+)", line)
            if match:
                result[match.group(1)] = float(match.group(2))
        require(len(result) == 2, "Center did not expose data-forwarding counters")
        return result

    @staticmethod
    def probe(protocol, port):
        socktype = socket.SOCK_STREAM if protocol == "tcp" else socket.SOCK_DGRAM
        with socket.socket(socket.AF_INET, socktype) as sock:
            sock.settimeout(1)
            try:
                token = secrets.token_bytes(16)
                if protocol == "tcp":
                    sock.connect(("127.0.0.1", port))
                    sock.sendall(token)
                    result = sock.recv(1024)
                else:
                    sock.sendto(token, ("127.0.0.1", port))
                    result, _ = sock.recvfrom(1024)
                return result == b"echo:" + token
            except OSError:
                return False

    def probes(self, forwards):
        return {name: self.probe(name.split("_")[1], port) for name, port in forwards.items()}

    def check(self):
        rpcs = {name: self.allocate_port() for name in ("center", "a", "b")}
        listeners = {name: self.allocate_port() for name in ("center", "a", "b")}
        services = {}
        forwards = {}
        for side in ("a", "b"):
            for protocol in ("tcp", "udp"):
                server = EchoServer(protocol)
                self.servers.append(server)
                services[f"{side}_{protocol}"] = server.port
                forwards[f"{side}_{protocol}"] = self.allocate_port(protocol)
        self.start("center", "10.199.88.1", rpcs["center"], listeners["center"])
        for side, ip, target in (("a", "10.199.88.2", "b"), ("b", "10.199.88.3", "a")):
            target_ip = "10.199.88.3" if target == "b" else "10.199.88.2"
            rules = [(p, forwards[f"{side}_{p}"], target_ip, services[f"{target}_{p}"])
                     for p in ("tcp", "udp")]
            self.start(side, ip, rpcs[side], listeners[side], listeners["center"], rules)

        self.wait(lambda: self.route_is(rpcs["a"], "10.199.88.3", "10.199.88.1/24")
                  and self.route_is(rpcs["b"], "10.199.88.2", "10.199.88.1/24"),
                  "Two-hop A-C-B topology did not become ready")
        before = self.counters(rpcs["center"])
        if self.p2p_only:
            result = self.probes(forwards)
            require(not any(result.values()), f"P2P-only relayed data on baseline: {result}")
        else:
            self.wait(lambda: all(self.probes(forwards).values()),
                      "Relay-only positive control did not carry TCP/UDP in both directions")
        after = self.counters(rpcs["center"])
        require((after["traffic_packets_forwarded"] == before["traffic_packets_forwarded"])
                if self.p2p_only else (after["traffic_packets_forwarded"] > before["traffic_packets_forwarded"]),
                "Center forwarding counter disagreed with relay-only control")
        print(f"  A-C-B only: {'blocked' if self.p2p_only else 'TCP/UDP relayed'}; center data packets "
              f"{before['traffic_packets_forwarded']:.0f} -> {after['traffic_packets_forwarded']:.0f}", flush=True)

        self.injector = FaultInjector(listeners["b"])
        url = f"tcp://127.0.0.1:{self.injector.port}"
        self.run_cli(rpcs["a"], "connector", "add", url)
        self.wait(lambda: self.route_is(rpcs["a"], "10.199.88.3", "DIRECT")
                  and self.route_is(rpcs["b"], "10.199.88.2", "DIRECT"),
                  "Direct peer route did not appear on both endpoints")
        before = self.counters(rpcs["center"])
        self.wait(lambda: all(self.probes(forwards).values()),
                  "Direct peer route did not carry TCP/UDP in both directions")
        after = self.counters(rpcs["center"])
        require(self.injector.byte_count > 0, "Fault injector did not carry the A-B link")
        require(after["traffic_packets_forwarded"] == before["traffic_packets_forwarded"],
                "Center forwarded data during direct peer test")
        print("  A-B via local fault-injection splice: TCP/UDP passed; center forwarded no data", flush=True)

        self.run_cli(rpcs["a"], "connector", "remove", url)
        self.injector.close()
        self.injector = None
        self.wait(lambda: self.route_is(rpcs["a"], "10.199.88.3", "10.199.88.1/24")
                  and self.route_is(rpcs["b"], "10.199.88.2", "10.199.88.1/24"),
                  "A-C-B route was not restored after the injected disconnect")
        before = self.counters(rpcs["center"])
        if self.p2p_only:
            result = self.probes(forwards)
            require(not any(result.values()), f"P2P-only relayed data after disconnect: {result}")
        else:
            self.wait(lambda: all(self.probes(forwards).values()),
                      "Relay-only positive control did not recover TCP/UDP after disconnect")
        after = self.counters(rpcs["center"])
        require((after["traffic_packets_forwarded"] == before["traffic_packets_forwarded"])
                if self.p2p_only else (after["traffic_packets_forwarded"] > before["traffic_packets_forwarded"]),
                "Center forwarding counter disagreed after disconnect")
        print(f"  A-B cut: {'blocked' if self.p2p_only else 'TCP/UDP relayed again'}; center data packets "
              f"{before['traffic_packets_forwarded']:.0f} -> {after['traffic_packets_forwarded']:.0f}", flush=True)

    def close(self):
        if self.injector is not None:
            self.injector.close()
        for _, process, _ in self.processes:
            if process.poll() is None:
                process.terminate()
        for _, process, logfile in self.processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            logfile.close()
        for server in self.servers:
            server.close()
        self.temp.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True, help="Path to EasyTier 2.6.4 core")
    parser.add_argument("--cli", type=Path, required=True, help="Path to matching EasyTier 2.6.4 CLI")
    args = parser.parse_args()
    for binary in (args.core, args.cli):
        require(binary.is_file(), f"Binary not found: {binary}")
        result = subprocess.run([str(binary), "--version"], capture_output=True, text=True,
                                timeout=10, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        require(result.returncode == 0 and re.search(r"\b2\.6\.4(?:\b|-)", result.stdout) is not None,
                f"Expected a verified 2.6.4 binary: {binary}")
    print("Isolated localhost topology; no TUN, STUN, UPnP, firewall or route changes.")
    print("A-B uses a local fault-injection splice; it is not proof of physical direct connectivity.")
    for only in (False, True):
        print(f"Policy: p2p_only={str(only).lower()}", flush=True)
        lab = Lab(args.core, args.cli, only)
        try:
            lab.check()
        finally:
            lab.close()
    print("PASS: both policies met the three-stage assertions.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (CheckFailed, OSError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
