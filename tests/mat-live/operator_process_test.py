"""Windows launcher output/ownership regression. Ordinary child processes only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
from live_verify import OwnedPublisher, run_operator_command


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--powershell", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(exist_ok=False)
    script = out / "launcher.ps1"
    script.write_text(r'''param([string]$Output)
$ErrorActionPreference='Stop'
$p=Start-Process -FilePath (Get-Process -Id $PID).Path -WindowStyle Hidden -ArgumentList @('-NoProfile','-Command','Start-Sleep -Milliseconds 2500') -RedirectStandardOutput "$Output.out" -RedirectStandardError "$Output.err" -PassThru
@{publisherPid=$p.Id;publisherCreated=$p.StartTime.ToFileTimeUtc().ToString()}|ConvertTo-Json -Compress
''', encoding="utf8")

    def command(label):
        return [str(args.powershell), "-NoProfile", "-File", str(script), "-Output", str(out / label)]

    began = time.monotonic()
    old = subprocess.run(command("old"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         timeout=8, creationflags=subprocess.CREATE_NO_WINDOW)
    old_elapsed = time.monotonic() - began
    (out / "old-pipe.log").write_bytes(old.stdout)
    assert old.returncode == 0 and old_elapsed >= 2.5, "Old inherited-pipe wait not reproduced"

    began = time.monotonic()
    body = run_operator_command(command("new"), out / "new-file.log")
    fixed_elapsed = time.monotonic() - began
    state = json.loads(body)
    child = OwnedPublisher(state)
    try:
        assert child.poll() is None, "Launcher still waited for its live child"
        wrong = dict(state, publisherCreated=str(int(state["publisherCreated"]) + 1))
        try:
            OwnedPublisher(wrong)
        except RuntimeError:
            pass
        else:
            raise AssertionError("Wrong process creation identity accepted")
        try:
            child.wait(.01)
        except subprocess.TimeoutExpired:
            pass
        else:
            raise AssertionError("Bounded child wait did not time out")
        assert child.wait(8) == 0
    finally:
        child.close()
    assert fixed_elapsed < old_elapsed - 1, "Launcher EOF wait regression"
    failed = False
    try:
        run_operator_command([str(args.powershell), "-NoProfile", "-Command", "exit 7"], out / "failed.log")
    except RuntimeError:
        failed = True
    assert failed
    files = [Path(__file__), Path(__file__).with_name("live_verify.py")]
    proof = {"result": "PASS", "nativeCapture": False, "oldPipeSeconds": old_elapsed,
             "fixedFileSeconds": fixed_elapsed, "childStillAliveAtLauncherReturn": True,
             "checks": ["inherited output-pipe wait reproduced", "file logging returns before child exits",
                        "creation identity mismatch rejected", "bounded wait", "exact child exit", "command failure"],
             "sources": [{"name": f.name, "sha256": hashlib.sha256(f.read_bytes()).hexdigest()} for f in files]}
    (out / "proof.json").write_text(json.dumps(proof, indent=2), encoding="utf8")
    print(f"PASS operator inherited-pipe regression: old={old_elapsed:.3f}s fixed={fixed_elapsed:.3f}s; nativeCapture=0")


if __name__ == "__main__":
    main()
