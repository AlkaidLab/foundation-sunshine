from pathlib import Path
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import subprocess,struct,json,os,argparse
parser=argparse.ArgumentParser();parser.add_argument('--executable',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
out=args.output
env=dict(os.environ);env['PATH']='C:/msys64/ucrt64/bin;'+env['PATH']
subprocess.run([str(args.executable.resolve()),str(out.resolve())],env=env,check=True)
key=HKDF(algorithm=hashes.SHA256(),length=16,salt=b'Sunshine MAT live v1',info=b'MATL AES128 GCM'+struct.pack('<QQ',123,7)).derive(bytes(range(16)))
header=b'MLIV'+bytes([1,32,0,0])+struct.pack('<QQII',123,65536,1104,0)
expected=key+header+AESGCM(key).encrypt(b'MATL'+struct.pack('<Q',65536),bytes(i%256 for i in range(1104)),header)
assert out.read_bytes()==expected
out.with_suffix('.json').write_text(json.dumps({'result':'PASS','reference':'Python cryptography HKDF SHA256 / AES128-GCM','sequence':65536,'bytes':len(expected),'nativeCaptureTested':False},indent=2))
print('PASS independent HKDF/AES-GCM complete packet oracle')
