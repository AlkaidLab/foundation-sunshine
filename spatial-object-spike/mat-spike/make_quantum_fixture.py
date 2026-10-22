"""TEST DATA ONLY: add synthetic Process markers to existing v2 Nioh PCM.

This intentionally uses the old 37-key fixture grouping. It does NOT recover
missing native boundaries, thread identity, timing, sample rate or object offsets.
The v3 parser itself never consults that grouping rule.
"""
import argparse,hashlib,json,math,struct
from pathlib import Path
from resident_mat_bridge import lockstep_batches,source_info

EVENT=struct.Struct('<IIQqIIfffffIQIIQQIiIIQ')
STATE=struct.Struct('<QIIIfffffIIQQI')
assert EVENT.size==112 and STATE.size==68


def canonical(value):
    return float('nan') if math.isnan(value) else value


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args()
    original,_,manifest=source_info(args.source)
    args.output.mkdir(parents=True,exist_ok=False)
    sequence=0;sample_count=0;object_count=0;markers=0;rounds=0
    total_bytes=0;chunk_bytes=0;chunks=0;stream=None
    pcm_hash=hashlib.sha256();state_hash=hashlib.sha256()
    encoder=0x123400  # Explicit fake address, not a captured encoder identity.

    def write(header,pcm=b''):
        nonlocal sequence,total_bytes,chunk_bytes,chunks,stream
        if stream is None or chunk_bytes>=2*1024*1024:
            if stream is not None:stream.close()
            name='global-objects.bin' if chunks==0 else f'global-objects-{chunks:04d}.bin'
            stream=(args.output/name).open('xb');chunks+=1;chunk_bytes=0
        data=EVENT.pack(*header)+pcm
        stream.write(data);total_bytes+=len(data);chunk_bytes+=len(data);sequence+=1

    def marker(qpc,frames):
        nonlocal markers
        raw=[0x474f424a,0,0,qpc,0,0,0.,0.,0.,0.,0.,123,sequence]
        write([*raw,0,2,0,encoder,frames,0,0,30720,0]);markers+=1

    try:
        for batch in lockstep_batches(args.source,37):
            if rounds==0:marker(batch[0].h[3],0) # Empty synthetic anchor, not recorded.
            for record in batch:
                h=record.h
                raw=list(h[:13]);raw[0]=0x474f424a;raw[6:9]=h[19:22]
                raw[11]=123;raw[12]=sequence # Synthetic single-producer thread/sequence.
                pcm=record.pcm.tobytes()
                write([*raw,h[14],1,h[17],encoder,0,0,0,0,0],pcm)
                pcm_hash.update(pcm);object_count+=1;sample_count+=h[1]
                state_hash.update(STATE.pack(h[2],h[4],h[5],h[1],
                    *map(canonical,h[6:11]),h[15],h[16],h[18],h[17],h[14]))
            marker(batch[-1].h[3],480);rounds+=1
    finally:
        if stream is not None:stream.close()
    if source_info(args.source)[2]!=manifest:raise ValueError('original source changed')
    if (object_count,sample_count)!=(original['blocks'],original['frames']):raise ValueError('original counters')
    def save(name,data):(args.output/name).write_text(json.dumps(data,indent=2),encoding='utf8')
    save('format.json',dict(version=3,header_bytes=112,source_kind='synthetic-process-markers-over-recorded-pcm',
        native_boundary_recovered=False,sample_rate_verified=False,
        caveat='Synthetic encoder/thread/sequence/Process markers over unchanged real Nioh PCM; 37-key rule used only by test-data generator.'))
    save('summary.json',dict(status=2,error=0,original_bytes_restored=True,dropped_full=0,dropped_busy=0,invalid=0,
        blocks=sequence,frames=sample_count,bytes=total_bytes,source_kind='synthetic fixture; not a native capture'))
    save('fixture-expected.json',dict(source_files=manifest,source_unchanged=True,object_records=object_count,
        markers=markers,completed_quanta=rounds,sample_clock_frames=rounds*480,object_samples=sample_count,
        pcm_sha256=pcm_hash.hexdigest(),stored_snapshot_sha256=state_hash.hexdigest(),chunks=chunks,
        native_boundary_verified=False,scope='Independent digests use original stored snapshots, not the new C++ cache.'))
    print(json.dumps(dict(output=str(args.output),rounds=rounds,chunks=chunks,object_records=object_count,
        native_boundary_verified=False)))


if __name__=='__main__':main()
