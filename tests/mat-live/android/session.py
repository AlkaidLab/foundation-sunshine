"""Process adapter for the isolated verifier; physical devices require opt-in."""
import json
import re
import subprocess


class AndroidClient:
    def __init__(self, adb, serial, apk, output, allow_physical_device=False):
        self.adb, self.serial, self.output = str(adb), serial, output
        self.process = self.log = None
        self.previous = None
        emulator = serial.startswith("emulator-") and self.device("shell", "getprop", "ro.kernel.qemu") == "1"
        if not emulator and not allow_physical_device:
            raise RuntimeError("Physical Android session requires explicit opt-in")
        self.previous = self.device("shell", "getprop", "debug.checkjni")
        try:
            self.device("shell", "setprop", "debug.checkjni", "1")
            self.device("install", "-r", "-t", str(apk.resolve()))
        except Exception as primary:
            try:
                self.close()
            except Exception as cleanup:
                primary.add_note("Android cleanup also failed: " + str(cleanup))
            raise

    def start(self, options):
        command = [self.adb, "-s", self.serial, "shell", "am", "instrument", "-w"]
        for key, value in options.items():
            command += ["-e", key, str(value)]
        command.append("com.sunshine.matmock/.MatInstrumentation")
        self.log = (self.output / "client.log").open("wb")
        self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT)

    def device(self, *command):
        run = subprocess.run([self.adb, "-s", self.serial, *command], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60, text=True, encoding="utf8", errors="replace")
        if run.returncode:
            raise RuntimeError(run.stdout)
        return run.stdout.strip()

    @property
    def pid(self):
        return self.process.pid

    @property
    def returncode(self):
        return self.process.returncode if self.process else None

    def poll(self):
        return self.process.poll() if self.process else 0

    def wait(self, timeout):
        code = self.process.wait(timeout=timeout)
        self.log.flush()
        text = (self.output / "client.log").read_text(encoding="utf8", errors="replace")
        match = re.search(r"^INSTRUMENTATION_RESULT: proof=(.+)$", text, re.MULTILINE)
        if not match:
            raise RuntimeError(text)
        proof = json.loads(match[1])
        runtime = self.device("logcat", "-d", "-v", "brief", "--pid=" + str(proof["pid"]))
        (self.output / "android-runtime.log").write_text(runtime, encoding="utf8")
        assert "JNI DETECTED ERROR" not in runtime and "FATAL EXCEPTION" not in runtime
        assert "Late-enabling -Xcheck:jni" in runtime, "CheckJNI activation was not observed"
        proof.update(android=True, checkJniEnabled=True)
        (self.output / "client-result.json").write_text(json.dumps(proof, indent=2), encoding="utf8")
        return code

    def close(self):
        if self.process and self.process.poll() is None:
            raise RuntimeError("Android instrumentation still running; no forced kill")
        if self.log:
            self.log.close()
        if self.previous is not None:
            self.device("shell", "setprop debug.checkjni '" + self.previous + "'")
            self.device("uninstall", "com.sunshine.matmock")
            self.previous = None
