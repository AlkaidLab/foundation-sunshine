"""Bounded offline inspection of the existing Windows encoder capture.

Observed PCM-slot / six-bit-field layout follows public research in
barralutz/spatial-audio-lab at 5c689815680f3ea5b09fb35ea4dbe63a3dc5bfb7,
src/mat_format.* and mat_pipeline.cpp. This is NOT a general Dolby MAT decoder,
conformance checker, licensed renderer, or proof of Android compatibility.
No vendor DLLs, driver, hooks, playback, security or device configuration.
"""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import wave

import numpy as np

ROOT = Path(__file__).resolve().parent
RECORD = struct.Struct('<IIQqQIIIIiIQ')
PERIOD = 61440
PREAMBLE = bytes.fromhex('72f81f4e')
FULL = bytes.fromhex('834143c2082f')
LFE = bytes.fromhex('834140f20b2f')
TABLE = bytes.fromhex('01d61f37cdf37cdf37cdf37cdf37cdf37cdf')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def find_all(data, pattern):
    offsets = []
    start = 0
    while True:
        pos = data.find(pattern, start)
        if pos < 0:
            return offsets
        offsets.append(pos)
        start = pos + 1


def unswap(data):
    if len(data) % 2:
        raise ValueError('odd transport-word payload length')
    return np.frombuffer(data, dtype='<u2').astype('>u2').tobytes()


