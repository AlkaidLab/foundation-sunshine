// NON-PRODUCTION fixed RFC 7748 keys. Prints deterministic interoperability
// vectors for the v1 draft; it is not a production enrollment implementation.
import { createHash, createHmac, createPrivateKey, createPublicKey, diffieHellman } from 'node:crypto'

const bytes = (hex) => Buffer.from(hex, 'hex')
const label = (name) => Buffer.from(`${name}\0`, 'ascii')
const sha = (data) => createHash('sha256').update(data).digest()
const mac = (key, data) => createHmac('sha256', key).update(data).digest()
const hex = (data) => data.toString('hex')
const xkey = (priv, pub) => createPrivateKey({
  key: { kty: 'OKP', crv: 'X25519', d: priv.toString('base64url'), x: pub.toString('base64url') }, format: 'jwk',
})

const token = bytes('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f')
const certHash = bytes('202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f')
const challenge = bytes('404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f')
const bootstrapId = bytes('606162636465666768696a6b6c6d6e6f')
const hostPrivate = bytes('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a')
const hostPublic = bytes('8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a')
const clientPrivate = bytes('5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb')
const clientPublic = bytes('de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f')
const clientCertHash = bytes('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f')
const hostIp = bytes('64400a01')
const clientIp = bytes('64400a02')
const port = 47989
const peer = Buffer.from('udp://discovery.example:11010', 'ascii')
const portBytes = Buffer.alloc(2); portBytes.writeUInt16BE(port)
const peerLength = Buffer.alloc(2); peerLength.writeUInt16BE(peer.length)
const host = xkey(hostPrivate, hostPublic)
const client = xkey(clientPrivate, clientPublic)
const shared = diffieHellman({ privateKey: host, publicKey: createPublicKey(client) })
const clientShared = diffieHellman({ privateKey: client, publicKey: createPublicKey(host) })
if (!shared.equals(clientShared) || hex(shared) !== '4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742') throw new Error('X25519 vector mismatch')
const transcript = Buffer.concat([
  label('VPLUS-ENROLL-POP-V1'), Buffer.from([1]), sha(token), certHash,
  bootstrapId, challenge, hostPublic, clientPublic, clientCertHash, hostIp, clientIp, portBytes, peerLength, peer,
])
const salt = sha(Buffer.concat([label('VPLUS-ENROLL-HKDF-SALT-V1'), transcript]))
const prk = mac(salt, shared)
const proofKey = mac(prk, Buffer.concat([label('VPLUS-ENROLL-POP-KEY-V1'), Buffer.from([1])]))
const proof = mac(proofKey, Buffer.concat([label('VPLUS-ENROLL-POP-PROOF-V1'), transcript]))
const confirmationHash = sha(Buffer.concat([label('VPLUS-ENROLL-SAS-V1'), transcript, proof]))
const bootstrapPrk = mac(bootstrapId, token)
const bootstrapSecret = mac(bootstrapPrk, Buffer.concat([label('VPLUS-ENROLL-BOOTSTRAP-SECRET-V1'), Buffer.from([1])]))
const vector = {
  notice: 'PUBLIC TEST KEYS ONLY - NEVER USE FOR REAL ENROLLMENT',
  version: 1,
  token: hex(token), host_certificate_der_sha256: hex(certHash), bootstrap_id: hex(bootstrapId),
  challenge: hex(challenge), host_x25519_private: hex(hostPrivate), host_x25519_public: hex(hostPublic),
  client_x25519_private: hex(clientPrivate), client_x25519_public: hex(clientPublic), client_certificate_der_sha256: hex(clientCertHash),
  host_ipv4: '100.64.10.1', client_ipv4: '100.64.10.2', enrollment_port: port, discovery_peer: peer.toString('ascii'),
  token_sha256: hex(sha(token)), x25519_shared: hex(shared), transcript: hex(transcript),
  hkdf_salt: hex(salt), hkdf_prk: hex(prk), proof_key: hex(proofKey), proof: hex(proof),
  confirmation_sha256: hex(confirmationHash), confirmation_code: hex(confirmationHash.subarray(0, 8)).match(/.{4}/g).join('-'),
  bootstrap_prk: hex(bootstrapPrk), bootstrap_secret: hex(bootstrapSecret),
}
process.stdout.write(`${JSON.stringify(vector, null, 2)}\n`)
