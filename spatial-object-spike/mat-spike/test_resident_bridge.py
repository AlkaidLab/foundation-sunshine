import hashlib,math,tempfile,time,unittest
from unittest.mock import patch
from pathlib import Path
import numpy as np
from resident_mat_bridge import Bridge,StateCache,Snapshot,ObjectFramer,Record,HEADER,FRAME,CALL,lockstep_batches,scaled_pcm
from replay_resident_bridge import paced


def record(seq=0,host=7,obj=42,gen=1,flags=6,state=1,xyz=(1.,2.,3.),raw=None,epoch=0,pcm=None,typ=1):
    if raw is None:raw=xyz if flags&4 else (0.,0.,0.)
    if pcm is None:pcm=np.full(480,.1,dtype='<f4')
    h=(0x324f424a,len(pcm),host,1000000+seq*1000,obj,typ,*xyz,.5,.25,123,seq,2,flags,state,1,epoch,gen,*raw,0)
    # Round metadata exactly as the real disk protocol does.
    return Record(HEADER.unpack(HEADER.pack(*h)),pcm)


def calls(blob):
    frame=FRAME.unpack_from(blob);offset=FRAME.size;result=[]
    for _ in range(frame[2]):
        h=CALL.unpack_from(blob,offset);offset+=CALL.size
        pcm=np.frombuffer(blob,dtype='<f4',count=h[7],offset=offset);offset+=4*h[7];result.append((h,pcm))
    if offset!=len(blob):raise AssertionError('trailing call bytes')
    return result


