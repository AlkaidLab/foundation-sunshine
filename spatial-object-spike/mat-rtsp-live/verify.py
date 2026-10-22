"""Isolated full Sunshine/NVHTTP/RTSP/Moonlight session, no media decoder.

Preprovisions an ephemeral paired certificate ONLY in this test's state file.
Never changes the installed host, Windows trust, drivers, or audio endpoints.
Timeouts report PIDs; no kill/restart fallback. WM_ENDSESSION targets our own
hidden Sunshine window and runs its ordinary shutdown path.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import time
import urllib.parse
import urllib.request
import uuid
import winreg
import xml.etree.ElementTree as ET
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "spatial-object-spike/mat-spike/mat-native-session-run02/capture/encoder-payload.raw"
EXPECTED = "79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6"

def write_json(path, value):
    with path.open("x", encoding="utf8") as f:
        json.dump(value, f, indent=2)

def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()

def cert_pair(directory, name):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now-datetime.timedelta(days=1)).not_valid_after(now+datetime.timedelta(days=2))
            .sign(key, hashes.SHA256()))
    pem = cert.public_bytes(serialization.Encoding.PEM)
    (directory / (name+".pem")).write_bytes(pem)
    (directory / (name+".key")).write_bytes(key.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,serialization.NoEncryption()))
    return pem.decode("ascii")

def check_bridge_absent():
    # Production startup cleans stale bridge values; require that there are none.
    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            key=winreg.OpenKey(root,r"SOFTWARE\Khronos\Vulkan\ImplicitLayers",0,winreg.KEY_READ|winreg.KEY_WOW64_64KEY)
        except FileNotFoundError:
            continue
        with key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                name,_,_=winreg.EnumValue(key,i)
                if Path(name).name.lower()=="vklayer_zako_virtual_hdr.json":
                    raise RuntimeError("Existing Vulkan bridge registration: do not start isolated host")

def stop_own_host(process):
    if process.poll() is not None:
        return
    user=ctypes.WinDLL("user32",use_last_error=True)
    user.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
    user.GetClassNameW.argtypes=[ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_int]
    user.PostMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t]
    found=[]
    callback_type=ctypes.WINFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_ssize_t)
    @callback_type
    def visit(hwnd,param):
        pid=ctypes.c_ulong();user.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
        if pid.value==process.pid:
            name=ctypes.create_unicode_buffer(256);user.GetClassNameW(hwnd,name,256)
            if name.value=="SunshineSessionMonitorClass":
                found.append(hwnd)
        return 1
    user.EnumWindows(visit,0)
    if len(found)!=1:
        raise RuntimeError(f"Own host PID {process.pid} has no unique shutdown window; left running")
    if not user.PostMessageW(found[0],0x16,1,0): # WM_ENDSESSION to our own process only
        raise ctypes.WinError(ctypes.get_last_error())
    process.wait(timeout=25)

def main():
    parser=argparse.ArgumentParser();parser.add_argument("--output",required=True)
    parser.add_argument("--port",type=int,default=52010)
    parser.add_argument("--playback-client",type=Path,help="JSON command prefix for actual JNI/Java playback session")
    args=parser.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    assert digest(SOURCE)==EXPECTED and SOURCE.stat().st_size==10936320
    check_bridge_absent()
    # Reserve/check all relevant ports before launch; don't terminate any owner.
    for typ, offsets in ((socket.SOCK_STREAM,(-5,0,1,21,30)),(socket.SOCK_DGRAM,(9,10,11,12))):
        for offset in offsets:
            with socket.socket(socket.AF_INET,typ) as sock:
                sock.bind(("127.0.0.1",args.port+offset))
    cert_pair(out,"server");client_pem=cert_pair(out,"client")
    identity=str(uuid.uuid4())
    write_json(out/"state.json",{"root":{"uniqueid":str(uuid.uuid4()),"named_devices":[
        {"name":"MAT full RTSP local acceptance","uuid":identity,"cert":client_pem}]}})
    salt=uuid.uuid4().hex[:16];password=uuid.uuid4().hex
    write_json(out/"creds.json",{"username":"mat-local-test","salt":salt,
        "password":hashlib.sha256((password+salt).encode()).hexdigest()})
    write_json(out/"apps.json",{"env":{},"apps":[{"name":"MAT local desktop",
        "cmd":"","image-path":"desktop.png","exclude-global-prep-cmd":"true"}]})
    config={"port":args.port,"bind_address":"127.0.0.1","address_family":"ipv4",
        "mdns_broadcast":"false","upnp":"false","system_tray":"false","min_log_level":"info",
        "mouse":"false","keyboard":"false","controller":"false","virtual_mouse":"false",
        "native_pen_touch":"false","native_touchpad_optimization":"false","clipboard_sync":"false",
        "enable_dsu_server":"false","client_gamepad_override":"false","usb_forwarding_enabled":"false",
        "install_steam_audio_drivers":"false","stream_mic":"false","keep_sink_default":"false",
        "stream_audio":"true","mat_dump_replay":SOURCE.as_posix(),"vdd_headless_create":"false",
        "vdd_keep_enabled":"true","vdd_vulkan_hdr_bridge":"false","display_device_prep":"no_operation",
        "resolution_change":"no_operation","refresh_rate_change":"no_operation","hdr_prep":"no_operation",
        "client_fingerprint_remote_rules":"false","file_mapping_port":args.port+30,"file_mappings":"[]",
        "lan_encryption_mode":0,"wan_encryption_mode":0,"encoder":"nvenc","hevc_mode":1,"av1_mode":1,
        "file_state":(out/"state.json").as_posix(),"credentials_file":(out/"creds.json").as_posix(),
        "pkey":(out/"server.key").as_posix(),"cert":(out/"server.pem").as_posix(),
        "file_apps":(out/"apps.json").as_posix(),"log_path":(out/"host.log").as_posix()}
    (out/"sunshine.conf").write_text("\n".join(f"{k} = {v}" for k,v in config.items())+"\n",encoding="utf8")
    portable=out/"host";portable.mkdir()
    built=ROOT/"build/mat-rtsp-live"
    shutil.copy2(built/"sunshine.exe",portable/"sunshine.exe")
    shutil.copytree(built/"assets",portable/"assets")
    (portable/"config").mkdir()
    env=os.environ.copy()
    # Explicit process-local opt-out: nvprefs::load refuses absent ProgramData.
    # Do not read/recover/change the installed service's NVIDIA undo/profile.
    for key in list(env):
        if key.upper()=="PROGRAMDATA":del env[key]
    env["PATH"]="C:/msys64/ucrt64/bin;"+env.get("PATH","")
    context=ssl.create_default_context(cafile=str(out/"server.pem"));context.check_hostname=False
    context.load_cert_chain(str(out/"client.pem"),str(out/"client.key"))
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),urllib.request.HTTPSHandler(context=context))
    def request(route,query=None,timeout=25):
        params={"uniqueid":identity,"uuid":str(uuid.uuid4()),"clientname":"MAT local acceptance"}
        params.update(query or {})
        url=f"https://127.0.0.1:{args.port-5}/{route}?"+urllib.parse.urlencode(params)
        with opener.open(url,timeout=timeout) as response:body=response.read()
        tree=ET.fromstring(body)
        if tree.attrib.get("status_code")!="200":raise RuntimeError(body.decode())
        return body,tree
    console=(out/"host-console.log").open("wb")
    host=subprocess.Popen([str(portable/"sunshine.exe"),str(out/"sunshine.conf")],cwd=portable,env=env,
        creationflags=subprocess.CREATE_NO_WINDOW,stdout=console,stderr=subprocess.STDOUT)
    write_json(out/"process.json",{"pid":host.pid,"exe":str(portable/"sunshine.exe"),"port":args.port})
    print(f"HOST PID {host.pid}",flush=True)
    results=[]
    try:
        deadline=time.monotonic()+90
        while True:
            if host.poll() is not None:raise RuntimeError(f"Host exited {host.returncode}")
            try:
                info_bytes,info=request("serverinfo",timeout=2);break
            except (OSError,urllib.error.URLError):
                if time.monotonic()>deadline:raise
                time.sleep(.3)
        (out/"serverinfo.xml").write_bytes(info_bytes)
        apps_bytes,apps=request("applist");(out/"applist.xml").write_bytes(apps_bytes)
        appid=apps.findtext("App/ID")
        assert appid and info.findtext("PairStatus")=="1"
        print("PAIRED isolated certificate; production servers ready",flush=True)
        for mode in ("plain","aes","rtspenc-aes"):
            run=out/mode;run.mkdir();key=os.urandom(16).hex();iv=os.urandom(4)+bytes(12)
            audio_mode="plain" if mode=="plain" else "aes"
            rikeyid=int.from_bytes(iv[:4],"big")
            launch_bytes,launch=request("launch",{"appid":appid,"rikey":key,"rikeyid":rikeyid,
                "mode":"640x360x30","localAudioPlayMode":0,"surroundAudioInfo":196610,
                "sops":0,"useVdd":0,"customScreenMode":0,"touchKeyboard":0,"hdrMode":0,"corever":int(mode=="rtspenc-aes"),
                "remoteControllersBitmap":0,"gcmap":0,"gcpersist":0})
            (run/"launch.xml").write_bytes(launch_bytes)
            target=run/"received.raw"
            prefix=json.loads(args.playback_client.read_text(encoding="utf-8-sig")) if args.playback_client else [str(Path(__file__).parent/"client-build/mat-session-client.exe")]
            command=prefix+[launch.findtext("sessionUrl0"),
                info.findtext("appversion"),info.findtext("GfeVersion"),info.findtext("ServerCodecModeSupport"),
                str(target),key,iv.hex(),audio_mode]
            with (run/"client.log").open("wb") as log:
                client_started=time.monotonic()
                client=subprocess.Popen(command,cwd=run,env=env,creationflags=subprocess.CREATE_NO_WINDOW,
                    stdout=log,stderr=subprocess.STDOUT)
                print(f"{mode} CLIENT PID {client.pid}",flush=True)
                try:code=client.wait(timeout=50)
                except subprocess.TimeoutExpired:
                    write_json(run/"timeout.json",{"clientPid":client.pid,"hostPid":host.pid})
                    raise RuntimeError(f"Client {client.pid} timed out; no forced termination")
            log=(run/"client.log").read_text(encoding="utf8",errors="replace")
            assert code==0,(code,log[-1500:])
            assert f"CONNECTED codec={5 if args.playback_client else 4} encrypted={int(audio_mode=='aes')} profileConfirmed=1 sha={EXPECTED} cycles=178" in log
            assert launch.findtext("sessionUrl0").startswith("rtspenc://" if mode=="rtspenc-aes" else "rtsp://")
            assert "decodeCalls=0" in log
            if args.playback_client:
                status=json.loads(Path(str(target)+".playback.json").read_text())
                assert status=={"completed":True,"cycles":178,"bytes":10936320,"sha256":EXPECTED,"fakeHardware":True},status
                assert not target.exists() and not Path(str(target)+".partial").exists()
                assert "rendererDrained=1" in log and "inputSHAVerified=1" in log
                byte_count=status["bytes"]
            else:
                assert target.stat().st_size==SOURCE.stat().st_size and digest(target)==EXPECTED
                assert not Path(str(target)+".partial").exists()
                status=json.loads(Path(str(target)+".json").read_text())
                assert status=={"published":True,"completeEOS":True,"failed":False,"cycles":178,
                    "expectedCycles":178,"sha256":EXPECTED,"expectedSha256":EXPECTED},status
                byte_count=target.stat().st_size
            result={"mode":mode,"bytes":byte_count,"cycles":178,"sha256":EXPECTED,
                "sourceSha256":EXPECTED,"audioDecodeCalls":0,"fullLiStartConnection":True,"clientExitCode":code,
                "status":status,"clientWallSeconds":round(time.monotonic()-client_started,3),
                "encryptedRTSP":mode=="rtspenc-aes","playbackFakeHardware":bool(args.playback_client)}
            results.append(result);write_json(run/"proof.json",result)
            print(f"PASS {mode} {byte_count} bytes SHA256 {EXPECTED}",flush=True)
            request("cancel")
            # Actual cancellation is asynchronous; poll normal host state.
            deadline=time.monotonic()+15
            while True:
                _,state=request("serverinfo")
                if state.findtext("currentgame")=="0":break
                if time.monotonic()>deadline:raise RuntimeError("Own session failed to cancel")
                time.sleep(.1)
            time.sleep(1)
        write_json(out/"proof.json",{"kind":"full-production-RTSP-loopback-MAT-playback-fake-hardware" if args.playback_client else "full-production-RTSP-loopback-no-decoding","source":str(SOURCE),
            "hostSha256":digest(portable/"sunshine.exe"),"clientSha256":digest(Path(command[0])),"results":results,
            "androidDeviceTested":False,"installedServiceUntouched":True,"ephemeralPairingPreprovisioned":True})
    finally:
        stop_own_host(host)
        console.close()
        print(f"Own host exited {host.returncode}",flush=True)

if __name__=="__main__":main()
