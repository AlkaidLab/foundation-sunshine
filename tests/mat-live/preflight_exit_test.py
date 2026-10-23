"""Preflight cleanup failures must fail the real CLI; only isolated hosts run."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import sys


def free_port():
    for base in (52410, 53100, 54100, 55100, 56100, 57100):
        try:
            for kind, offsets in ((socket.SOCK_STREAM, (-5, 0, 1, 21, 30)), (socket.SOCK_DGRAM, (9, 10, 11, 12))):
                for offset in offsets:
                    with socket.socket(socket.AF_INET, kind) as sock:
                        sock.bind(("127.0.0.1", base + offset))
        except OSError:
            continue
        return base
    raise RuntimeError("No isolated preflight port available")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--host-build", type=Path, required=True)
    parser.add_argument("--client-build", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(exist_ok=False)
    runner = out / "runner.py"
    runner.write_text('''import sys
mode, source = sys.argv[1:3]
del sys.argv[1:3]
sys.path.insert(0, source)
import live_verify
if mode != "success":
    original = live_verify.stop_own_host
    def stop(host):
        original(host)  # Actually stop the exact owned host before injecting failure.
        raise RuntimeError("INJECTED_CLEANUP_FAILURE")
    live_verify.stop_own_host = stop
if mode == "primary":
    import ui_preflight
    def verify(*args):
        raise ValueError("INJECTED_PRIMARY_FAILURE")
    ui_preflight.verify_ui = verify
live_verify.main()
''', encoding="utf8")
    cases = []
    for mode in ("success", "cleanup", "primary"):
        case = out / mode
        command = [sys.executable, str(runner), mode, str(Path(__file__).resolve().parent),
                   "--preflight-only", "--ui-preflight", "--output", str(case),
                   "--host-build", str(args.host_build.resolve()), "--client-build", str(args.client_build.resolve()),
                   "--port", str(free_port())]
        with (out / f"{mode}.log").open("wb") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        evidence = json.loads((case / "result.json").read_text(encoding="utf8"))
        text = (out / f"{mode}.log").read_text(encoding="utf8", errors="replace")
        assert evidence["hostExitCode"] == 0 and evidence["publisherExitCode"] is None
        assert evidence["passed"] == (mode == "success")
        assert (result.returncode == 0) == (mode == "success")
        if mode != "success":
            assert "INJECTED_CLEANUP_FAILURE" in evidence["cleanupErrors"]
        if mode == "primary":
            assert evidence["failure"] == "INJECTED_PRIMARY_FAILURE"
            assert "ValueError: INJECTED_PRIMARY_FAILURE" in text
        cases.append({"mode": mode, "exitCode": result.returncode, "hostStopped": True})
    (out / "proof.json").write_text(json.dumps({"result": "PASS", "nativeCapture": False, "cases": cases}, indent=2), encoding="utf8")
    print("PASS isolated preflight success, cleanup-failure exit, primary exception preservation; nativeCapture=0")


if __name__ == "__main__":
    main()