def object_fields(data, table, index):
    bit = (table + len(TABLE)) * 8 + index * 36
    if bit + 36 > len(data) * 8:
        raise ValueError('truncated observed object table')
    value = int.from_bytes(data[bit//8:(bit+36+7)//8], 'big')
    shift = 8*((bit+36+7)//8-bit//8) - (bit % 8) - 36
    packed = (value >> shift) & ((1 << 36)-1)
    return tuple((packed >> ((5-i)*6)) & 63 for i in range(6))


def inspect_burst(carrier):
    if len(carrier) != PERIOD or carrier[:4] != PREAMBLE:
        raise ValueError('incomplete/misaligned fixed carrier burst')
    pc, pd = struct.unpack_from('<HH', carrier, 4)
    if pc & 31 != 0x16 or pc & 0x80 or not 2 <= pd <= PERIOD-8 or pd % 2:
        raise ValueError(f'invalid MAT-family IEC header: {pc=:x}, {pd=}')
    logical = unswap(carrier[8:8+pd])
    full = find_all(logical, FULL)
    lfe = find_all(logical, LFE)
    tables = find_all(logical, TABLE)
    valid = len(full) == 62 and len(lfe) == 2 and len(tables) == 2
    valid = valid and all(p+len(FULL)+960 <= len(logical) for p in full)
    valid = valid and all(p+len(LFE)+240 <= len(logical) for p in lfe)
    valid = valid and all(p+len(TABLE)+90 <= len(logical) for p in tables)
    result = dict(pc=pc,pd=pd,logical_head=logical[:16].hex(),full_markers=len(full),
                  lfe_markers=len(lfe),table_markers=len(tables),observed_layout_valid=bool(valid),
                  transport_padding_nonzero=sum(b != 0 for b in carrier[8+pd:]),
                  carrier_sha256=sha(carrier),logical_sha256=sha(logical))
    if not valid:
        return result,logical,None,None,None
    audio = np.stack([np.frombuffer(logical, dtype='>i2',count=480,offset=p+len(FULL))
                      for p in full]).reshape(2,31,480).astype('<i2')
    low = np.stack([np.frombuffer(logical,dtype='>i2',count=120,offset=p+len(LFE))
                    for p in lfe]).astype('<i2')
    fields = np.array([[object_fields(logical,t,obj) for obj in range(20)] for t in tables],dtype=np.uint8)
    return result,logical,audio,low,fields


def read_capture(directory):
    summary = json.loads((directory/'summary.json').read_text())
    if summary.get('capture_kind')=='independent_mat_encoder':
        # The standalone worker installs no hook; do not pretend that hook bytes were restored.
        if not (summary['status']==2 and summary['error']==0 and summary['thread_completed']
                and summary['initialized'] and summary['hooks_installed'] is False
                and summary['source_blocks']>0
                and summary['processed_source_blocks']==summary['source_blocks']
                and all(summary[key]>=0 for key in ['create_hr','mapper_hr','initialize_hr','submit_hr','process_hr'])):
            raise ValueError('independent encoder did not finish all input cleanly')
    elif summary['status']!=2 or summary['error']!=0 or not summary['original_bytes_restored']:
        raise ValueError('original capture did not stop cleanly')
    if any(summary[key] for key in ['dropped','invalid','failures']):
        raise ValueError('original capture has integrity errors')
    phase_path=directory/'processing.json'
    if summary.get('capture_kind')=='independent_mat_encoder' and phase_path.exists():
        phase=json.loads(phase_path.read_text())
        if not (phase['protocol_version']==2
                and phase['preroll_blocks']==phase['processed_preroll_blocks']
                and phase['tail_blocks']==phase['processed_tail_blocks']
                and phase['source_blocks']==phase['processed_source_blocks']==summary['source_blocks']
                and phase['preroll_frames']==480*phase['preroll_blocks']
                and summary['records']==phase['preroll_blocks']+phase['source_blocks']+phase['tail_blocks']):
            raise ValueError('independent encoder processing phases do not agree')
    data = bytearray(); records=[]
    with (directory/'encoder-output.bin').open('rb') as source:
        while raw := source.read(RECORD.size):
            if len(raw)!=RECORD.size:
                raise ValueError('truncated record header')
            r=RECORD.unpack(raw)
            if r[0]!=0x42595445 or r[2]!=len(records) or r[1]!=r[7] or not 0<=r[1]<=r[6]<=65536 or r[9]<0:
                raise ValueError('invalid record')
            body=source.read(r[1])
            if len(body)!=r[1]: raise ValueError('truncated record payload')
            records.append(r); data.extend(body)
    if len(records)!=summary['records'] or len(data)!=summary['bytes']:
        raise ValueError('capture counters do not agree')
    if any(records[i][3]>records[i+1][3] for i in range(len(records)-1)):
        raise ValueError('QPC went backwards')
    if bytes(data)!=(directory/'encoder-payload.raw').read_bytes():
        raise ValueError('old extraction differs from recorded bytes')
    return bytes(data),records,summary


def signal_metrics(audio, rate=48000):
    data=audio.astype(np.float64)/32768
    rows=[]
    for slot,channel in enumerate(data):
        peak=float(np.abs(channel).max())
        rms=float(np.sqrt(np.mean(channel*channel)))
        tone=[]
        for frequency in [523.25,783.99]:
            phase=2*np.pi*frequency*np.arange(channel.size)/rate
            a=np.column_stack((np.sin(phase),np.cos(phase)))
            coeff=np.linalg.lstsq(a,channel,rcond=None)[0]
            prediction=a@coeff
            residual=float(np.sum((channel-prediction)**2))
            power=float(prediction@prediction)
            tone.append(dict(hz=frequency,amplitude=float(np.linalg.norm(coeff)),
                             fitted_signal_to_residual_db=float(10*np.log10(max(power,1e-30)/max(residual,1e-30)))))
        rows.append(dict(slot=slot,role='static-slot' if slot<11 else 'dynamic-slot',
                         object_index=slot-11 if slot>=11 else None,peak=peak,rms=rms,
                         nonzero_samples=int(np.count_nonzero(channel)),source_tone_fits=tone))
    return rows


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture',type=Path,default=ROOT.parent/'resident/encoded01')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=False)
    carrier,records,old=read_capture(args.capture.resolve())
    if len(carrier)%PERIOD: raise ValueError('partial final burst')
    if find_all(carrier,PREAMBLE)!=list(range(0,len(carrier),PERIOD)):
        raise ValueError('preamble cadence mismatch or unexpected embedded preamble')
    (out/'carrier.iec61937').write_bytes(carrier)
    details=[];audios=[];lows=[];positions=[];indices=[];heads=Counter()
    with (out/'logical-payloads.length-prefixed').open('xb') as logical_file:
        for index in range(len(carrier)//PERIOD):
            block=carrier[index*PERIOD:(index+1)*PERIOD]
            result,logical,audio,low,fields=inspect_burst(block)
            result['burst']=index;details.append(result);heads[result['logical_head'][:8]]+=1
            logical_file.write(struct.pack('<I',len(logical)));logical_file.write(logical)
            if audio is not None:
                audios.extend(audio);lows.extend(low);positions.extend(fields)
                indices.extend([index*2,index*2+1])
    (out/'bursts.json').write_text(json.dumps(details,indent=2),encoding='utf8')
    if not audios: raise ValueError('no bursts match the observed PCM/object layout')
    # Keep only observed-valid windows; original indices record any gaps.
    pcm=np.concatenate(audios,axis=1)
    low=np.concatenate(lows)
    fields=np.array(positions)
    pcm.T.astype('<i2').tofile(out/'fullband-31ch-48k.pcm16')
    low.astype('<i2').tofile(out/'lfe-12k.pcm16')
    np.savez(out/'observed-slots.npz',audio_pcm16=pcm,fields=fields,half_indices=indices,sample_rate=48000)
    with (out/'object-fields.csv').open('x',newline='') as stream:
        writer=csv.writer(stream);writer.writerow(['half','time_seconds','object','f0','f1','f2','f3','f4','f5'])
        for k,tick in enumerate(indices):
            for obj in range(20):writer.writerow([tick,tick*.01,obj,*fields[k,obj].tolist()])
    metrics=signal_metrics(pcm)
    moving=[]
    for obj in range(20):
        values=fields[:,obj]
        moving.append(dict(object=obj,field_min=values.min(axis=0).tolist(),field_max=values.max(axis=0).tolist(),
            changes_per_field=np.count_nonzero(np.diff(values.astype(int),axis=0),axis=0).tolist()))
    active=sorted([row for row in metrics if row['rms']>1e-6],key=lambda row:row['rms'],reverse=True)
    for row in active[:4]:
        slot=row['slot']
        with wave.open(str(out/f'slot-{slot:02d}-48k.wav'),'wb') as wav:
            wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(48000);wav.writeframes(pcm[slot].astype('<i2').tobytes())
    invalid=[r['burst'] for r in details if not r['observed_layout_valid']]
    input_frames=sum(r[5] for r in records)
    encoded_seconds=input_frames/48000
    independent=old.get('capture_kind')=='independent_mat_encoder'
    source_frames=old['source_input_frames'] if independent else input_frames
    result=dict(input=str(args.capture.resolve()),capture_integrity_passed=True,
        original_sha256=sha(carrier),capture_records=len(records),carrier_bytes=len(carrier),bursts=len(details),
        carrier_period_bytes=PERIOD,source_input_frames=source_frames,source_input_seconds=source_frames/48000,
        processing_input_frames=input_frames,carrier_duration_seconds=encoded_seconds,
        carrier_mbps=len(carrier)*8/encoded_seconds/1e6,bursts_per_audio_second=len(details)/encoded_seconds,
        observed_valid_bursts=len(details)-len(invalid),observed_invalid_bursts=invalid,
        logical_head_counts=dict(heads),pd_counts=dict(Counter(r['pd'] for r in details)),
        output_pcm_frames=int(pcm.shape[1]),observed_active_slots=active,all_slot_metrics=metrics,
        object_field_activity=moving,lfe_peak=int(np.abs(low.astype(int)).max()),
        reference_commit='5c689815680f3ea5b09fb35ea4dbe63a3dc5bfb7',
        conclusions=dict(native_carrier_available=True,pcm_slots_observed=bool(active),raw_object_fields_observed=True,
                         exact_original_objects_verified=False,coordinates_calibrated=False,
                         dolby_mat_conformance_verified=False,android_local_decode_verified=False),
        caveats=['Observed-layout extractor only, not general or certified Dolby MAT decoder.',
                 'Slot identity is not an application object ID; six fields are raw research fields, not calibrated XYZ.',
                 ('New independently instantiated native encoder output; exact source is in the fixture/launch report.' if independent else
                  'Source was two controlled moving sine objects on 2026-10-18, not a game or new capture.'),
                 '48k/480-sample interpretation is observed source configuration; not a carrier PCM playback format.',
                 ('Installed Dolby components were called in audiodg; no virtual driver, copied vendor binaries or endpoint changes.' if independent else
                  'No third-party driver scripts or licensed codec binaries were executed.'),
                 'Raw extraction includes any bootstrap cycle matching the observed layout; stable-window/source comparison is a separate check.'])
    ffmpeg=ROOT.parent/'joc-spike/ffmpeg-tools/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe'
    check=subprocess.run([str(ffmpeg),'-hide_banner','-nostdin','-f','spdif','-i',str(out/'carrier.iec61937'),'-f','null','-'],
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
    (out/'ffmpeg-spdif.log').write_bytes(check.stderr+check.stdout)
    result['ffmpeg_spdif_returncode']=check.returncode
    (out/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in result.items() if k not in ['all_slot_metrics','object_field_activity']},indent=2))


if __name__=='__main__': main()
