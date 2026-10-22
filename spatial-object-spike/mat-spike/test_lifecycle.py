import csv,json,tempfile,unittest
from pathlib import Path
import numpy as np
from compare_lifecycle import compare_tracks,timings
from prepare_lifecycle_input import build_case,OBJECT,BLOCKS


class LifecycleChecks(unittest.TestCase):
    def fixture(self):
        rng=np.random.default_rng(92)
        source=np.zeros((2,2400))
        source[0,480:960]=rng.uniform(-.02,.02,480)
        source[1,1440:1920]=rng.uniform(-.02,.02,480)
        segments=[dict(generation=0,id=9,created_block=1,source_start_block=1,end_block=2,frames=480),
                  dict(generation=1,id=9,created_block=3,source_start_block=3,end_block=4,frames=480)]
        slots=np.zeros((20,3840));slots[4]=np.pad(np.floor(source.sum(axis=0)*32768)/32768,(0,1440))
        return source,slots,segments,np.arange(6,14),np.zeros((8,20,6),dtype=np.uint8)

    def test_same_id_two_generations_same_slot(self):
        result=compare_tracks(*self.fixture())
        self.assertTrue(result['full_timeline_audio_check_passed'])
        self.assertEqual([r['observed_slot'] for r in result['objects']],[15,15])

    def test_generation_can_move_to_different_slot(self):
        source,slots,segments,ticks,fields=self.fixture()
        slots[8,1440:1920]=slots[4,1440:1920];slots[4,1440:1920]=0
        result=compare_tracks(source,slots,segments,ticks,fields)
        self.assertTrue(result['full_timeline_audio_check_passed'])
        self.assertEqual([r['observed_slot'] for r in result['objects']],[15,19])

    def test_retained_audio_after_end_or_missing_source_cannot_pass(self):
        source,slots,segments,ticks,fields=self.fixture()
        slots[4,2400]=.01
        result=compare_tracks(source,slots,segments,ticks,fields)
        self.assertFalse(result['full_timeline_audio_check_passed'])
        self.assertEqual(result['unassigned_dynamic_nonzero_samples'],1)
        slots[4,2400]=0;slots[4,1440:1460]=0
        self.assertFalse(compare_tracks(source,slots,segments,ticks,fields)['full_timeline_audio_check_passed'])

    def test_missing_carrier_half_does_not_pass(self):
        source,slots,segments,ticks,fields=self.fixture();ticks[3:]+=1
        self.assertFalse(compare_tracks(source,slots,segments,ticks,fields)['full_timeline_audio_check_passed'])

    def test_individual_matches_cannot_hide_generation_time_shift(self):
        source,slots,segments,ticks,fields=self.fixture()
        second=slots[4,1440:1920].copy();slots[4,1440:1920]=0;slots[4,1460:1940]=second
        result=compare_tracks(source,slots,segments,ticks,fields)
        self.assertTrue(all(row['observed_full_source_continuity_passed'] for row in result['objects']))
        self.assertFalse(result['all_generations_share_one_timeline_offset'])
        self.assertFalse(result['full_timeline_audio_check_passed'])

    def test_generated_schedule_has_real_create_end_reuse_and_twenty(self):
        for kind in [1,2]:
            raw,audio,xyz,segments=build_case(kind)
            self.assertEqual(len(raw),12+BLOCKS*(4+20*OBJECT.size))
            self.assertTrue(np.isfinite(audio).all());self.assertTrue(np.isfinite(xyz).all())
            if kind==1:
                reused=[s for s in segments if s['id']==2001]
                self.assertEqual(len(reused),2)
                self.assertLess(reused[0]['end_block'],reused[1]['created_block'])
            else:self.assertEqual(len(segments),20)

    def test_deadlines_are_measured_not_assumed(self):
        names=['block','target_qpc','start_qpc','process_start_qpc','finish_qpc','submissions','active_objects']
        rows=[[b,1_000_000+b*10000,1_000_050+b*10000,1_000_080+b*10000,1_000_200+b*10000,0,0] for b in range(240)]
        summary=dict(processed_source_blocks=240,qpc_frequency=1_000_000)
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'timing.csv'
            def save():
                with path.open('w',newline='') as stream:
                    writer=csv.writer(stream);writer.writerow(names);writer.writerows(rows)
            save();self.assertTrue(timings(path,summary,[])['deadline_check_passed'])
            rows[-1][4]=rows[-1][1]+11000
            save();result=timings(path,summary,[])
            self.assertFalse(result['deadline_check_passed']);self.assertEqual(result['missed_deadline_blocks'],[239])


if __name__=='__main__':unittest.main()
