"""New sender binary, previous unchanged Moonlight E-AC3 receiver/fixture.
No process termination on timeout, no native audio capture, loopback only.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess

ROOT=pathlib.Path(__file__).resolve().parents[2]
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--build',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);build=a.build.resolve()
    old=ROOT/'spatial-object-spike/rtp-spike';fixture=old/'game-pass02/wire-source.eac3';expected=old/'game-pass02/sent.eac3'
    expected_sha=sha(expected);assert expected_sha=='acf803c1733ad95e8a1b74c27f15aa00b8b6076afc65d2b79db39d4b898b149d'
    results=[]
    for enc,fault in [('plain','none'),('aes','none'),('aes','reorder'),('aes','drop2')]:
        case=out/f'{enc}-{fault}';case.mkdir();dest=case/'received.eac3';stats=case/'stats.json'
        with (case/'receiver.log').open('w') as err,(case/'sender.log').open('w') as log:
            receiver=subprocess.Popen([str(old/'build08/moonlight-audio-dump.exe'),str(dest),str(case/'frames.csv'),str(stats),'93',enc,fault],
                                      stdout=subprocess.PIPE,stderr=err,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
            ready=receiver.stdout.readline().strip();assert ready.startswith('READY '),(receiver.pid,ready)
            sender=subprocess.Popen([str(build/'sunshine-audio-replay.exe'),'--audio-rtp-replay',str(fixture),ready.split()[1],enc],
                                    stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
            try:sender.communicate(timeout=15);receiver.communicate(timeout=10)
            except subprocess.TimeoutExpired as e:raise RuntimeError(f'Stopped without killing sender {sender.pid}, receiver {receiver.pid}') from e
        result=json.loads(stats.read_text());assert sender.returncode==receiver.returncode==0,result
        assert sha(dest)==expected_sha and dest.stat().st_size==expected.stat().st_size
        results.append(dict(encryption=enc,fault=fault,sha256=sha(dest),stats=result))
        print(f'PASS legacy E-AC3 {enc}-{fault}',flush=True)
    (out/'proof.json').write_text(json.dumps(dict(passed=True,cases=results,senderSha256=sha(build/'sunshine-audio-replay.exe'),
        receiverSha256=sha(old/'build08/moonlight-audio-dump.exe')),indent=2))
if __name__=='__main__':main()
