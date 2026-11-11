"""Full RTSP/video acceptance with Windows or Android mock output. No replay.

The shared test helpers handle isolated certificates and orderly host shutdown.
No installed host is modified.
Native MAT mode needs explicit authorization; Android Opus mode never attaches.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET
from host_support import cert_pair, check_bridge_absent, stop_own_host


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf8")


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def log_text(path):
    return path.read_text(encoding="utf8", errors="replace") if path.exists() else ""


def signal_stop(name):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_wchar_p]
    kernel.OpenEventW.restype = ctypes.c_void_p
    kernel.SetEvent.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    event = kernel.OpenEventW(2, 0, name)
    if not event:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not kernel.SetEvent(event):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(event)


def run_operator_command(command, log_path, timeout=25):
    # Start-Process may leave an inherited copy of the parent's stdout pipe in
    # its long-lived native child. communicate(PIPE) then waits for that child,
    # even after the launcher exited. A file requires no pipe EOF to return.
    with log_path.open("wb") as log:
        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                   timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW)
    if completed.returncode:
        raise RuntimeError(f"Operator command failed; inspect {log_path.name}")
    return log_path.read_bytes()


class OwnedPublisher:
    """Wait on the exact process started by the operator control script."""
    def __init__(self, state):
        self.pid = state["publisherPid"]
        self.returncode = None
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        self.kernel.OpenProcess.restype = ctypes.c_void_p
        self.kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.c_void_p] * 4
        self.kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.handle = self.kernel.OpenProcess(0x100000 | 0x1000, 0, self.pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        created, exited, system, user = (ctypes.c_ulonglong() for _ in range(4))
        if not self.kernel.GetProcessTimes(self.handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)) or created.value != int(state["publisherCreated"]):
            self.close()
            raise RuntimeError("Operator publisher creation identity changed")

    def poll(self):
        if self.returncode is None and self.kernel.WaitForSingleObject(self.handle, 0) == 0:
            code = ctypes.c_ulong()
            if not self.kernel.GetExitCodeProcess(self.handle, ctypes.byref(code)):
                raise ctypes.WinError(ctypes.get_last_error())
            self.returncode = code.value
        return self.returncode

    def wait(self, timeout):
        if self.kernel.WaitForSingleObject(self.handle, int(timeout * 1000)) != 0:
            raise subprocess.TimeoutExpired("owned native publisher", timeout)
        return self.poll()

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--host-build", type=Path, required=True)
    p.add_argument("--client-build", type=Path)
    p.add_argument("--adb", type=Path)
    p.add_argument("--android-serial")
    p.add_argument("--android-apk", type=Path)
    p.add_argument("--android-session-only", action="store_true",
                   help="Full Android session with Opus fallback, no native capture")
    p.add_argument("--port", type=int, default=52410)
    p.add_argument("--preflight-only", action="store_true")
    preflight_checks = p.add_mutually_exclusive_group()
    preflight_checks.add_argument("--ui-preflight", action="store_true")
    preflight_checks.add_argument("--ticket-preflight", action="store_true")
    p.add_argument("--publisher", type=Path)
    p.add_argument("--tap", type=Path)
    p.add_argument("--tap-sha")
    p.add_argument("--codec", type=Path)
    p.add_argument("--codec-sha")
    p.add_argument("--probe", type=Path)
    p.add_argument("--engine-pid", type=int)
    p.add_argument("--engine-created")
    p.add_argument("--endpoint-id")
    p.add_argument("--seconds", type=int, default=20)
    p.add_argument("--bed-7.1.4", dest="bed714", action="store_true")
    p.add_argument("--reattach", action="store_true")
    p.add_argument("--operator-build", type=Path)
    p.add_argument("--powershell", type=Path)
    args = p.parse_args()
    android = all((args.adb, args.android_serial, args.android_apk))
    if any((args.adb, args.android_serial, args.android_apk)) and not android:
        p.error("Android needs explicit ADB, emulator serial and diagnostic APK")
    if not android and not args.client_build:
        p.error("Explicit client build or Android diagnostic APK required")
    if args.android_session_only and (not android or args.preflight_only):
        p.error("Android software session requires Android arguments, without --preflight-only")
    client_binary = args.android_apk if android else args.client_build / "mat-live-session-client.exe"
    if args.ui_preflight and not args.preflight_only:
        p.error("UI preflight needs --preflight-only")
    if args.ticket_preflight and not args.preflight_only:
        p.error("Ticket preflight needs --preflight-only")
    if bool(args.operator_build) != bool(args.powershell):
        p.error("Operator build and explicit PowerShell executable must be paired")
    if not 8 <= args.seconds <= 30:
        p.error("Native capture duration must be 8..30 seconds")
    if not args.preflight_only and not args.android_session_only and not all((args.publisher, args.tap, args.tap_sha, args.codec, args.codec_sha,
                                          args.probe, args.engine_pid, args.engine_created, args.endpoint_id)):
        p.error("Explicit pinned native producer/engine/endpoint arguments required")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    check_bridge_absent()
    for typ, offsets in ((socket.SOCK_STREAM, (-5, 0, 1, 21, 30)), (socket.SOCK_DGRAM, (9, 10, 11, 12))):
        for offset in offsets:
            with socket.socket(socket.AF_INET, typ) as sock:
                sock.bind(("127.0.0.1", args.port + offset))
    cert_pair(out, "server")
    client_pem = cert_pair(out, "client")
    identity = str(uuid.uuid4())
    save(out / "state.json", {"root": {"uniqueid": str(uuid.uuid4()), "named_devices": [
        {"name": "Native live MAT RTSP acceptance", "uuid": identity, "cert": client_pem}]}})
    salt = uuid.uuid4().hex[:16]
    password = uuid.uuid4().hex
    save(out / "creds.json", {"username": "mat-live-local", "salt": salt,
                              "password": hashlib.sha256((password + salt).encode()).hexdigest()})
    save(out / "apps.json", {"env": {}, "apps": [{"name": "Live desktop", "cmd": "",
          "image-path": "desktop.png", "exclude-global-prep-cmd": "true"}]})
    identifier = uuid.uuid4().hex
    pipe = "\\\\.\\pipe\\SunshineMatLive-active"
    stop = "Local\\SunshineMatLiveStop-" + identifier
    config = {"port": args.port, "bind_address": "127.0.0.1", "address_family": "ipv4",
        "mdns_broadcast": "false", "upnp": "false", "system_tray": "false", "min_log_level": "info",
        "mouse": "false", "keyboard": "false", "controller": "false", "virtual_mouse": "false",
        "native_pen_touch": "false", "native_touchpad_optimization": "false", "clipboard_sync": "false",
        "enable_dsu_server": "false", "client_gamepad_override": "false", "usb_forwarding_enabled": "false",
        "install_steam_audio_drivers": "false", "stream_mic": "false", "keep_sink_default": "false",
        "stream_audio": "true", "mat_live": "true",
        "vdd_headless_create": "false", "vdd_keep_enabled": "true", "vdd_vulkan_hdr_bridge": "false",
        "display_device_prep": "no_operation", "resolution_change": "no_operation",
        "refresh_rate_change": "no_operation", "hdr_prep": "no_operation", "client_fingerprint_remote_rules": "false",
        "file_mapping_port": args.port + 30, "file_mappings": "[]", "lan_encryption_mode": 0,
        "wan_encryption_mode": 0, "encoder": "software", "hevc_mode": 1, "av1_mode": 1,
        "file_state": (out / "state.json").as_posix(), "credentials_file": (out / "creds.json").as_posix(),
        "pkey": (out / "server.key").as_posix(), "cert": (out / "server.pem").as_posix(),
        "file_apps": (out / "apps.json").as_posix(), "log_path": (out / "host.log").as_posix()}
    if args.ticket_preflight:
        config.update(ping_timeout=1000, min_log_level="debug", stream_audio="false")
    (out / "sunshine.conf").write_text("\n".join(f"{k} = {v}" for k, v in config.items()) + "\n", encoding="utf8")
    portable = out / "host"
    portable.mkdir()
    shutil.copy2(args.host_build / "sunshine.exe", portable / "sunshine.exe")
    shutil.copytree(args.host_build / "assets", portable / "assets")
    (portable / "config").mkdir()
    env = os.environ.copy()
    # Process-local opt-out of installed NVIDIA preference recovery; no system change.
    for key in list(env):
        if key.upper() == "PROGRAMDATA":
            del env[key]
    env["PATH"] = "C:/msys64/ucrt64/bin;" + env.get("PATH", "")
    context = ssl.create_default_context(cafile=str(out / "server.pem"))
    context.check_hostname = False
    context.load_cert_chain(str(out / "client.pem"), str(out / "client.key"))
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=context))

    def request(route, query=None, timeout=25):
        params = {"uniqueid": identity, "uuid": str(uuid.uuid4()), "clientname": "Live MAT local acceptance"}
        params.update(query or {})
        url = f"https://127.0.0.1:{args.port-5}/{route}?" + urllib.parse.urlencode(params)
        with opener.open(url, timeout=timeout) as response:
            body = response.read()
        tree = ET.fromstring(body)
        if tree.attrib.get("status_code") != "200":
            raise RuntimeError(body.decode())
        return body, tree

    host = publisher = source = client = None
    logs = []
    launched = False
    result = {"passed": False, "preflightOnly": args.preflight_only, "sampleReplayUsed": False,
              "audioPayloadWrittenToDisk": False, "videoPayloadWrittenToDisk": False,
              "hardwareSink": False, "avSyncCalibrated": False,
              "nativeCaptureSeconds": 0 if args.android_session_only or args.preflight_only else args.seconds}
    capture_path = out / "capture.log"
    operator_script = Path(__file__).resolve().parents[2] / "tools/sonic-mat-live/manage-live.ps1"

    def operator(action, extra=()):
        command = [str(args.powershell), "-NoProfile", "-File", str(operator_script), "-Action", action, *extra]
        return run_operator_command(command, out / f"operator-{action.lower()}.log")

    def start(command, name, cwd=None, process_env=None):
        log = (out / f"{name}.log").open("wb")
        logs.append(log)
        return subprocess.Popen(command, cwd=cwd, env=process_env, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW)

    def wait_log(process, name, marker, seconds):
        deadline = time.monotonic() + seconds
        path = capture_path if name == "capture" else out / f"{name}.log"
        while marker not in log_text(path):
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError(f"{name} missing {marker}; no retry")
            time.sleep(.01)

    try:
        host = start([str(portable / "sunshine.exe"), str(out / "sunshine.conf")], "host-console", portable, env)
        save(out / "process.json", {"hostPid": host.pid, "port": args.port, "pipe": pipe, "stopEvent": stop})
        deadline = time.monotonic() + 90
        while True:
            if host.poll() is not None:
                raise RuntimeError(f"Host exited {host.returncode}")
            try:
                info_bytes, info = request("serverinfo", timeout=2)
                break
            except (OSError, urllib.error.URLError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.2)
        (out / "serverinfo.xml").write_bytes(info_bytes)
        apps_bytes, apps = request("applist")
        (out / "applist.xml").write_bytes(apps_bytes)
        assert info.findtext("PairStatus") == "1" and apps.findtext("App/ID")
        result["isolatedTlsPairing"] = True
        print("PREFLIGHT paired isolated Sunshine server ready", flush=True)
        if args.preflight_only:
            if args.ticket_preflight:
                from ticket_preflight import verify_ticket_expiry
                launched = True  # Keep cancellation in the existing guarded cleanup.
                result["ticketExpiry"] = verify_ticket_expiry(request, apps.findtext("App/ID"), out / "host.log")
            if args.ui_preflight:
                from ui_preflight import verify_ui
                result["uiStatus"] = verify_ui(opener, args.port, config, out / "sunshine.conf")
            result["passed"] = True
            return
        if android:
            from android.session import AndroidClient
            client = AndroidClient(args.adb, args.android_serial, args.android_apk, out)
        if not args.android_session_only:
            for dll, expected in ((args.tap, args.tap_sha), (args.codec, args.codec_sha)):
                assert sha(dll) == expected
            mode = "--reattach" if args.reattach else "--attach"
            if args.operator_build:
                options = ["-Build", str(args.operator_build), "-Output", str(out / "operator"),
                           "-EnginePid", str(args.engine_pid), "-EngineCreated", args.engine_created,
                           "-Seconds", str(args.seconds), "-SessionId", identifier]
                if args.reattach:
                    options.append("-Reattach")
                operator("Start", options)
                state = json.loads((out / "operator" / "capture.json").read_text(encoding="utf-8-sig"))
                publisher = OwnedPublisher(state)
                assert Path(state["producer"]).resolve() == args.publisher.resolve() and state["pipe"] == pipe and state["stopEvent"] == stop
                assert not state["continuous"] and state["maximumSeconds"] == args.seconds
                capture_path = out / "operator" / "capture.log"
            else:
                publisher = start([str(args.publisher), mode, str(args.engine_pid), args.engine_created,
                      str(args.tap), args.tap_sha, str(args.codec), args.codec_sha, pipe, stop,
                      "--max-seconds", str(args.seconds)], "capture")
            wait_log(publisher, "capture", "ATTACHED source=plugin-objects-to-native-MAT", 3)
            source = start([str(args.probe), "--emit", "--endpoint", args.endpoint_id, "--seconds",
                             str(args.seconds + 2), "--objects", "2", "--variant", "0"] +
                           (["--bed-7.1.4"] if args.bed714 else []), "source")
            wait_log(publisher, "capture", "NATIVE_MAT_READY", 5)
            if args.operator_build:
                status = json.loads(operator("Status", ["-Metadata", str(out / "operator" / "capture.json")]))
                assert status["state"] == "NativeReady" and status["publisherAlive"] and status["engineIdentityRetained"]
                result["operatorObservedNativeReady"] = True
        key = os.urandom(16).hex()
        iv = os.urandom(4) + bytes(12)
        body, launch = request("launch", {"appid": apps.findtext("App/ID"), "rikey": key,
            "rikeyid": int.from_bytes(iv[:4], "big"), "mode": "640x360x30",
            "localAudioPlayMode": 1 if args.android_session_only else 0,
            "continuousAudio": 1 if args.android_session_only else 0,
            "surroundAudioInfo": 196610, "sops": 0, "useVdd": 0, "customScreenMode": 0,
            "touchKeyboard": 0, "hdrMode": 0, "corever": 1, "remoteControllersBitmap": 0, "gcmap": 0, "gcpersist": 0})
        launched = True
        (out / "launch.xml").write_bytes(body)
        session_url = launch.findtext("sessionUrl0")
        assert session_url and session_url.startswith("rtspenc://")
        if android:
            client.start({
                "sessionHost": "10.0.2.2", "sessionUrl": session_url.replace("127.0.0.1", "10.0.2.2"),
                "appVersion": info.findtext("appversion"), "gfeVersion": info.findtext("GfeVersion"),
                "serverCodecMask": info.findtext("ServerCodecModeSupport"), "inputKey": key, "inputIv": iv.hex(),
                "audioCodec": 6 if args.android_session_only else 7,
                "expectedCodec": 0 if args.android_session_only else 7,
                "seconds": args.seconds if args.android_session_only else args.seconds + 10})
        else:
            client = start([str(client_binary.resolve()), session_url,
                 info.findtext("appversion"), info.findtext("GfeVersion"), info.findtext("ServerCodecModeSupport"),
                 key, iv.hex(), str(out / "client-result.json"), str(args.seconds + 10)], "client")
        print(f"FULL_RTSP client={client.pid} host={host.pid} nativeCapture={publisher is not None}", flush=True)
        client.wait(timeout=args.seconds + 30)
        if args.android_session_only:
            status = json.loads((out / "client-result.json").read_text())
            assert client.returncode == 0 and status["passed"] and status["codec"] == 0
            assert status["fullLiStartConnection"] and status["videoFramesDecoded"] >= 30
            result.update(passed=True, client=status, fullSunshineRtspSession=True, encryptedRtsp=True,
                          source="Sunshine continuous Opus fallback (idle silence); compressed audio mock; Android MediaCodec H264")
            return
        publisher.wait(timeout=8)
        source.wait(timeout=8)
        status = json.loads((out / "client-result.json").read_text())
        capture_log = log_text(capture_path)
        match = re.search(r"LEASE_END_ACK cycles=(\d+) sha256=([0-9a-f]{64})", capture_log)
        assert match, "Native publisher did not confirm stream SHA/EOS"
        assert client.returncode == publisher.returncode == source.returncode == 0
        assert status["passed"] and status["cycles"] == int(match[1]) and status["sha256"] == match[2]
        assert status["mockOutputCompleted"] and status["mockPlayedFrames"] == status["cycles"] * 3840
        assert status["mockWrittenSha256"] == status["mockPlayedSha256"] == match[2]
        assert "HOOK_STOP restored=1" in capture_log and "PLUGIN_STOP" in capture_log
        if args.bed714:
            assert "staticMask=0x1ffe dynamicObjects=2" in capture_log, "Full bed/dynamic input not observed"
            rounds = re.search(r"PLUGIN_STOP passes=(\d+) inputSamples=(\d+)", capture_log)
            assert rounds and int(rounds[1]) > 0 and int(rounds[2]) == int(rounds[1]) * 14 * 480
            result["staticBedMask"] = 0x1ffe
            result["staticBedChannels"] = 12
            result["dynamicObjects"] = 2
        host_log = log_text(out / "host.log") + log_text(out / "host-console.log")
        assert f"Native MAT capture ended cleanly: {status['cycles']} cycles" in host_log
        result.update(passed=True, client=status, producerSha256=match[2], producerCycles=int(match[1]),
                      fullSunshineRtspSession=True, encryptedRtsp=True,
                      source="live ISpatialAudioClient 7.1.4 bed + two-object generator" if args.bed714 else "live ISpatialAudioClient two-object generator")
        print(f"PASS_FULL_NATIVE_RTSP cycles={status['cycles']} decodedVideoFrames={status['videoFramesDecoded']} sha256={status['sha256']}", flush=True)
    except Exception as error:
        result["failure"] = str(error)
        raise
    finally:
        cleanup_errors = []
        if publisher and publisher.poll() is None:
            try:
                signal_stop(stop)
                publisher.wait(timeout=12)
            except Exception as e:
                cleanup_errors.append(str(e))
        if launched:
            try:
                request("cancel", timeout=10)
            except Exception as e:
                cleanup_errors.append(str(e))
        for process, label in ((client, "client"), (source, "source")):
            if process and process.poll() is None:
                try:
                    process.wait(timeout=args.seconds + 5)
                except Exception:
                    cleanup_errors.append(f"{label} PID {process.pid} left running; no forced kill")
        if host:
            try:
                stop_own_host(host)
            except Exception as e:
                cleanup_errors.append(str(e))
        for log in logs:
            log.close()
        if android and client:
            try:
                client.close()
            except Exception as error:
                cleanup_errors.append(str(error))
        if args.operator_build and (out / "operator" / "capture.json").exists():
            try:
                stopped = json.loads(operator("Stop", ["-Metadata", str(out / "operator" / "capture.json")]))
                assert stopped["state"] == "StoppedVerified" and stopped["hooksRestored"]
                result["operatorStopVerified"] = True
            except Exception as error:
                cleanup_errors.append(str(error))
            if capture_path.exists():
                shutil.copy2(capture_path, out / "capture.log")
        result.update(hostExitCode=host.returncode if host else None,
            publisherExitCode=publisher.returncode if publisher else None, clientExitCode=client.returncode if client else None,
            sourceExitCode=source.returncode if source else None, cleanupErrors=cleanup_errors,
            hostSha256=sha(portable / "sunshine.exe"), clientSha256=sha(client_binary))
        if cleanup_errors or (host and host.returncode != 0):
            result["passed"] = False
        save(out / "result.json", result)
        if isinstance(publisher, OwnedPublisher):
            publisher.close()
        if not result["passed"]:
            print("FAIL_FULL_RTSP inspect result.json and original logs", flush=True)
            if sys.exc_info()[0] is None:
                raise RuntimeError("Acceptance failed during cleanup; inspect result.json")


if __name__ == "__main__":
    main()
