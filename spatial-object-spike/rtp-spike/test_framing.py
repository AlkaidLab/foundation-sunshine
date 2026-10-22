import unittest
from run import JOC, crc16, fixed_frame, split_frames


class Framing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frames = split_frames((JOC/'stream06/game2/objects.eac3').read_bytes())

    def test_real_fixture(self):
        self.assertEqual(len(self.frames),93)
        self.assertTrue(all(crc16(f[2:])==0 for f in self.frames))

    def test_fixed_size_crc_and_audio_prefix(self):
        for frame in self.frames:
            padded=fixed_frame(frame,3360)
            self.assertEqual(len(padded),3360)
            self.assertEqual(crc16(padded[2:]),0)
            self.assertEqual(split_frames(padded),[padded])
            self.assertEqual(frame[4:-3],padded[4:len(frame)-3])
            self.assertEqual(frame[-3] & 0xfc,padded[len(frame)-3] & 0xfc)
            self.assertEqual(fixed_frame(padded,3360),padded)

    def test_reject_malformed_or_unsupported(self):
        for data in [b'', self.frames[0][:-1],b'x'+self.frames[0]]:
            with self.assertRaises(ValueError): split_frames(data)
        frame=self.frames[0]
        for size in [len(frame)-2,4096,3361]:
            with self.assertRaises(ValueError): fixed_frame(frame,size)
        bad=bytearray(frame); bad[10]^=1
        with self.assertRaises(ValueError): fixed_frame(bytes(bad),3360)
        bad=bytearray(frame); bad[-3]|=2
        with self.assertRaises(ValueError): fixed_frame(bytes(bad),3360)


if __name__=='__main__': unittest.main()
