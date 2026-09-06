"""Windows hardware E2E using production Sunshine/Qt tunnel classes.

Requires an already-shared usbipd busid and an ADB device. Imports the device,
executes ADB through the imported device, releases it, then repeats to prove
reconnect. Does not install drivers, change sharing, or touch running Sunshine.
Logs and ephemeral test credentials are written to --output (do not commit it).
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import subprocess
import time

from test_reverse_tunnel import wait_for


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server-probe", "client-probe", "usbip", "busid", "adb-serial", "output"):
        parser.add_argument(f"--{name}", required=True)
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--adb", default="adb")
    args = parser.parse_args()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False)
    for name in ("server", "client"):
        subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(root / f"{name}.key"), "-out", str(root / f"{name}.crt"),
            "-days", "1", "-subj", f"/CN={name}"], check=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (root / "token").write_text(secrets.token_hex(24))
    env = os.environ.copy()
    commands = []

    def run(command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=15, env=env)
        commands.append({"command": command, "exit": result.returncode,
                         "stdout": result.stdout, "stderr": result.stderr})
        (root / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
        return result

    baseline = run([args.usbip, "port"])
    assert baseline.returncode == 0, baseline.stderr
    assert not baseline.stdout.strip(), "run on a host with no pre-existing USB/IP imports"
    host_log = root / "host.log"
    host_stop = root / "host.stop"
    def host_text():
        return host_log.read_text(encoding="utf-8", errors="replace")
    with host_log.open("w") as output:
        host = subprocess.Popen([str(Path(args.server_probe).resolve()), str(root / "server.crt"),
            str(root / "server.key"), str(root / "client.crt"), str(root / "token"), "0",
            str(Path(args.usbip).resolve()), str(host_stop), "real"], env=env,
            stdout=output, stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: "LISTENING " in host_text())
            port = host_text().split("LISTENING ")[1].splitlines()[0]
            for cycle in range(2):
                # ADB's handle to the physical devnode vetoes usbipd's restart.
                # Reopen ADB only after the device exists on the virtual hub.
                assert run([args.adb, "kill-server"]).returncode == 0
                log = root / f"client-{cycle}.log"
                stop = root / f"client-{cycle}.stop"
                before = host_text().count("attached busid")
                with log.open("w") as output:
                    client = subprocess.Popen([str(Path(args.client_probe).resolve()), "127.0.0.1", port,
                        str(root / "server.crt"), str(root / "client.crt"), str(root / "client.key"),
                        str(root / "token"), args.busid, "3240", str(stop)], env=env,
                        stdout=output, stderr=subprocess.STDOUT)
                    try:
                        wait_for(lambda: host_text().count("attached busid") > before, 15)
                        imported = run([args.usbip, "port"])
                        assert imported.returncode == 0 and imported.stdout.strip(), imported.stderr
                        run([args.adb, "reconnect"])
                        def adb_ready():
                            time.sleep(0.5)
                            result = run([args.adb, "-s", args.adb_serial, "shell", "echo", "REVERSE_USB_E2E_OK"])
                            return result.returncode == 0 and "REVERSE_USB_E2E_OK" in result.stdout
                        wait_for(adb_ready, 30)
                        state = run(["usbipd", "state"])
                        assert state.returncode == 0, state.stderr
                        devices = json.loads(state.stdout)["Devices"]
                        device = next(d for d in devices if d.get("BusId") == args.busid)
                        assert device.get("ClientIPAddress"), "original device is not exported to USB/IP"
                        print(f"PASS cycle {cycle + 1}: imported device + ADB command over TLS", flush=True)
                    finally:
                        stop.touch()
                        try:
                            client.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            client.kill()
                            client.wait()
                    wait_for(lambda: not run([args.usbip, "port"]).stdout.strip(), 15)
                    assert client.returncode == 0, log.read_text()
                    print(f"PASS cycle {cycle + 1}: detach released all imported ports", flush=True)
        finally:
            host_stop.touch()
            try:
                host.wait(timeout=15)
            except subprocess.TimeoutExpired:
                host.kill()
                host.wait()
            print(host_text(), flush=True)
        assert host.returncode == 0
    print(f"PASS real hardware E2E; evidence: {root}", flush=True)


if __name__ == "__main__":
    main()
