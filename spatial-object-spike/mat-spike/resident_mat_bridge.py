"""Offline bridge core: captured v2 records -> full 480-frame MAT object snapshots.

No process access, vendor calls, playback or endpoints. Capture QPC is arrival time,
NOT a sample presentation clock. This source adapter requires verified lockstep rounds.
"""
from collections import Counter
from dataclasses import dataclass
import hashlib,json,math,struct
import numpy as np

HEADER=struct.Struct('<IIQqIIfffffIQIIIIQQfffI')
FILE=struct.Struct('<8sIIIIQ')
FRAME=struct.Struct('<IIIIqq')
CALL=struct.Struct('<IIII3fI')
assert (HEADER.size,FILE.size,FRAME.size,CALL.size)==(112,32,32,32)
RATE=48000
QUANTUM=480


@dataclass
class Record:
    h:tuple
    pcm:np.ndarray
    chunk:str='fixture'
    @property
    def key(self):return self.h[2],self.h[4],self.h[5]


def source_info(root):
    summary=json.loads((root/'summary.json').read_text())
    pos=json.loads((root/'position-summary.json').read_text())
    backend=json.loads((root/'backend.json').read_text())
    if not (summary['status']==2 and summary['error']==0 and summary['original_bytes_restored']
            and all(summary[k]==0 for k in ('dropped_full','dropped_busy','invalid'))
            and pos['stateful'] and pos['evictions']==0 and backend['extended_calls']==0):
        raise ValueError('capture integrity is not clean')
    paths=[root/'global-objects.bin',*sorted(root.glob('global-objects-[0-9]*.bin'))]
    hashes=[]
    for path in paths:
        hashes.append(dict(file=path.name,bytes=path.stat().st_size,sha256=file_sha256(path)))
    return summary,pos,hashes


def file_sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda:stream.read(1024*1024),b''):digest.update(data)
    return digest.hexdigest()


def records(root):
    sequence=0;last_qpc=None
    paths=[root/'global-objects.bin',*sorted(root.glob('global-objects-[0-9]*.bin'))]
    for path in paths:
        with path.open('rb') as stream:
            while data:=stream.read(HEADER.size):
                if len(data)!=HEADER.size:raise ValueError('truncated header')
                h=HEADER.unpack(data)
                if h[0]!=0x324f424a or h[13]!=2 or not 0<h[1]<=2048 or h[12]!=sequence or h[22]!=0:
                    raise ValueError('invalid v2 header/sequence')
                if last_qpc is not None and h[3]<last_qpc:raise ValueError('arrival QPC went backwards')
                raw=stream.read(h[1]*4)
                if len(raw)!=h[1]*4:raise ValueError('truncated PCM')
                pcm=np.frombuffer(raw,dtype='<f4')
                if not np.isfinite(pcm).all() or not math.isfinite(h[10]):raise ValueError('nonfinite PCM/gain')
                yield Record(h,pcm,path.name)
                sequence+=1;last_qpc=h[3]


def lockstep_batches(root,width):
    """Fail closed, not a general live quantum detector. Never bucket by rounded QPC."""
    template=None;batch=[]
    for record in records(root):
        batch.append(record)
        if len(batch)==width:
            keys=[r.key for r in batch]
            if template is None:
                if len(set(keys))!=width:raise ValueError('first round has duplicate objects')
                template=keys
            if len(set(keys))!=width or set(keys)!=set(template) or any(r.h[1]!=QUANTUM for r in batch):
                raise ValueError('source is not the verified complete-key-set/480-frame lockstep fixture')
            yield batch;batch=[]
    if batch:raise ValueError('partial final source round')


@dataclass(frozen=True)
class Snapshot:
    generation:int
    xyz:tuple|None
    volume:float|None
    gain:float
    flags:int
    state:int