class ResidentBridgeChecks(unittest.TestCase):
    def test_cached_position_and_gain_once_across_chunk_boundary(self):
        bridge=Bridge('session')
        first=record();first.chunk='part0';second=record(1,flags=2,state=2);second.chunk='part1'
        a,_=bridge.consume(0,[first]);b,_=bridge.consume(1,[second])
        one=calls(a)[0];two=calls(b)[0]
        self.assertEqual(one[0][:4],(1,0,1,7));self.assertEqual(two[0][:4],(1,0,1,6))
        self.assertEqual(two[0][4:7],(1.,2.,3.))
        np.testing.assert_array_equal(two[1],np.full(480,np.float32(.1)*np.float32(.5)*np.float32(.25),dtype='<f4'))
        self.assertEqual(bridge.cache.counts['position_state_2'],1)

    def test_unknown_is_not_fabricated_origin_and_audible_is_reported(self):
        bridge=Bridge('session');nan=(math.nan,)*3
        unknown=record(flags=2,state=0,xyz=nan,pcm=np.zeros(480,dtype='<f4'))
        output,_=bridge.consume(0,[unknown]);self.assertFalse(calls(output))
        output,_=bridge.consume(1,[record(1,flags=2,state=0,xyz=nan)])
        self.assertFalse(calls(output));self.assertEqual(bridge.counts['quarantined_unknown_audible_records'],1)
        output,_=bridge.consume(2,[record(2)])
        self.assertEqual(calls(output)[0][0][3],7)

    def test_corrupted_reconstructed_coordinate_rejected(self):
        cache=StateCache();cache.apply(record())
        bad=record(1,flags=2,state=2,xyz=(0.,2.,3.))
        with self.assertRaises(ValueError):cache.apply(bad)

    def test_cross_host_id_collision_gets_different_encoder_ids(self):
        bridge=Bridge('session');out,_=bridge.consume(0,[record(),record(1,host=8)])
        self.assertEqual([c[0][0] for c in calls(out)],[1,2])

    def test_call_reorder_does_not_rebind_source_slots_or_native_ids(self):
        bridge=Bridge('session');bridge.consume(0,[record(0,obj=42),record(1,obj=43)])
        out,_=bridge.consume(1,[record(2,obj=43,flags=2,state=2),record(3,obj=42,flags=2,state=2)])
        self.assertEqual([(h[0],h[1]) for h,_ in calls(out)],[(2,1),(1,0)])

    def test_same_id_new_generation_retires_old_native_id(self):
        bridge=Bridge('session');bridge.consume(0,[record()])
        out,_=bridge.consume(1,[record(1,gen=2,flags=7)])
        self.assertEqual([(h[0],h[3]) for h,_ in calls(out)],[(1,8),(2,7)])
        self.assertEqual(len(bridge.active),1)

    def test_explicit_end_preserves_last_audio_then_reuse_is_new_id(self):
        bridge=Bridge('session');bridge.consume(0,[record()])
        last,_=bridge.consume(1,[record(1,flags=8,state=2)])
        self.assertEqual(calls(last)[0][0][3],6);self.assertTrue(np.count_nonzero(calls(last)[0][1]))
        out,_=bridge.consume(2,[record(2,gen=2,flags=7)])
        self.assertEqual([(h[0],h[3]) for h,_ in calls(out)],[(1,8),(2,7)])
        self.assertEqual(bridge.counts['bridge_end_source_end'],1)

    def test_loss_invalidates_all_cached_encoder_objects(self):
        bridge=Bridge('session');bridge.consume(0,[record(),record(1,host=8)])
        out,_=bridge.consume(1,[record(2,gen=3,epoch=1,flags=2,state=0,xyz=(math.nan,)*3,pcm=np.zeros(480,dtype='<f4'))])
        self.assertEqual([h[3] for h,_ in calls(out)],[8,8]);self.assertFalse(bridge.active)
        self.assertEqual(bridge.cache.counts['loss_invalidations'],1)

    def test_default_origin_requires_actual_create_flag(self):
        created=record(flags=1,state=3,xyz=(0.,0.,0.));header=list(created.h);header[9]=1.
        snapshot=StateCache().apply(Record(tuple(header),created.pcm))
        self.assertEqual(snapshot.xyz,(0.,0.,0.));self.assertEqual(snapshot.volume,1.)
        unknown=StateCache().apply(record(flags=2,state=0,xyz=(math.nan,)*3))
        self.assertIsNone(unknown.xyz)

    def test_partial_and_large_chunks_reassemble_without_sample_loss(self):
        source=np.arange(1440,dtype='<f4');framer=ObjectFramer();output=[]
        cursor=0
        for length in [113,367,960]:output.extend(framer.push(source[cursor:cursor+length],(1.,2.,3.)));cursor+=length
        framer.finish();self.assertEqual(len(output),3)
        np.testing.assert_array_equal(np.concatenate([x.pcm for x in output]),source)
        self.assertLessEqual(framer.max_buffered,480)

    def test_midframe_coordinate_change_is_not_flattened(self):
        framer=ObjectFramer();self.assertFalse(framer.push(np.ones(257,dtype='<f4'),(1.,0.,0.)))
        frame=framer.push(np.ones(223,dtype='<f4'),(2.,0.,0.))[0]
        self.assertFalse(frame.single_position_ready);self.assertEqual([s['offset'] for s in frame.spans],[0,257])

    def test_tail_is_not_silently_dropped_or_padded(self):
        framer=ObjectFramer();framer.push(np.ones(17,dtype='<f4'),(0.,0.,0.))
        with self.assertRaises(ValueError):framer.finish()

    def test_eof_end_is_explicit_and_bridge_cannot_restart(self):
        bridge=Bridge('session');bridge.consume(0,[record()]);end=bridge.finish()
        self.assertEqual(FRAME.unpack_from(end)[3:],(1,-1,-1))
        self.assertEqual(calls(end)[0][0][3],8);self.assertEqual(bridge.counts['bridge_end_capture_eof'],1)
        with self.assertRaises(ValueError):bridge.consume(1,[record(1)])

    def test_rounds_can_span_rotated_files_but_sequence_gaps_fail(self):
        rows=[record(0,obj=1),record(1,obj=2),record(2,obj=1),record(3,obj=2)]
        encode=lambda r:HEADER.pack(*r.h)+r.pcm.tobytes()
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'global-objects.bin').write_bytes(encode(rows[0]))
            second=root/'global-objects-0001.bin';second.write_bytes(b''.join(encode(r) for r in rows[1:]))
            self.assertEqual(len(list(lockstep_batches(root,2))),2)
            second.write_bytes(b''.join(encode(r) for r in rows[2:]))
            with self.assertRaises(ValueError):list(lockstep_batches(root,2))

    def test_round_key_set_accepts_reorder_but_rejects_duplicate(self):
        rows=[record(0,obj=1),record(1,obj=2),record(2,obj=2),record(3,obj=1)]
        encode=lambda r:HEADER.pack(*r.h)+r.pcm.tobytes()
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'global-objects.bin';path.write_bytes(b''.join(encode(r) for r in rows))
            self.assertEqual(len(list(lockstep_batches(Path(temp),2))),2)
            rows[-1]=record(3,obj=2);path.write_bytes(b''.join(encode(r) for r in rows))
            with self.assertRaises(ValueError):list(lockstep_batches(Path(temp),2))

    def test_paced_queue_prefix_and_finalization_match_unpaced(self):
        rows=[record(b*2+i,obj=1+i) for b in range(4) for i in range(2)]
        bridge=Bridge('test');expected=hashlib.sha256()
        for b in range(4):
            out,_=bridge.consume(b,rows[b*2:b*2+2])
            if b>=1:expected.update(out)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'global-objects.bin').write_bytes(b''.join(HEADER.pack(*r.h)+r.pcm.tobytes() for r in rows))
            result=paced(root,2,'test',1,4,10_000_000,expected.hexdigest())
            self.assertTrue(result['passed']);self.assertEqual(result['completed_rounds'],3)

    def test_backpressure_aborts_instead_of_silent_drop_and_continue(self):
        class SlowBridge(Bridge):
            def consume(self,*args):
                time.sleep(.003);return super().consume(*args)
        rows=[record(b*2+i,obj=1+i) for b in range(30) for i in range(2)]
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'global-objects.bin').write_bytes(b''.join(HEADER.pack(*r.h)+r.pcm.tobytes() for r in rows))
            with patch('replay_resident_bridge.Bridge',SlowBridge):
                result=paced(root,2,'test',0,30,10_000_000,'not-an-expected-hash')
            self.assertFalse(result['passed']);self.assertEqual(result['producer_overrun_count'],1)
            self.assertLess(result['completed_rounds'],30)


if __name__=='__main__':unittest.main()
