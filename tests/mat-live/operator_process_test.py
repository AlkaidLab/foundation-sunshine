"""Windows launcher output/ownership regression. Ordinary child processes only."""
import argparse
import json
from pathlib import Path
import subprocess
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

    body = run_operator_command(command("new"), out / "new-file.log")
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
    failed = False
    try:
        run_operator_command([str(args.powershell), "-NoProfile", "-Command", "exit 7"], out / "failed.log")
    except RuntimeError:
        failed = True
    assert failed
    proof = {"result": "PASS", "nativeCapture": False,
             "checks": ["launcher returns while child is alive", "creation identity mismatch rejected",
                        "bounded wait", "exact child exit", "command failure"]}
    (out / "proof.json").write_text(json.dumps(proof, indent=2), encoding="utf8")
    print("PASS operator launcher return, child identity and bounded lifetime; nativeCapture=0")


if __name__ == "__main__":
    main()
