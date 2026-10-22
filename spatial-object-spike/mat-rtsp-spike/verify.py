"""No device, capture, elevation, deployment or kill-on-timeout. Native C audio
threads from the Android checkout, compiled for Windows. Not an RTSP live app.
"""
import argparse, hashlib, json, os, pathlib, subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
SHA='79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6'
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--build',type=pathlib.Path,required=True);p.add_argument('--sender',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True)
    a=p.parse_args();build=a.build.resolve();sender=a.sender.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env['PATH']='C:/msys64/ucrt64/bin;'+env.get('PATH','')
    source=ROOT/'spatial-object-spike/mat-spike/mat-native-session-run02/capture/encoder-payload.raw';assert sha(source)==SHA
    unit=subprocess.Popen([str(build/'mat-profile-unit.exe'),str(source),str(out/'unit')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
    try:u,e=unit.communicate(timeout=10)
    except subprocess.TimeoutExpired as exc:raise RuntimeError(f'Unit PID {unit.pid}; stopped without killing') from exc
    (out/'unit.log').write_text(u+e);assert unit.returncode==0,u+e
    proof=dict(scope='No-device: actual Android-checkout AudioStream and SDP generator on Windows; no live Sunshine RTSP handshake, APK or decoding',inputSha256=SHA,unit=u,cases=[])
    for mode in ('plain','aes'):
        case=out/mode;case.mkdir();dest=case/'received.raw'
        with (case/'receiver.stderr').open('w') as err,(case/'sender.log').open('w') as log:
            receiver=subprocess.Popen([str(build/'mat-audio-client.exe'),str(dest),SHA,'178',mode],stdout=subprocess.PIPE,stderr=err,text=True,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
            ready=receiver.stdout.readline().strip();assert ready.startswith('READY '),(receiver.pid,ready)
            send=subprocess.Popen([str(sender),'--mat-session-replay',str(source),ready.split()[1],mode],stdout=log,stderr=log,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
            try:send.communicate(timeout=15);r,_=receiver.communicate(timeout=15)
            except subprocess.TimeoutExpired as exc:raise RuntimeError(f'PIDs sender={send.pid}, receiver={receiver.pid}; no kill') from exc
        (case/'receiver.stdout').write_text(ready+'\n'+r)
        assert send.returncode==receiver.returncode==0,(send.returncode,receiver.returncode,r)
        assert 'decodeCalls=0 readyAfterReceiver=1' in r
        assert dest.stat().st_size==10936320 and sha(dest)==SHA
        status=json.loads(pathlib.Path(str(dest)+'.json').read_text());assert status['published'] and status['completeEOS'] and not status['failed'] and status['sha256']==SHA and status['cycles']==178,status
        announce=pathlib.Path(str(dest)+'.announce').read_text();assert 'x-ml-audio.codec:mat-dump-v1' in announce and 'x-nv-aqos.packetDuration:1' in announce and SHA in announce
        proof['cases'].append(dict(encryption=mode,passed=True,bytes=dest.stat().st_size,status=status,decodeCalls=0,readyAfterReceiver=True))
        print(f'PASS {mode}: actual AudioStream threads -> reassembly -> SHA256 {SHA}, no decode',flush=True)
    proof['passed']=True;(out/'proof.json').write_text(json.dumps(proof,indent=2))
    print('PASS unit + native threaded client plain/AES',flush=True)
if __name__=='__main__':main()
