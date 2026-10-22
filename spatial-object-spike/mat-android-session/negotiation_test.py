"""Negative-only RTSP peer; actual production client must reject before any carrier delivery.
This does NOT substitute for the separate full Sunshine positive acceptance.
"""
import argparse, json, os, socket, subprocess, threading
from pathlib import Path
SHA="79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6"
CARRIER="iec61937-192000-8-s16le"
ROOT=Path(__file__).resolve().parents[2]
SDP=("v=0\r\ns=MAT negative test\r\na=x-ss-audio.mat-playback:1\r\n"
     f"a=x-ss-audio.mat-carrier:{CARRIER}\r\na=x-ss-audio.mat-clock:48000\r\n"
     f"a=x-ss-audio.mat-sha256:{SHA}\r\na=x-ss-audio.mat-cycles:178\r\n")
ACK=[("X-SS-Audio-Codec","mat-iec61937-v1"),("X-SS-Mat-SHA256",SHA),
     ("X-SS-Mat-Cycles","178"),("X-SS-Mat-Carrier",CARRIER),("X-SS-Mat-Clock","48000")]
def main():
    p=argparse.ArgumentParser();p.add_argument("--client",type=Path,required=True);p.add_argument("--output",type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    command=json.loads(a.client.read_text(encoding="utf-8-sig"))
    # A controlled peer must not depend on an untracked previous run's XML.
    # These non-secret protocol version fields match the positive host profile.
    host_version, gfe_version, codec_modes = "7.1.431.-1", "3.23.0.74", "262145"
    env=dict(os.environ);env["PATH"]="C:/msys64/ucrt64/bin;"+env.get("PATH","")
    cases=[("missing-capability",SDP.replace("a=x-ss-audio.mat-playback:1\r\n",""),ACK),
           ("wrong-version",SDP.replace("mat-playback:1","mat-playback:2"),ACK),
           ("wrong-carrier",SDP.replace(CARRIER,"pcm"),ACK),
           ("wrong-clock",SDP.replace("mat-clock:48000","mat-clock:192000"),ACK),
           ("wrong-sha",SDP.replace(SHA,"A"*64),ACK),
           ("wrong-count",SDP.replace("mat-cycles:178","mat-cycles:1001"),ACK),
           ("duplicate-capability",SDP+"a=x-ss-audio.mat-playback:1\r\n",ACK)]
    for name,field,value in [("ack-codec","X-SS-Audio-Codec","mat-dump-v1"),
                             ("ack-sha","X-SS-Mat-SHA256","0"*64),
                             ("ack-count","X-SS-Mat-Cycles","177"),
                             ("ack-carrier","X-SS-Mat-Carrier","pcm"),
                             ("ack-clock","X-SS-Mat-Clock","192000")]:
        cases.append((name,SDP,[(k,value if k==field else v) for k,v in ACK]))
    cases.extend([("missing-ack",SDP,ACK[:-1]),("duplicate-ack",SDP,ACK+[ACK[-1]]),
                  ("casefold-duplicate-ack",SDP,ACK+[(ACK[-1][0].lower(),ACK[-1][1])])])
    results=[]
    for name,sdp,ack in cases:
        stop=threading.Event();errors=[];methods=[]
        server=socket.socket();server.bind(("127.0.0.1",0));server.listen();server.settimeout(.2)
        port=server.getsockname()[1]
        def serve():
            try:
                while not stop.is_set():
                    try:conn,_=server.accept()
                    except socket.timeout:continue
                    with conn:
                        conn.settimeout(5);data=b""
                        while b"\r\n\r\n" not in data:data+=conn.recv(65536)
                        header,body=data.split(b"\r\n\r\n",1)
                        lines=header.decode().split("\r\n");method=lines[0].split()[0];methods.append(method)
                        fields={k.lower():v for k,v in (line.split(":",1) for line in lines[1:] if ":" in line)}
                        length=int(fields.get("content-length","0"))
                        while len(body)<length:body+=conn.recv(65536)
                        payload=sdp.encode() if method=="DESCRIBE" else b""
                        extra=[("Session","MATTEST"),("Transport",f"unicast;server_port={port}")]
                        if method=="ANNOUNCE":extra+=ack
                        response=f"RTSP/1.0 200 OK\r\nCSeq:{fields['cseq']}\r\n"+"".join(f"{k}: {v}\r\n" for k,v in extra)
                        conn.sendall(response.encode()+f"Content-Length: {len(payload)}\r\n\r\n".encode()+payload)
            except Exception as e:
                if not stop.is_set():errors.append(repr(e))
        worker=threading.Thread(target=serve);worker.start()
        args=[f"rtsp://127.0.0.1:{port}",host_version,gfe_version,
              codec_modes,str(a.output/name),"00"*16,"00"*16,"plain","reject"]
        try:
            with (a.output/f"{name}.log").open("wb") as log:
                process=subprocess.Popen(command+args,stdout=log,stderr=subprocess.STDOUT,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
                try:code=process.wait(timeout=20)
                except subprocess.TimeoutExpired:raise RuntimeError(f"Negative client PID {process.pid} timed out; not killed")
        finally:
            stop.set();worker.join(timeout=6);server.close()
        log=(a.output/f"{name}.log").read_text(encoding="utf8",errors="replace")
        assert not errors and not worker.is_alive(),errors
        assert code==0 and "EXPECTED_NEGOTIATION_REJECTION" in log,(name,log[-1800:])
        expected="ANNOUNCE confirmation missing or mismatched" if "ack" in name else "Host lacks explicit finite MAT IEC playback profile"
        assert expected in log,(name,methods,log[-1800:])
        assert "PLAY" not in methods and "CONNECTED" not in log
        results.append({"case":name,"rejected":True,"methods":methods,"carrierBytesDelivered":0})
        print("PASS",name,flush=True)
    (a.output/"proof.json").write_text(json.dumps({"kind":"actual-client-negative-RTSP-peer","results":results},indent=2))
if __name__=="__main__":main()
