"""Production RTSP client against controlled capabilities; stops before media startup."""
import argparse, json, os, socket, subprocess, threading
from pathlib import Path
from negotiation_test import SDP, ACK, CARRIER, SHA

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--client",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    cases=[
        ("auto-mat",6,SDP,ACK,5),
        ("auto-legacy-host",6,"v=0\r\n",[],0),
        ("auto-dump-only",6,SDP.replace("mat-playback","mat-dump"),[],0),
        ("auto-version",6,SDP.replace("mat-playback:1","mat-playback:2"),[],0),
        ("auto-carrier",6,SDP.replace(CARRIER,"pcm"),[],0),
        ("auto-clock",6,SDP.replace("mat-clock:48000","mat-clock:192000"),[],0),
        ("auto-hash",6,SDP.replace(SHA,"A"*64),[],0),
        ("auto-count",6,SDP.replace("mat-cycles:178","mat-cycles:1001"),[],0),
        ("auto-duplicate",6,SDP+"a=x-ss-audio.mat-playback:1\r\n",[],0),
        ("auto-mat-bad-ack",6,SDP,ACK[:-1],-1),
        ("auto-stale-mat-ack",6,"v=0\r\n",ACK,-1),
        ("opus-ignores-host-mat",0,SDP,[],0),
        ("strict-missing",5,"v=0\r\n",[],-1),
        ("strict-mat",5,SDP,ACK,5),
        ("auto-gfe",6,"v=0\r\n",[],0),
    ]
    results=[];env=dict(os.environ);env["PATH"]="C:/msys64/ucrt64/bin;"+env.get("PATH","")
    for name,requested,sdp,ack,expected in cases:
        stop=threading.Event();methods=[];announces=[];errors=[]
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
                        payload=sdp.encode() if method=="DESCRIBE" else b""
                        extra=[("Session","MATCAP"),("Transport",f"unicast;server_port={port}")]
                        if method=="ANNOUNCE":extra+=ack
                        response=f"RTSP/1.0 200 OK\r\nCSeq: {fields['cseq']}\r\n"+"".join(f"{k}: {v}\r\n" for k,v in extra)
                        conn.sendall(response.encode()+f"Content-Length: {len(payload)}\r\n\r\n".encode()+payload)
            except Exception as error:
                if not stop.is_set():errors.append(repr(error))
        worker=threading.Thread(target=serve);worker.start()
        try:
            run=subprocess.run([str(args.client.resolve()),f"rtsp://127.0.0.1:{port}",str(requested),"gfe" if name.endswith("gfe") else "sunshine"],
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
            else: assert "x-ml-audio.codec:mat-iec61937-v1" in announces[0]
        results.append(dict(case=name,requested=requested,selected=expected,methods=methods,mediaTested=False))
        print("PASS",name,flush=True)
    (args.output/"proof.json").write_text(json.dumps(dict(result="PASS",scope="Actual production RTSP negotiation; controlled peer; no media/device playback",results=results),indent=2))

if __name__=="__main__":main()
