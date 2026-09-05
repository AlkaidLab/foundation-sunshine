"""Process-level TLS/attach regression tests; no USB driver or paired user data.

Build reverse_tunnel_probe, then run this script with its path. Optionally pass
--client-probe from moonlight-qt/tests/usb_forwarding_tunnel to test both real
transport implementations together. Put Qt/MSYS runtime DLLs on PATH on Windows.
"""
import argparse
import contextlib
import json
from pathlib import Path
import secrets
import socket
import ssl
import subprocess
import tempfile
import time


def wait_for(predicate, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.02)
    raise AssertionError("condition timed out")


def receive(sock, count):
    result = b""
    while len(result) < count:
        part = sock.recv(count - len(result))
        assert part, "unexpected EOF"
        result += part
    return result


def line(sock):
    result = b""
    while not result.endswith(b"\n"):
        result += receive(sock, 1)
        assert len(result) <= 4096
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_probe")
    parser.add_argument("--client-probe")
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()
    args.server_probe = str(Path(args.server_probe).resolve())
    if args.client_probe:
        args.client_probe = str(Path(args.client_probe).resolve())
    with tempfile.TemporaryDirectory(prefix="reverse-tunnel-test-") as directory:
        root = Path(directory)
        for name in ("server", "client", "wrong"):
            subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                            "-keyout", str(root / f"{name}.key"), "-out", str(root / f"{name}.crt"),
                            "-days", "1", "-subj", f"/CN={name}"],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        token = secrets.token_hex(24)
        (root / "token").write_text(token)
        (root / "wrong-token").write_text("wrong")
        sequence = 0

        @contextlib.contextmanager
        def server(mode="ok"):
            nonlocal sequence
            sequence += 1
            log = root / f"server-{sequence}.log"
            stop = root / f"server-{sequence}.stop"
            with log.open("w") as output:
                process = subprocess.Popen([args.server_probe, str(root / "server.crt"),
                    str(root / "server.key"), str(root / "client.crt"), str(root / "token"),
                    "0", "synthetic", str(stop), mode], stdout=output, stderr=subprocess.STDOUT)
                try:
                    wait_for(lambda: "LISTENING " in log.read_text())
                    port = int(log.read_text().split("LISTENING ")[1].splitlines()[0])
                    yield port, log
                finally:
                    stop.touch()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        raise AssertionError("service.stop() did not finish")
                    assert process.returncode == 0, f"exit={process.returncode}\n{log.read_text()}"

        def connect(port, identity="client"):
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            ctx.load_cert_chain(root / f"{identity}.crt", root / f"{identity}.key")
            return ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=5))

        def handshake(sock, supplied_token=token, tail=b""):
            sock.sendall(json.dumps({"op": "forward", "token": supplied_token,
                                     "busid": "1-1"}).encode() + b"\n" + tail)

        with server() as (port, log):
            for supplied_token in ("", "wrong"):
                with connect(port) as client:
                    handshake(client, supplied_token)
                    assert json.loads(line(client))["reason"] == "unauthorized"
            assert "IMPORT_EXCHANGED" not in log.read_text()
            for tail in (b"", b"REPLY\0\r\n"):
                before = log.read_text().count("DETACHED")
                attached_before = log.read_text().count("attached busid")
                with connect(port) as client:
                    handshake(client, tail=tail)
                    assert json.loads(line(client))["op"] == "ready"
                    assert receive(client, 8) == b"\0IMPORT\n"
                    if not tail:
                        client.sendall(b"REPLY\0\r\n")
                    wait_for(lambda: log.read_text().count("attached busid") > attached_before)
                wait_for(lambda: log.read_text().count("DETACHED") > before)
            try:
                with connect(port, "wrong") as client:
                    handshake(client)
                    assert client.recv(1) == b""
            except (ssl.SSLError, ConnectionError):
                pass
        print("PASS host: token rejection, peer rejection, ready-before-attach, tail preservation, detach/reconnect")

        with server("fail-after-import") as (port, log):
            with connect(port) as client:
                handshake(client)
                assert json.loads(line(client))["op"] == "ready"
                assert receive(client, 8) == b"\0IMPORT\n"
                client.sendall(b"REPLY\0\r\n")
                try:
                    assert client.recv(1) == b"", "late failure injected bytes into USB/IP"
                except (ConnectionError, ssl.SSLError):
                    # Windows closes an unclean shutdown with RST; the reset
                    # itself carries no USB/IP payload, which is the contract.
                    pass
        print("PASS host: late attach failure closes raw stream")

        # Teardown while attach is still waiting on its USB/IP reply must join
        # the worker and drain callbacks before destroying the TLS context.
        with server() as (port, log):
            client = connect(port)
            handshake(client)
            assert json.loads(line(client))["op"] == "ready"
            assert receive(client, 8) == b"\0IMPORT\n"
        with client:
            try:
                assert client.recv(1) == b""
            except (ConnectionError, ssl.SSLError):
                # Windows teardown closes with RST; no payload rides the reset.
                pass
        print("PASS host: stop cancels pending import and drains TLS callbacks")

        if args.client_probe:
            for name, pin, secret in (("valid", "server", "token"),
                                      ("bad-pin", "wrong", "token"),
                                      ("bad-token", "server", "wrong-token")):
                with server() as (port, host_log), socket.socket() as exporter:
                    exporter.bind(("127.0.0.1", 0))
                    exporter.listen(1)
                    exporter.settimeout(12)
                    stop = root / f"qt-{name}.stop"
                    log = root / f"qt-{name}.log"
                    with log.open("w") as output:
                        process = subprocess.Popen([args.client_probe, "127.0.0.1", str(port),
                            str(root / f"{pin}.crt"), str(root / "client.crt"), str(root / "client.key"),
                            str(root / secret), "1-1", str(exporter.getsockname()[1]), str(stop)],
                            stdout=output, stderr=subprocess.STDOUT)
                        try:
                            with exporter.accept()[0] as local:
                                local.settimeout(12)
                                if name == "valid":
                                    assert receive(local, 8) == b"\0IMPORT\n"
                                    local.sendall(b"REPLY\0\r\n")
                                    wait_for(lambda: "attached busid" in host_log.read_text())
                                    assert "FORWARDING" in log.read_text()
                                    stop.touch()
                                else:
                                    try:
                                        assert local.recv(1) == b"", "failure left exporter open or leaked data"
                                    except (ConnectionError, OSError):
                                        # Windows failure teardown uses RST.
                                        pass
                                process.wait(timeout=12)
                            assert process.returncode == (0 if name == "valid" else 1), log.read_text()
                            if name == "valid":
                                wait_for(lambda: "DETACHED" in host_log.read_text())
                            else:
                                assert "IMPORT_EXCHANGED" not in host_log.read_text()
                        finally:
                            if process.poll() is None:
                                process.kill()
                                process.wait()
                print(f"PASS both production transports: {name}")


if __name__ == "__main__":
    main()
