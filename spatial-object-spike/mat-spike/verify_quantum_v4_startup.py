"""Offline startup diagnosis; reads disk images and the existing native capture.

No process attachment, DLL loading, audio activation, inverse-gain repair or
free parameter fitting. Models SSE float32 operations from matching PE/PDB.
"""
import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np
from verify_native_quantum_v4_content import HEADER, check, digest, load, source_pcm


class PE:
    def __init__(self, path, expected):
        self.path = path
        check(digest(path).upper() == expected, 'PE version/hash mismatch')
        self.data = path.read_bytes()
        nt = struct.unpack_from('<I', self.data, 0x3c)[0]
        check(self.data[nt:nt + 4] == b'PE\0\0', 'PE signature')
        count = struct.unpack_from('<H', self.data, nt + 6)[0]
        optional = struct.unpack_from('<H', self.data, nt + 20)[0]
        table = nt + 24 + optional
        self.sections = [struct.unpack_from('<8sIIII', self.data, table + n * 40)
                         for n in range(count)]

    def at(self, rva, count):
        for _, _, address, size, offset in self.sections:
            if address <= rva and rva + count <= address + size:
                return self.data[offset + rva - address:offset + rva - address + count]
        raise ValueError('RVA not disk-backed')

    def guard(self, rva, text):
        expected = bytes.fromhex(text)
        check(self.at(rva, len(expected)) == expected, f'code guard {rva:x}')


def fade_from_disassembly(frames, exponent, table_size=16):
    f = np.float32
    # CFadeWindowLUT::Initialize: table[k]=(1-k/M)^exponent,
    # delta[k]=table[k]-table[k+1]. Values derive from PE, not capture ratios.
    table = np.array([f(math.pow(float(f(f(1) - f(f(k) / f(table_size)))), exponent))
                      for k in range(table_size + 1)], dtype='<f4')
    delta = np.zeros(table_size + 1, dtype='<f4')
    delta[:-1] = table[:-1] - table[1:]
    # SetStep(fade-in): position=M, step=F32(-F32(M/frames)),
    # offset=1, scale=-1. Apply uses 1-(offset+scale*interpolated).
    position = f(table_size)
    step = f(f(f(table_size) / f(frames)) * f(-1))
    envelope = np.zeros(frames, dtype='<f4')
    for sample in range(frames):
        index = int(position)
        fraction = f(position - f(index))
        position = f(position + step)
        interpolated = f(table[index] - f(fraction * delta[index]))
        inverted = f(f(interpolated * f(-1)) + f(1))
        envelope[sample] = f(f(1) - inverted)
    return envelope


def set_position(properties, xyz):
    struct.pack_into('<fff', properties, 16, *xyz)
    flags = struct.unpack_from('<I', properties, 12)[0]
    struct.pack_into('<I', properties, 12, flags | 4)


