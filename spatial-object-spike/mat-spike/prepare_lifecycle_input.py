"""Create deterministic bounded lifecycle and 20-object paced fixtures. No playback."""
import argparse,hashlib,json,struct
from pathlib import Path
import numpy as np

FRAMES=480
BLOCKS=240
OBJECT=struct.Struct('<II3f480f')
assert OBJECT.size==1940


def build_case(kind):
    # starts are creation times; audio may start later to allow silent initial preroll.
    segments=([(1001,0,20,220),(2001,50,50,90),(2001,120,120,160),(2002,170,170,210)]
              if kind==1 else [(3000+i,0,20,220) for i in range(20)])
    rng=np.random.default_rng(20261020+kind)
    audio=np.zeros((len(segments),BLOCKS*FRAMES),dtype='<f4')
    xyz=np.zeros((len(segments),BLOCKS,3),dtype='<f4')
    events=[[] for _ in range(BLOCKS)]
    descriptions=[]
    for gen,(identity,created,start,end) in enumerate(segments):
        audio[gen,start*FRAMES:end*FRAMES]=rng.uniform(-.015,.015,(end-start)*FRAMES).astype('<f4')
        t=np.arange(BLOCKS)*.01
        xyz[gen,:,0]=2*np.sin(t*2.1+gen*.73)
        xyz[gen,:,1]=.6*np.sin(t*1.7+gen*.47)
        xyz[gen,:,2]=-2*np.cos(t*2.1+gen*.73)
        for b in range(created,end+1):
            flags=8 if b==end else (7 if b==created else 6)
            events[b].append(OBJECT.pack(identity,flags,*xyz[gen,b],*audio[gen,b*FRAMES:(b+1)*FRAMES]))
        descriptions.append(dict(generation=gen,id=identity,created_block=created,source_start_block=start,
                                 end_block=end,frames=(end-start)*FRAMES))
    data=bytearray(struct.pack('<III',kind,BLOCKS,10))
    for rows in events:
        assert len(rows)<=20
        data.extend(struct.pack('<I',len(rows)))
        data.extend(b''.join(rows));data.extend(bytes((20-len(rows))*OBJECT.size))
    return data,audio,xyz,descriptions


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    data=bytearray(struct.pack('<8sIIII',b'MATLIF1\0',1,2,BLOCKS,FRAMES));cases=[]
    for kind,name in [(1,'lifecycle'),(2,'twenty')]:
        raw,audio,xyz,segments=build_case(kind);data.extend(raw)
        np.savez(args.output/f'{name}-expected.npz',audio=audio,windows_xyz=xyz,sample_rate=48000)
        info=dict(case=name,kind=kind,blocks=BLOCKS,pace_ms=10,segments=segments,
                  source='deterministic independent broadband PCM; not a new game capture')
        (args.output/f'{name}-manifest.json').write_text(json.dumps(info,indent=2),encoding='utf8');cases.append(info)
    (args.output/'input.bin').write_bytes(data)
    result=dict(input_sha256=hashlib.sha256(data).hexdigest(),input_bytes=len(data),cases=cases)
    (args.output/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps(dict(input_bytes=len(data),input_sha256=result['input_sha256']),indent=2))


if __name__=='__main__':main()
