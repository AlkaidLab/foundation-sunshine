"""MAT carrier -> experimental MTU-bounded RTP fragments -> localhost -> disk.
NOT the existing Sunshine audio protocol or an integrated Moonlight client.
No encryption or FEC. This checks byte-preserving fragmentation feasibility only.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import socket
import struct
import threading
import time
import zlib

from analyze_mat import PERIOD, PREAMBLE

RTP=struct.Struct('!BBHII')
FRAGMENT=struct.Struct('!4sIHHII')
MAX_DATAGRAM=1200
DATA_BYTES=MAX_DATAGRAM-RTP.size-FRAGMENT.size
PT=110  # PRIVATE RESEARCH CHOICE, not negotiated or allocated in Sunshine.
SSRC=0x4d415453


def packets(frame,index,start_sequence=0):
    if len(frame)!=PERIOD or frame[:4]!=PREAMBLE:raise ValueError('expected full carrier burst')
    count=math.ceil(len(frame)/DATA_BYTES)
    crc=zlib.crc32(frame)
    for part in range(count):
        header=RTP.pack(0x80,PT,(start_sequence+part)&65535,index*20,SSRC)
        descriptor=FRAGMENT.pack(b'MATS',index,part,count,len(frame),crc)
        yield header+descriptor+frame[part*DATA_BYTES:(part+1)*DATA_BYTES]


class Reassembler:
    def __init__(self):self.pending={};self.completed=set()
    def push(self,data):
        if not RTP.size+FRAGMENT.size<len(data)<=MAX_DATAGRAM:raise ValueError('datagram size')
        version,pt,seq,timestamp,ssrc=RTP.unpack_from(data)
        magic,index,part,count,total,crc=FRAGMENT.unpack_from(data,RTP.size)
        if version!=0x80 or pt!=PT or ssrc!=SSRC or magic!=b'MATS' or timestamp!=index*20:
            raise ValueError('invalid private RTP header')
        if index>=2000 or total!=PERIOD or count!=math.ceil(total/DATA_BYTES) or part>=count:
            raise ValueError('invalid fragment bounds')
        payload=data[RTP.size+FRAGMENT.size:]
        if len(payload)!=min(DATA_BYTES,total-part*DATA_BYTES):raise ValueError('invalid fragment length')
        if index in self.completed:return None
        if index not in self.pending:
            if len(self.pending)>=8:raise ValueError('bounded frame window exceeded')
            self.pending[index]=(crc,[None]*count)
        old,parts=self.pending[index]
        if crc!=old:raise ValueError('CRC header conflict')
        if parts[part] is not None and parts[part]!=payload:raise ValueError('duplicate fragment conflict')
        parts[part]=payload
        if any(p is None for p in parts):return None
        frame=b''.join(parts)
        if zlib.crc32(frame)!=crc:raise ValueError('reassembled CRC mismatch')
        del self.pending[index];self.completed.add(index)
        return index,frame


def run(source,output,reverse=False):
    data=source.read_bytes()
    if not data or len(data)%PERIOD or len(data)//PERIOD>2000:raise ValueError('bounded fixed-burst input required')
    output.mkdir(parents=True,exist_ok=False)
    frames=len(data)//PERIOD
    receiver=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    receiver.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
    receiver.bind(('127.0.0.1',0));receiver.settimeout(.2)
    sender=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sender.bind(('127.0.0.1',0))
    ready=threading.Event();done=threading.Event()
    received={};arrivals={};sent={};errors=[];wire_sizes=[];assembly=Reassembler()
    def receive():
        ready.set()
        try:
            while len(received)<frames and not done.is_set():
                try:packet,peer=receiver.recvfrom(2048)
                except socket.timeout:continue
                if peer!=sender.getsockname():raise ValueError('unexpected local sender')
                wire_sizes.append(len(packet))
                item=assembly.push(packet)
                if item is not None:
                    index,frame=item;received[index]=frame;arrivals[index]=time.perf_counter()
        except Exception as exc:errors.append(repr(exc))
    thread=threading.Thread(target=receive,daemon=True);thread.start();ready.wait(timeout=2)
    clock=time.perf_counter();sequence=0
    try:
        for index in range(frames):
            delay=clock+index*.020-time.perf_counter()
            if delay>0:time.sleep(delay)
            block=data[index*PERIOD:(index+1)*PERIOD]
            batch=list(packets(block,index,sequence));sequence+=len(batch)
            if reverse:batch.reverse()
            sent[index]=time.perf_counter()
            for packet in batch:sender.sendto(packet,receiver.getsockname())
        thread.join(timeout=3)
    finally:
        done.set();thread.join(timeout=1);sender.close();receiver.close()
    complete=len(received)==frames and not errors
    restored=b''.join(received[index] for index in sorted(received))
    (output/'received-carrier.iec61937').write_bytes(restored)
    latency=sorted((arrivals[k]-sent[k])*1000 for k in arrivals)
    def percentile(q):return latency[min(len(latency)-1,int((len(latency)-1)*q))] if latency else None
    result=dict(scope='Experimental private RTP fragmentation ONLY, not Sunshine/Moonlight integration, decoding, AES or FEC',
        frames=frames,received_frames=len(received),audio_seconds=frames*.020,
        reverse_fragments=reverse,packets=len(wire_sizes),fragments_per_frame=math.ceil(PERIOD/DATA_BYTES),
        max_udp_payload=max(wire_sizes,default=0),max_ipv4_packet_without_options=max(wire_sizes,default=0)+28,
        carrier_mbps=PERIOD*8/.020/1e6,rtp_udp_payload_mbps=sum(wire_sizes)*8/(frames*.020)/1e6,
        input_sha256=hashlib.sha256(data).hexdigest(),received_sha256=hashlib.sha256(restored).hexdigest(),
        identical=data==restored,errors=errors,incomplete_frames=list(assembly.pending),
        availability_to_reassembly_ms=dict(p50=percentile(.5),p99=percentile(.99),max=max(latency,default=0)),
        latency_caveat='Same-process Python loopback scheduling; excludes capture, 20ms burst fill, Dolby decode/output. Not a production latency promise.',
        passed=bool(complete and data==restored))
    (output/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps(result,indent=2));return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--reverse-fragments',action='store_true')
    args=parser.parse_args();result=run(args.source,args.output,args.reverse_fragments)
    raise SystemExit(0 if result['passed'] else 1)