class StateCache:
    def __init__(self):self.entries={};self.epoch=0;self.counts=Counter()
    def apply(self,record):
        h=record.h;flags=h[14]
        if flags&~15:raise ValueError('unsupported source property flags')
        if h[17]!=self.epoch:
            self.entries.clear();self.epoch=h[17];self.counts['loss_invalidations']+=1
        key=record.key;old=self.entries.get(key)
        if old is None or flags&1:
            if old is not None and old['g']==h[18]:raise ValueError('create did not change source generation')
            entry=dict(g=h[18],p=None,v=1. if flags&1 else None)
        else:
            entry=old
            if entry['g']!=h[18]:raise ValueError('generation changed without create/end/loss boundary')
        if flags&2 and math.isfinite(h[9]):entry['v']=h[9]
        if h[5]!=1:
            state=4;xyz=None
        elif flags&4:
            entry['p']=tuple(h[19:22]) if all(map(math.isfinite,h[19:22])) else None
            xyz=entry['p'];state=1 if xyz is not None else 0
        elif flags&1:entry['p']=(0.,0.,0.);xyz=entry['p'];state=3
        else:xyz=entry['p'];state=2 if xyz is not None else 0
        stored=tuple(h[6:9])
        if state!=h[15] or (xyz is None and not all(map(math.isnan,stored))) or (xyz is not None and xyz!=stored):
            raise ValueError('replayed position cache differs from recorded snapshot')
        volume=entry['v']
        if bool(h[16])!=(volume is not None) or (volume is None and not math.isnan(h[9])) or (volume is not None and h[9]!=volume):
            raise ValueError('replayed volume cache differs from recorded snapshot')
        self.counts[f'position_state_{state}']+=1
        self.counts['creates']+=bool(flags&1);self.counts['ends']+=bool(flags&8)
        if flags&8:self.entries.pop(key,None)
        else:self.entries[key]=entry
        return Snapshot(h[18],xyz,volume,h[10],flags,state)


@dataclass
class FramedObject:
    pcm:np.ndarray
    spans:list
    @property
    def single_position_ready(self):
        positions={span['xyz'] for span in self.spans}
        return len(positions)==1 and None not in positions


class ObjectFramer:
    """Preserve piecewise metadata; never flatten a coordinate change inside 480 frames."""
    def __init__(self):self.parts=[];self.spans=[];self.buffered=0;self.max_buffered=0
    def push(self,pcm,xyz):
        result=[];offset=0
        while offset<len(pcm):
            take=min(QUANTUM-self.buffered,len(pcm)-offset)
            self.parts.append(np.array(pcm[offset:offset+take],dtype='<f4',copy=True))
            self.spans.append(dict(offset=self.buffered,frames=take,xyz=xyz))
            self.buffered+=take;offset+=take;self.max_buffered=max(self.max_buffered,self.buffered)
            if self.buffered==QUANTUM:
                result.append(FramedObject(np.concatenate(self.parts),self.spans))
                self.parts=[];self.spans=[];self.buffered=0
        return result
    def finish(self):
        if self.buffered:raise ValueError('partial final object quantum; explicit pad policy required')


def scaled_pcm(pcm,snapshot):
    if snapshot.volume is None:raise ValueError('unknown volume cannot be guessed')
    # Defined F32 two-step arithmetic; output uses volume=1, callGain=1 downstream.
    return np.multiply(np.multiply(pcm,np.float32(snapshot.volume),dtype=np.float32),np.float32(snapshot.gain),dtype=np.float32)


def call_bytes(identity,source_slot,typ,flags,xyz,pcm):
    if len(pcm)!=QUANTUM or not np.isfinite(pcm).all():raise ValueError('invalid bridge PCM')
    xyz=(math.nan,)*3 if xyz is None else xyz
    return CALL.pack(identity,source_slot,typ,flags,*xyz,QUANTUM)+pcm.astype('<f4',copy=False).tobytes()


def frame_bytes(index,calls,first_qpc,last_qpc,eof=False):
    return FRAME.pack(index,QUANTUM,len(calls),int(eof),first_qpc,last_qpc)+b''.join(calls)


