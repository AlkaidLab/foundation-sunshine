"""Build and run the optional JNI mock diagnostic on an explicitly selected device.

Uses installed SDK/NDK/JDK tools, an existing Android OpenSSL build and current
common-c sources. Physical devices require an explicit opt-in. No device output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "ndk", "openssl", "java-home", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--allow-physical-device", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    out = args.output.resolve()
    out.mkdir(exist_ok=False, parents=True)
    sdk, ndk, crypto, java = (p.resolve() for p in (args.sdk, args.ndk, args.openssl, args.java_home))
    tools = sorted((sdk / "build-tools").iterdir(), key=lambda p: tuple(map(int, p.name.split("."))))[-1]
    platform = sdk / "platforms" / "android-34" / "android.jar"
    cmake = sdk / "cmake" / "3.22.1" / "bin"
    adb = sdk / "platform-tools" / "adb.exe"
    package = "com.sunshine.matmock"

    def run(command, timeout=180):
        process = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding="utf8", errors="replace", timeout=timeout, cwd=root)
        with (out / "commands.log").open("a", encoding="utf8") as log:
            log.write(" ".join(map(str, command)) + "\n" + process.stdout + "\n")
        if process.returncode:
            raise RuntimeError(process.stdout + f"\nCommand failed: {command[0]}")
        return process.stdout.strip()

    def device(*command):
        return run([adb, "-s", args.serial, *command])

    emulator = args.serial.startswith("emulator-") and device("shell", "getprop", "ro.kernel.qemu") == "1"
    if not emulator and not args.allow_physical_device:
        raise RuntimeError("Physical-device installation requires --allow-physical-device and an explicit serial")
    abi = device("shell", "getprop", "ro.product.cpu.abi")
    if abi not in ("x86_64", "arm64-v8a"):
        raise RuntimeError("This diagnostic supports x86_64 and arm64-v8a")
    run([cmake / "cmake.exe", "-S", source, "-B", out / "native", "-G", "Ninja",
         f"-DCMAKE_MAKE_PROGRAM={cmake / 'ninja.exe'}",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build/cmake/android.toolchain.cmake'}",
         f"-DANDROID_ABI={abi}", "-DANDROID_PLATFORM=android-23", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         f"-DOPENSSL_ROOT={crypto}"])
    run([cmake / "cmake.exe", "--build", out / "native", "--parallel", "2"])
    classes, dex = out / "classes", out / "dex"
    classes.mkdir(); dex.mkdir()
    run([java / "bin/javac.exe", "-source", "8", "-target", "8", "-bootclasspath", platform,
         "-d", classes, *sorted(source.glob("*.java"))])
    run([java / "bin/java.exe", "-cp", tools / "lib/d8.jar", "com.android.tools.r8.D8",
         "--min-api", "23", "--lib", platform, "--output", dex, *classes.rglob("*.class")])
    unsigned, aligned, apk = (out / name for name in ("unsigned.apk", "aligned.apk", "mat-jni-mock.apk"))
    run([tools / "aapt2.exe", "link", "--manifest", source / "AndroidManifest.xml", "-I", platform, "-o", unsigned])
    with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(dex / "classes.dex", "classes.dex")
        archive.write(out / "native/libmatjni.so", f"lib/{abi}/libmatjni.so")
    run([tools / "zipalign.exe", "-f", "4", unsigned, aligned])
    keystore = out / "test.keystore"
    run([java / "bin/keytool.exe", "-genkeypair", "-keystore", keystore, "-storepass", "android",
         "-keypass", "android", "-alias", "matmock", "-dname", "CN=MAT JNI diagnostic",
         "-keyalg", "RSA", "-validity", "2"])
    run([java / "bin/java.exe", "-jar", tools / "lib/apksigner.jar", "sign", "--ks", keystore,
         "--ks-pass", "pass:android", "--out", apk, aligned])
    previous = device("shell", "getprop", "debug.checkjni")
    cleanup_errors = []
    try:
        device("shell", "setprop", "debug.checkjni", "1")
        device("install", "-r", "-t", apk)
        text = device("shell", "am", "instrument", "-w", package + "/.MatInstrumentation")
        (out / "instrumentation.log").write_text(text, encoding="utf8")
        match = re.search(r"^INSTRUMENTATION_RESULT: proof=(.+)$", text, re.MULTILINE)
        if not match:
            raise RuntimeError(text)
        proof = json.loads(match[1])
        assert proof["passed"] and len(proof["cases"]) == 10
        runtime = device("logcat", "-d", "-v", "brief", "--pid=" + str(proof["pid"]))
        (out / "runtime.log").write_text(runtime, encoding="utf8")
        assert "JNI DETECTED ERROR" not in runtime and "FATAL EXCEPTION" not in runtime
        assert "Late-enabling -Xcheck:jni" in runtime, "CheckJNI activation was not observed"
        proof.update(serial=args.serial, api=device("shell", "getprop", "ro.build.version.sdk"),
                     abi=abi, emulator=emulator, checkJniEnabled=True, nativeBuildType="RelWithDebInfo",
                     commonHead=run(["git", "-C", root / "third-party/moonlight-common-c", "rev-parse", "HEAD"]),
                     hostHead=run(["git", "rev-parse", "HEAD"]),
                     apkSha256=hashlib.sha256(apk.read_bytes()).hexdigest(),
                     cryptoSha256=hashlib.sha256((crypto / abi / "libcrypto.a").read_bytes()).hexdigest())
    finally:
        # Only the explicitly selected device and this diagnostic package are touched.
        for command in (("shell", "setprop debug.checkjni '" + previous + "'"), ("uninstall", package)):
            try:
                device(*command)
            except Exception as error:
                cleanup_errors.append(str(error))
                print("Diagnostic cleanup failed: " + str(error), file=sys.stderr)
    if cleanup_errors:
        raise RuntimeError("Diagnostic cleanup failed; inspect commands.log")
    proof["cleanupErrors"] = []
    (out / "proof.json").write_text(json.dumps(proof, indent=2), encoding="utf8")
    print(json.dumps(proof, indent=2))


if __name__ == "__main__":
    main()
