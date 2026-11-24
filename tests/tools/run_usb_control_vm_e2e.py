"""Two real USB control-transfer cycles through the production Qt/Sunshine tunnel.

The local Windows machine exports an already-shared Android USB device. A remote
Windows machine imports it. SSH access and both remote probes must already exist;
this script does not install drivers or modify device sharing. It stops local ADB
to release its device handle. Stop remote ADB before running, too.
Evidence folders contain temporary private keys; do not commit them.
"""
import argparse
import base64
import json
import secrets
import socket
import subprocess
import sys
from pathlib import Path

from test_reverse_tunnel import wait_for


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("ssh-target", "identity-file", "client-probe", "busid", "vid", "pid", "serial", "output"):
        parser.add_argument(f"--{name}", required=True)
    parser.add_argument("--remote-probe", default="C:/usb-e2e/reverse_tunnel_probe.exe")
    parser.add_argument("--remote-control-probe", default="C:/usb-e2e/usb_control_probe.exe")
    parser.add_argument("--remote-usbip", default="C:/Program Files/USBip/usbip.exe")
    parser.add_argument("--remote-workdir", default="C:/usb-e2e")
    parser.add_argument("--remote-port", type=int, default=24496)
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--adb", default="adb")
    args = parser.parse_args()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False)
    remote = args.remote_workdir.rstrip("/") + "/control-" + secrets.token_hex(8)
    ssh = ["ssh", "-i", args.identity_file, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
           "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
    evidence = []

    def encoded(script):
        script = "$ProgressPreference='SilentlyContinue'; [Console]::OutputEncoding=[Text.Encoding]::UTF8; " + script
        return "powershell -NoProfile -EncodedCommand " + base64.b64encode(script.encode("utf-16le")).decode()

    def ps(script):
        result = subprocess.run(ssh + [args.ssh_target, encoded(script)], capture_output=True, timeout=20)
        out = result.stdout.decode("utf-8", errors="replace")
        err = result.stderr.decode("utf-8", errors="replace")
        evidence.append(dict(command=script, exit=result.returncode, stdout=out, stderr=err))
        (root / "commands.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding="utf-8")
        print(out + err, flush=True)
        if result.returncode:
            raise RuntimeError(f"Remote command failed: {script}")
        return out

    def ports():
        return ps(f"& {quote(args.remote_usbip)} port; exit $LASTEXITCODE").strip()

    if ports():
        raise RuntimeError("Remote host has existing imports; will not disturb them")
    ps(f"New-Item -ItemType Directory -Path {quote(remote)} -ErrorAction Stop | Out-Null")
    for name in ("server", "client"):
        subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(root / f"{name}.key"), "-out", str(root / f"{name}.crt"),
                        "-days", "1", "-subj", f"/CN={name}"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (root / "token").write_text(secrets.token_hex(24))
    # Upload through the existing SSH channel, avoiding a second shell's path quoting.
    for name in ("server.crt", "server.key", "client.crt", "token"):
        data = base64.b64encode((root / name).read_bytes()).decode()
        script = f"[IO.File]::WriteAllBytes({quote(remote + '/' + name)}, [Convert]::FromBase64String('{data}'))"
        subprocess.run(ssh + [args.ssh_target, encoded(script)], check=True, timeout=20,
                       stdout=subprocess.DEVNULL)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        local_port = listener.getsockname()[1]
    host_log = root / "host.log"
    host_stop = remote + "/host.stop"
    host_args = [args.remote_probe, remote + "/server.crt", remote + "/server.key",
                 remote + "/client.crt", remote + "/token", str(args.remote_port),
                 args.remote_usbip, host_stop, "real"]
    host_text = lambda: host_log.read_text(encoding="utf-8", errors="replace")
    with host_log.open("w") as output:
        host = subprocess.Popen(ssh + ["-L", f"127.0.0.1:{local_port}:127.0.0.1:{args.remote_port}",
                                "-o", "ExitOnForwardFailure=yes", args.ssh_target,
                                encoded("& " + " ".join(map(quote, host_args)) + "; exit $LASTEXITCODE")],
                                stdout=output, stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: "LISTENING " in host_text(), 20)
            for cycle in range(2):
                subprocess.run([args.adb, "kill-server"], check=True, timeout=10)
                stop = root / f"client-{cycle}.stop"
                with (root / f"client-{cycle}.log").open("w") as log:
                    client = subprocess.Popen([str(Path(args.client_probe).resolve()), "127.0.0.1", str(local_port),
                        str(root / "server.crt"), str(root / "client.crt"), str(root / "client.key"),
                        str(root / "token"), args.busid, "3240", str(stop)], stdout=log, stderr=subprocess.STDOUT)
                    try:
                        wait_for(lambda: host_text().count("attached busid") > cycle, 20)
                        if not ports():
                            raise RuntimeError("Imported device missing")
                        command = "& " + " ".join(map(quote, [args.remote_control_probe, args.vid, args.pid, args.serial]))
                        if "PASS USB_CONTROL" not in ps(command + "; exit $LASTEXITCODE"):
                            raise RuntimeError("USB control-transfer probe failed")
                        print(f"PASS cycle {cycle + 1}: real USB control transfers over TLS", flush=True)
                    finally:
                        stop.touch()
                        try:
                            client.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            client.kill()
                            client.wait()
                        # Check cleanup even when functional validation raises an exception.
                        wait_for(lambda: not ports(), 15)
                    if client.returncode:
                        raise RuntimeError(f"Qt probe exited {client.returncode}")
                print(f"PASS cycle {cycle + 1}: imported port released", flush=True)
        finally:
            try:
                ps(f"New-Item -ItemType File -Path {quote(host_stop)} -Force | Out-Null")
            finally:
                try:
                    host.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    host.kill()
                    host.wait()
                subprocess.run([args.adb, "devices", "-l"], timeout=20)
                print(host_text(), flush=True)
        if host.returncode:
            raise RuntimeError(f"Sunshine probe exited {host.returncode}")
    print(f"PASS two-cycle USB control E2E; evidence: {root}", flush=True)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
