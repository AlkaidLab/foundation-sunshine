import struct
import unittest
import json
import tempfile
import numpy as np
from pathlib import Path
from analyze_mat import FULL, LFE, TABLE, PERIOD, PREAMBLE, RECORD, inspect_burst, object_fields, unswap, read_capture
from transport_probe import MAX_DATAGRAM, Reassembler, packets
from compare_native import coverage, infer_lag


class ContinuityChecks(unittest.TestCase):
    def setUp(self):
        self.source=np.random.default_rng(7).normal(0,.01,48000)
        self.decoded=np.pad(np.floor(self.source*32768)/32768,(9184,1376))
        self.ticks=np.arange(6,128)

    def test_preroll_alignment_and_whole_source(self):
        lag=infer_lag(self.source,self.decoded)
        self.assertEqual(lag,9184)
        result=coverage(self.source,self.decoded,lag,self.ticks)
        self.assertTrue(result['full_source_observed_within_one_pcm16_lsb'])
        self.assertEqual(result['outside_source_nonzero_samples'],0)

    def test_missing_half_cannot_be_hidden_by_concatenation(self):
        ticks=self.ticks.copy();ticks[30:]+=1
        self.assertFalse(coverage(self.source,self.decoded,9184,ticks)['full_source_observed_within_one_pcm16_lsb'])

    def test_repeated_or_missing_source_samples_cannot_pass(self):
        changed=self.decoded.copy();changed[9184+12000:9184+12480]=changed[9184+11520:9184+12000]
        self.assertFalse(coverage(self.source,changed,9184,self.ticks)['full_source_observed_within_one_pcm16_lsb'])
        self.assertFalse(coverage(self.source,self.decoded[9600:],-416,self.ticks)['full_source_observed_within_one_pcm16_lsb'])

    def test_extra_audio_outside_source_is_exposed(self):
        changed=self.decoded.copy();changed[12]=.01
        self.assertEqual(coverage(self.source,changed,9184,self.ticks)['outside_source_nonzero_samples'],1)


class MatChecks(unittest.TestCase):
    def test_independent_capture_integrity_is_not_hook_restoration(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            summary=dict(capture_kind='independent_mat_encoder',status=2,error=0,thread_completed=True,
                initialized=True,hooks_installed=False,source_blocks=1,processed_source_blocks=1,
                create_hr=0,mapper_hr=0,initialize_hr=0,submit_hr=0,process_hr=0,
                dropped=0,invalid=0,failures=0,records=1,bytes=0)
            (root/'encoder-output.bin').write_bytes(RECORD.pack(0x42595445,0,0,1,2,480,30720,0,3,0,0,0))
            (root/'encoder-payload.raw').write_bytes(b'')
            (root/'summary.json').write_text(json.dumps(summary))
            data,records,_=read_capture(root)
            self.assertEqual(data,b'');self.assertEqual(len(records),1)
            for key,value in [('initialized',False),('processed_source_blocks',0),('hooks_installed',True),('failures',1)]:
                changed={**summary,key:value};(root/'summary.json').write_text(json.dumps(changed))
                with self.assertRaises(ValueError):read_capture(root)
            (root/'summary.json').write_text(json.dumps(summary))
            phase=dict(protocol_version=2,preroll_blocks=0,processed_preroll_blocks=0,
                tail_blocks=0,processed_tail_blocks=0,source_blocks=1,processed_source_blocks=1,preroll_frames=0)
            (root/'processing.json').write_text(json.dumps(phase))
            read_capture(root)
            for key,value in [('processed_preroll_blocks',1),('source_blocks',2),('tail_blocks',1),('preroll_frames',480)]:
                (root/'processing.json').write_text(json.dumps({**phase,key:value}))
                with self.assertRaises(ValueError):read_capture(root)

    def test_word_swap_roundtrip(self):
        source=bytes(range(256))*4
        self.assertEqual(unswap(unswap(source)),source)
        with self.assertRaises(ValueError):unswap(b'odd')

    def test_fields_cross_nibbles(self):
        rows=[tuple((obj*7+i*3)%64 for i in range(6)) for obj in range(20)]
        bits=''.join(f'{v:06b}' for row in rows for v in row)
        payload=TABLE+int(bits,2).to_bytes(90,'big')
        for obj,row in enumerate(rows):self.assertEqual(object_fields(payload,0,obj),row)
        with self.assertRaises(ValueError):object_fields(payload[:-1],0,19)

    def test_reject_incomplete_and_invalid_headers(self):
        with self.assertRaises(ValueError):inspect_burst(bytes(PERIOD-1))
        for pc,pd in [(0x15,100),(0x96,100),(22,1),(22,65534)]:
            data=PREAMBLE+struct.pack('<HH',pc,pd)+bytes(PERIOD-8)
            with self.assertRaises(ValueError):inspect_burst(data)

    def test_no_fake_decoded_objects(self):
        data=PREAMBLE+struct.pack('<HH',22,100)+bytes(PERIOD-8)
        result,_,audio,low,fields=inspect_burst(data)
        self.assertFalse(result['observed_layout_valid'])
        self.assertIsNone(audio);self.assertIsNone(low);self.assertIsNone(fields)

    def test_bounds_on_false_marker(self):
        logical=FULL*62+LFE*2+TABLE*2
        data=PREAMBLE+struct.pack('<HH',22,len(logical))+unswap(logical)
        data+=bytes(PERIOD-len(data))
        result,_,audio,_,_=inspect_burst(data)
        self.assertFalse(result['observed_layout_valid']);self.assertIsNone(audio)


class FragmentChecks(unittest.TestCase):
    def setUp(self):self.frame=PREAMBLE+bytes(PERIOD-len(PREAMBLE))

    def test_reversed_and_duplicate_fragments(self):
        chunks=list(packets(self.frame,7));self.assertEqual(len(chunks),53)
        self.assertTrue(all(len(p)<=MAX_DATAGRAM for p in chunks))
        decoder=Reassembler();result=None
        for packet in reversed(chunks):
            item=decoder.push(packet)
            if item is not None:result=item
        self.assertEqual(result,(7,self.frame))
        self.assertIsNone(decoder.push(chunks[0]))
        self.assertFalse(decoder.pending)

    def test_missing_fragment_does_not_fabricate_frame(self):
        decoder=Reassembler()
        for packet in list(packets(self.frame,1))[1:]:self.assertIsNone(decoder.push(packet))
        self.assertEqual(set(decoder.pending),{1});self.assertFalse(decoder.completed)

    def test_checksum_and_conflicting_duplicate(self):
        chunks=list(packets(self.frame,0))
        corrupt=bytearray(chunks[0]);corrupt[-1]^=1
        decoder=Reassembler();decoder.push(chunks[0])
        with self.assertRaises(ValueError):decoder.push(corrupt)
        decoder=Reassembler();decoder.push(corrupt)
        with self.assertRaises(ValueError):
            for packet in chunks[1:]:decoder.push(packet)

    def test_window_and_packet_bounds(self):
        decoder=Reassembler()
        for index in range(8):decoder.push(next(packets(self.frame,index)))
        with self.assertRaises(ValueError):decoder.push(next(packets(self.frame,8)))
        for data in [b'',bytes(MAX_DATAGRAM+1)]:
            with self.assertRaises(ValueError):Reassembler().push(data)


if __name__=='__main__':unittest.main()