def first_get_buffer(properties, volume):
    # Basic dynamic-object branch at audioses RVA34c60..34c91.
    flags = struct.unpack_from('<I', properties, 12)[0]
    struct.pack_into('<I', properties, 12, flags | 3 | 4)
    struct.pack_into('<ffff', properties, 16, 0., 0., 0., volume)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run', type=Path, required=True)
    ap.add_argument('--content-proof', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    engine = PE(Path('C:/Windows/System32/audioeng.dll'),
                '3936BFFDC4AA365E74C9995359F2821AAF759A14155A180CFA95764962B9F1D3')
    session = PE(Path('C:/Windows/System32/audioses.dll'),
                 '95AF7710EC1A13252283B3740A3EFEEB7C014790F99DC9404D6AEC426E5C6C4E')
    engine.guard(0x6d4d0, '48c7819000000010000000')
    engine.guard(0xfa332, 'e8d99ef4ff')  # APOProcess -> CFadeProcessor::Apply.
    session.guard(0x34c83, '895810')
    session.guard(0x34c8a, '895814')
    session.guard(0x34c91, '895818')
    session.guard(0x30430, 'f3440f114010')
    exponent = struct.unpack('<f', engine.at(0x106d9c, 4))[0]
    check(exponent == 1.75, 'unexpected fade exponent')
    proof = load(args.content_proof)
    capture = args.run / 'capture'
    check(proof['capture_integrity_passed'] and proof['startup_anomalous_objects'] == 3,
          'requires unchanged bounded source proof')
    expected = proof['source_files']
    check([dict(file=r['file'], bytes=(capture/r['file']).stat().st_size,
                sha256=digest(capture/r['file'])) for r in expected] == expected, 'capture changed')
    starts = []
    for row in expected:
        with (capture / row['file']).open('rb') as stream:
            while raw := stream.read(160):
                check(len(raw) == 160, 'short header')
                h = HEADER.unpack(raw)
                payload = stream.read(h[1] * 4)
                check(len(payload) == h[1]*4, 'short PCM')
                if h[14] == 4 and h[15] & 1:
                    starts.append((h, np.frombuffer(payload, dtype='<f4').copy()))
    check(len(starts) == 3, 'startup count')
    envelope = fade_from_disassembly(480, exponent)
    wrong_exponent = fade_from_disassembly(480, 1.0)
    rows = []
    tests = []
    all_model_exact = 0
    for h, pcm in starts:
        variant = h[21] - 1
        which = 0 if h[9] > .09 else 1
        source = source_pcm(variant, which, 0)
        model = source * envelope
        exact = int(np.count_nonzero(model.view('<u4') == pcm.view('<u4')))
        max_error = float(np.max(np.abs(model.astype(np.float64) - pcm.astype(np.float64))))
        check(exact == 480, 'PE-derived fade does not explain every captured startup sample')
        check(not np.array_equal((source * wrong_exponent).view('<u4'), pcm.view('<u4')),
              'wrong exponent negative control')
        all_model_exact += exact
        rows.append(dict(domain=h[21], object_id=h[4], variant=variant, source_object=which,
                         samples=480, model_bit_exact_samples=exact, model_max_error=max_error))
        boundary = next(b for b in proof['startup_boundaries'] if b['event_sequence'] == h[12])
        xyz = boundary['source_xyz']
        old = bytearray(32)
        set_position(old, xyz)
        first_get_buffer(old, h[9])
        check(struct.unpack_from('<fff', old, 16) == h[6:9] and struct.unpack_from('<I', old, 12)[0] == h[15],
              'old source order model differs from native first properties')
        corrected = bytearray(32)
        first_get_buffer(corrected, h[9])
        set_position(corrected, xyz)
        check(struct.unpack_from('<fff', corrected, 16) == tuple(xyz), 'new order model')
        tests.append(dict(domain=h[21], source_object=which,
            old_order='SetPosition -> first GetBuffer', old_order_xyz=list(struct.unpack_from('<fff', old, 16)),
            corrected_order='first GetBuffer -> SetPosition', corrected_order_xyz=list(struct.unpack_from('<fff', corrected, 16)),
            corrected_order_model_passed=True, corrected_order_native_tested=False))
    check(all_model_exact == 1440, 'model total')
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(passed=True, scope='Offline disassembly-derived models against unchanged native01 capture',
        audioeng_sha256=digest(engine.path), audioses_sha256=digest(session.path),
        script_sha256=digest(Path(__file__)), input_content_proof_sha256=digest(args.content_proof),
        raw_capture_unchanged=True, additional_uac=False, process_attached=False,
        dll_loaded_or_audio_played=False, fade_lut_segments=16, fade_exponent=exponent,
        fade_model_samples=1440, fade_model_bit_exact_samples=all_model_exact,
        fade_model_max_error=0., fitted_parameters=False, inverse_processing=False,
        first_packet_models=rows, source_call_order_models=tests,
        fade_causal_status='Strong static call-chain plus bit-exact waveform evidence; no same-call live upstream/downstream probe',
        coordinate_causal_status='Source calls setters before GetBuffer; matched audioses startup branch overwrites XYZ. Offline model reproduces captured metadata.',
        corrected_source_native_verified=False, capture_default_changed=False,
        conclusion='Keep native fade for faithful Windows-pipeline replay. Fix source probe GetBuffer-before-setters order before blaming capture for startup XYZ. No need to inverse-fade or move production hook based only on this source test.')
    (args.output / 'proof.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