class Bridge:
    def __init__(self,session):
        self.session=session;self.cache=StateCache();self.identities={};self.active={};self.next_id=1;self.source_slots={}
        self.framers={};self.pending_ends=[];self.manifest=[];self.counts=Counter();self.last_frame=-1;self.closed=False
    def consume(self,index,batch):
        if self.closed:raise ValueError('bridge already finalized')
        if index!=self.last_frame+1:raise ValueError('bridge quantum gap')
        self.last_frame=index;objects=[];beds=[]
        for native_id,slot,xyz,reason in self.pending_ends:
            objects.append(call_bytes(native_id,slot,1,8,xyz,np.zeros(QUANTUM,dtype='<f4')))
            self.counts[f'bridge_end_{reason}']+=1
        self.pending_ends=[]
        for record in batch:
            slot=self.source_slots.setdefault(record.key,len(self.source_slots))
            h=record.h
            if h[17]!=self.cache.epoch:
                for native_id,old_slot,xyz in self.active.values():
                    objects.append(call_bytes(native_id,old_slot,1,8,xyz,np.zeros(QUANTUM,dtype='<f4')))
                    self.counts['bridge_end_loss_invalidation']+=1
                self.active.clear();self.identities.clear()
                for framer in self.framers.values():framer.finish()
                self.framers.clear()
            snapshot=self.cache.apply(record)
            self.counts['source_records']+=1;self.counts['source_object_samples']+=len(record.pcm)
            # Session + host + ID + type + loss epoch + generation: no cross-stream ID collisions.
            key=(self.session,*record.key,h[17],snapshot.generation)
            if h[5]!=1:
                if snapshot.volume is None:raise ValueError('static volume unknown; retain in original capture only')
                beds.append(call_bytes(0,slot,h[5],h[14],None,scaled_pcm(record.pcm,snapshot)))
                self.counts['preserved_static_samples']+=len(record.pcm);continue
            if snapshot.xyz is None or snapshot.volume is None:
                if np.count_nonzero(record.pcm):self.counts['quarantined_unknown_audible_records']+=1
                else:self.counts['deferred_unknown_silent_records']+=1
                self.counts['quarantined_or_deferred_samples']+=len(record.pcm)
                if key in self.active:
                    native_id,old_slot,xyz=self.active.pop(key)
                    objects.append(call_bytes(native_id,old_slot,1,8,xyz,np.zeros(QUANTUM,dtype='<f4')))
                    self.counts['bridge_end_unknown']+=1
                    self.identities.pop(key,None)
                    self.framers.pop(key).finish()
                continue
            if key not in self.identities:
                # End an old generation before rebinding a source key. Native IDs themselves are not reused.
                for old_key in list(self.active):
                    if old_key[:4]==key[:4] and old_key!=key:
                        native_id,old_slot,xyz=self.active.pop(old_key)
                        objects.append(call_bytes(native_id,old_slot,1,8,xyz,np.zeros(QUANTUM,dtype='<f4')))
                        self.counts['bridge_end_generation_change']+=1
                        self.identities.pop(old_key,None);self.framers.pop(old_key).finish()
                native_id=self.next_id;self.next_id+=1;self.identities[key]=native_id
                self.manifest.append(dict(native_id=native_id,source_slot=slot,session=self.session,host=h[2],
                    original_id=h[4],type=h[5],loss_epoch=h[17],generation=h[18],first_ready_frame=index,
                    source_create_observed=bool(h[14]&1),creation_reason='first known snapshot; may be late attach'))
                self.counts['bridge_creates']+=1;flags=7
            else:native_id=self.identities[key];flags=6
            pcm=scaled_pcm(record.pcm,snapshot)
            if np.abs(pcm).max()>1:self.counts['native_pcm_range_exceedances']+=1
            framer=self.framers.setdefault(key,ObjectFramer());assembled=framer.push(pcm,snapshot.xyz)
            if len(assembled)!=1 or framer.buffered or not assembled[0].single_position_ready:
                raise ValueError('this source adapter needs one aligned 480-frame single-position object per round')
            objects.append(call_bytes(native_id,slot,1,flags,snapshot.xyz,assembled[0].pcm))
            self.active[key]=(native_id,slot,snapshot.xyz)
            if len(self.active)>20:raise ValueError('more than 20 dynamic objects; no silent eviction')
            self.counts['emitted_dynamic_samples']+=len(pcm);self.counts['dynamic_calls']+=1
            if h[14]&8:
                self.active.pop(key);self.pending_ends.append((native_id,slot,snapshot.xyz,'source_end'))
                self.identities.pop(key,None)
                self.framers.pop(key).finish()
        self.counts['max_active_objects']=max(self.counts['max_active_objects'],len(self.active))
        return (frame_bytes(index,objects,batch[0].h[3],batch[-1].h[3]),
                frame_bytes(index,beds,batch[0].h[3],batch[-1].h[3]))
    def finish(self):
        if self.closed:raise ValueError('bridge already finalized')
        self.closed=True
        for framer in self.framers.values():framer.finish()
        calls=[]
        for native_id,slot,xyz,reason in self.pending_ends:
            calls.append(call_bytes(native_id,slot,1,8,xyz,np.zeros(QUANTUM,dtype='<f4')))
            self.counts[f'bridge_end_{reason}']+=1
        self.pending_ends=[]
        for native_id,slot,xyz in self.active.values():
            calls.append(call_bytes(native_id,slot,1,8,xyz,np.zeros(QUANTUM,dtype='<f4')))
            self.counts['bridge_end_capture_eof']+=1
        self.active.clear()
        # -1 means synthetic EOF, not a fabricated source arrival or game end timestamp.
        return frame_bytes(self.last_frame+1,calls,-1,-1,eof=True)
