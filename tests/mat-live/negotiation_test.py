"""Production RTSP client against controlled capabilities; stops before media startup."""
import argparse, json, os, socket, subprocess, threading
from pathlib import Path
CARRIER="iec61937-192000-8-s16le"
SHA="0"*64
SDP=("v=0\r\n"+"a=x-ss-audio.mat-playback:1\r\n"+
     f"a=x-ss-audio.mat-carrier:{CARRIER}\r\na=x-ss-audio.mat-clock:48000\r\n"+
     f"a=x-ss-audio.mat-sha256:{SHA}\r\na=x-ss-audio.mat-cycles:1\r\n")
ACK=[("X-SS-Audio-Codec","mat-iec61937-v1"),("X-SS-Mat-SHA256",SHA),
     ("X-SS-Mat-Cycles","1"),("X-SS-Mat-Carrier",CARRIER),("X-SS-Mat-Clock","48000")]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--client",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    live=("v=0\r\n"+"".join(f"a=x-ss-audio.mat-live{suffix}:{value}\r\n" for suffix,value in
        [("","1"),("-carrier",CARRIER),("-clock","48000"),("-crypto","aes128-gcm-hkdf-sha256"),("-epoch","123"),("-domain","7")]))
    live_ack=[("X-SS-Audio-Codec","mat-live-v1"),("X-SS-Mat-Carrier",CARRIER),("X-SS-Mat-Clock","48000"),
        ("X-SS-Mat-Live-Crypto","aes128-gcm-hkdf-sha256"),("X-SS-Mat-Live-Epoch","123"),("X-SS-Mat-Live-Domain","7")]
    cases=[
        ("auto-live",6,live,live_ack,7),
        ("auto-finite-only",6,SDP,[],0),
        ("auto-legacy-host",6,"v=0\r\n",[],0),
        ("auto-live-version",6,live.replace("mat-live:1","mat-live:2"),[],0),
        ("auto-live-carrier",6,live.replace(CARRIER,"pcm"),[],0),
        ("auto-live-clock",6,live.replace("clock:48000","clock:192000"),[],0),
        ("auto-live-crypto",6,live.replace("aes128-gcm-hkdf-sha256","cbc"),[],0),
        ("auto-live-epoch",6,live.replace("epoch:123","epoch:0"),[],0),
        ("auto-live-overflow",6,live.replace("epoch:123","epoch:9223372036854775808"),[],0),
        ("auto-live-duplicate",6,live+"a=x-ss-audio.mat-live:1\r\n",[],0),
        ("auto-live-bad-ack",6,live,live_ack[:-1],-1),
        ("auto-live-finite-ack",6,live,live_ack+[("X-SS-Mat-Cycles","178")],-1),
        ("auto-live-wrong-epoch",6,live,live_ack[:-2]+[("X-SS-Mat-Live-Epoch","124"),live_ack[-1]],-1),
        ("auto-live-duplicate-ack",6,live,live_ack+[live_ack[-1]],-1),
        ("auto-stale-live-ack",6,"v=0\r\n",live_ack,-1),
        ("auto-stale-finite-ack",6,"v=0\r\n",ACK,-1),
        ("opus-ignores-live",0,live,[],0),
        ("strict-live-missing",7,SDP,[],-1),
        ("strict-live",7,live,live_ack,7),
        ("removed-finite",5,SDP,ACK,-1),
        ("removed-dump",4,SDP,ACK,-1),
        ("auto-gfe",6,"v=0\r\n",[],0),
    ]
    cases = [(*case, None) for case in cases] + [
        ("device-mat",6,live,live_ack,7,"mat"),
        *[(f"device-{device}",6,live,[],0,device) for device in ("gone","speaker","pcm","no-iec")],
        ("strict-device-pcm",7,live,[],-1,"pcm"),
    ]
    results=[];env=dict(os.environ);env["PATH"]="C:/msys64/ucrt64/bin;"+env.get("PATH","")
    for name,requested,sdp,ack,expected,device in cases:
        stop=threading.Event();methods=[];announces=[];errors=[];describe_live=[]
        server=socket.socket();server.bind(("127.0.0.1",0));server.listen();server.settimeout(.1)
        port=server.getsockname()[1]
        def serve():
            try:
                while not stop.is_set():
                    try: conn,_=server.accept()
                    except socket.timeout: continue
                    with conn:
                        conn.settimeout(3);data=b""
                        while b"\r\n\r\n" not in data:
                            part=conn.recv(65536)
                            if not part:raise RuntimeError("truncated RTSP header")
                            data+=part
                        header,body=data.split(b"\r\n\r\n",1)
                        lines=header.decode().split("\r\n");method=lines[0].split()[0];methods.append(method)
                        fields={k.lower():v.strip() for k,v in (line.split(":",1) for line in lines[1:] if ":" in line)}
                        while len(body)<int(fields.get("content-length","0")):
                            part=conn.recv(65536)
                            if not part:raise RuntimeError("truncated RTSP payload")
                            body+=part
                        if method=="ANNOUNCE":announces.append(body.decode())
                        if method=="DESCRIBE":describe_live.append(fields.get("x-ml-mat-live"))
                        payload=sdp.encode() if method=="DESCRIBE" else b""
                        extra=[("Session","MATCAP"),("Transport",f"unicast;server_port={port}")]
                        if method=="ANNOUNCE":extra+=ack
                        response=f"RTSP/1.0 200 OK\r\nCSeq: {fields['cseq']}\r\n"+"".join(f"{k}: {v}\r\n" for k,v in extra)
                        conn.sendall(response.encode()+f"Content-Length: {len(payload)}\r\n\r\n".encode()+payload)
            except Exception as error:
                if not stop.is_set():errors.append(repr(error))
        worker=threading.Thread(target=serve);worker.start()
        try:
            run=subprocess.run([str(args.client.resolve()),f"rtsp://127.0.0.1:{port}",str(requested),"gfe" if name.endswith("gfe") else "sunshine"] + ([device] if device else []),
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,timeout=15,creationflags=subprocess.CREATE_NO_WINDOW)
        finally:stop.set();worker.join(4);server.close()
        log=run.stdout.decode("utf8",errors="replace").replace("\r\n","\n");(args.output/f"{name}.log").write_text(log)
        assert not errors and not worker.is_alive(),(name,errors)
        if expected<0:
            assert run.returncode==3 and "RTSP_SELECTED" not in log and "PLAY" not in methods,(name,log[-2000:])
        else:
            assert run.returncode==0 and f"RTSP_SELECTED codec={expected}\n" in log and "PLAY" in methods,(name,methods,log[-2400:])
            assert len(announces)==1
            if expected==0:
                assert "x-ml-audio.mat-" not in announces[0] and "mat-iec61937" not in announces[0],(name,announces)
                assert "x-ml-audio.codec:6" not in announces[0]
            else:
                assert f"x-ml-audio.codec:{'mat-live-v1' if expected==7 else 'mat-iec61937-v1'}" in announces[0]
                if expected==7:assert "mat-sha256" not in announces[0] and "mat-cycles" not in announces[0]
        effective = requested if not device or device == "mat" else -1 if requested == 7 else 0
        assert describe_live==(["1"] if effective in (6,7) and not name.endswith("gfe") else [] if effective in (4,5,-1) else [None]),(name,describe_live)
        results.append(dict(case=name,requested=requested,selected=expected,methods=methods,mediaTested=False))
        print("PASS",name,flush=True)
    (args.output/"proof.json").write_text(json.dumps(dict(result="PASS",scope="Actual production RTSP negotiation; controlled peer; no media/device playback",results=results),indent=2))

if __name__=="__main__":main()
