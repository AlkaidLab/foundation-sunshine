"""Bounded native MAT inputs. Generated output only; no playback or endpoint changes."""
import argparse, hashlib, json, struct
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parent
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    fixture=ROOT.parent/'joc-spike/game02/input.npz'
    with np.load(fixture,allow_pickle=False) as source:
        audio=source['audio'].copy();xyz=source['windows_xyz'].copy()
    assert audio.shape==(2,144000) and xyz.shape==(2,300,3)
    energy=np.square(audio.reshape(2,300,480),dtype=np.float64).sum(axis=2)
    score=np.array([min(energy[:,i:i+100].sum(axis=1)) for i in range(201)])
    first=int(np.argmax(score));assert score[first]>0
    game_audio=audio[:,first*480:(first+100)*480];game_xyz=xyz[:,first:first+100]
    samples=np.arange(48000)/48000;times=np.arange(100)*.01
    tone_audio=np.array([.02*np.sin(2*np.pi*f*samples) for f in (523.25,783.99)],dtype=np.float32)
    tone_xyz=np.array([np.stack([np.sin(times*1.1+i),.5*np.sin(times*.7+i),-np.cos(times*1.1+i)],axis=1) for i in range(2)])
    cases=[(1,(901,902),tone_audio,tone_xyz),(2,(2204929444,1915588004),game_audio,game_xyz)]
    path=args.output/'input.bin'
    with path.open('xb') as out:
        out.write(struct.pack('<8sIIII',b'MATINP1\0',1,2,100,480))
        for kind,ids,pcm,pos in cases:
            assert np.isfinite(pcm).all() and np.abs(pcm).max()<=1 and np.isfinite(pos).all() and np.abs(pos).max()<10000
            out.write(struct.pack('<II',kind,100))
            for block in range(100):
                for obj in range(2):
                    out.write(struct.pack('<I3f',ids[obj],*pos[obj,block]));out.write(pcm[obj,block*480:(block+1)*480].astype('<f4').tobytes())
            np.savez(args.output/('tone-expected.npz' if kind==1 else 'game-expected.npz'),audio=pcm.astype('<f4'),windows_xyz=pos.astype('<f4'),object_ids=np.array(ids,dtype=np.uint32))
    assert path.stat().st_size==774440
    summary=dict(input_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),bytes=path.stat().st_size,case_blocks=100,frames_per_block=480,
        source_fixture=str(fixture),source_fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),game_fixture_first_block=first,
        coordinate_mapping='Original captured listener-relative Windows XYZ; no R32 normalization or axis swap',
        game_gain='Uses previously applied source volume*callGain; replay volume=1, gain=1',full_game_mix=False,static_bed_included=False)
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf8');print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
