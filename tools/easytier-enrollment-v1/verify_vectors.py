"""Independently verify the NON-PRODUCTION v1 vector with Python and OpenSSL.

Uses only Python's standard library, Node.js for cross-checking the JSON
generator, and a locally installed OpenSSL executable for X25519 operations.
No actual credentials, network access, or running EasyTier instance are used.
"""
import hashlib
import hmac
import ipaddress
import json
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VECTOR = ROOT / "tests/fixtures/remote-connect/enrollment-v1.json"
GENERATOR = ROOT / "tools/easytier-enrollment-v1/generate_vectors.mjs"
PKCS8_PREFIX = bytes.fromhex("302e020100300506032b656e04220420")
SPKI_PREFIX = bytes.fromhex("302a300506032b656e032100")


def label(value):
    return value.encode("ascii") + b"\0"


def sha(value):
    return hashlib.sha256(value).digest()


def mac(key, value):
    return hmac.new(key, value, hashlib.sha256).digest()


def unhex(v, name, length):
    result = bytes.fromhex(v[name])
    if len(result) != length or v[name] != result.hex():
        raise ValueError(f"{name} must be canonical lowercase hex of length {length}")
    return result


def transcript(v):
    if type(v["version"]) is not int or v["version"] != 1:
        raise ValueError("unsupported version")
    token = unhex(v, "token", 32)
    host_cert = unhex(v, "host_certificate_der_sha256", 32)
    bootstrap_id = unhex(v, "bootstrap_id", 16)
    challenge = unhex(v, "challenge", 32)
    host_public = unhex(v, "host_x25519_public", 32)
    client_public = unhex(v, "client_x25519_public", 32)
    client_cert = unhex(v, "client_certificate_der_sha256", 32)
    parsed_ip = ipaddress.IPv4Address(v["host_ipv4"])
    if str(parsed_ip) != v["host_ipv4"]:
        raise ValueError("non-canonical IPv4 address")
    address = parsed_ip.packed
    parsed_client_ip = ipaddress.IPv4Address(v["client_ipv4"])
    if str(parsed_client_ip) != v["client_ipv4"]:
        raise ValueError("non-canonical client IPv4 address")
    client_address = parsed_client_ip.packed
    port = v["enrollment_port"]
    if not isinstance(port, int) or isinstance(port, bool) or not 1 <= port <= 65535:
        raise ValueError("invalid port")
    peer = v["discovery_peer"].encode("ascii")
    if not 1 <= len(peer) <= 512 or not peer.startswith(b"udp://") or any(c < 33 or c > 126 for c in peer):
        raise ValueError("invalid peer bytes")
    return b"".join((label("VPLUS-ENROLL-POP-V1"), b"\x01", sha(token), host_cert,
                     bootstrap_id, challenge, host_public, client_public, client_cert, address, client_address,
                     struct.pack(">HH", port, len(peer)), peer))


def derive(private, peer_public):
    with tempfile.TemporaryDirectory(prefix="enrollment-v1-public-test-") as tmp:
        private_file, public_file = Path(tmp) / "key.der", Path(tmp) / "peer.der"
        private_file.write_bytes(PKCS8_PREFIX + private)
        public_file.write_bytes(SPKI_PREFIX + peer_public)
        return subprocess.run(
            ["openssl", "pkeyutl", "-derive", "-inkey", str(private_file), "-keyform", "DER",
             "-peerkey", str(public_file), "-peerform", "DER"],
            capture_output=True, check=True, timeout=10).stdout


def main():
    v = json.loads(VECTOR.read_text(encoding="utf-8-sig"))
    node = subprocess.run(["node", str(GENERATOR)], capture_output=True, text=True,
                          encoding="utf-8", check=True, timeout=10)
    if json.loads(node.stdout) != v:
        raise AssertionError("Node generator and checked-in fixture differ")
    token = unhex(v, "token", 32)
    host_priv, client_priv = unhex(v, "host_x25519_private", 32), unhex(v, "client_x25519_private", 32)
    host_pub, client_pub = unhex(v, "host_x25519_public", 32), unhex(v, "client_x25519_public", 32)
    shared = derive(host_priv, client_pub)
    if shared != derive(client_priv, host_pub) or shared == bytes(32):
        raise AssertionError("invalid X25519 shared secret")
    try:
        low_order = derive(host_priv, bytes(32))
    except subprocess.CalledProcessError:
        pass  # OpenSSL rejects this public key before returning a shared value.
    else:
        if low_order == bytes(32):
            pass  # Protocol implementation must reject this result.
        else:
            raise AssertionError("unexpected result for all-zero X25519 public key")
    t = transcript(v)
    salt = sha(label("VPLUS-ENROLL-HKDF-SALT-V1") + t)
    prk = mac(salt, shared)
    key = mac(prk, label("VPLUS-ENROLL-POP-KEY-V1") + b"\x01")
    proof = mac(key, label("VPLUS-ENROLL-POP-PROOF-V1") + t)
    confirmation = sha(label("VPLUS-ENROLL-SAS-V1") + t + proof)
    boot_prk = mac(unhex(v, "bootstrap_id", 16), token)
    boot_secret = mac(boot_prk, label("VPLUS-ENROLL-BOOTSTRAP-SECRET-V1") + b"\x01")
    actual = {
        "token_sha256": sha(token).hex(), "x25519_shared": shared.hex(), "transcript": t.hex(),
        "hkdf_salt": salt.hex(), "hkdf_prk": prk.hex(), "proof_key": key.hex(), "proof": proof.hex(),
        "confirmation_sha256": confirmation.hex(),
        "confirmation_code": "-".join(confirmation.hex()[i:i+4] for i in range(0, 16, 4)),
        "bootstrap_prk": boot_prk.hex(), "bootstrap_secret": boot_secret.hex(),
    }
    for name, value in actual.items():
        if v[name] != value:
            raise AssertionError(f"mismatched {name}")
    for field in ("token", "host_certificate_der_sha256", "bootstrap_id", "challenge", "host_x25519_public", "client_x25519_public", "client_certificate_der_sha256"):
        altered = dict(v)
        raw = bytearray.fromhex(v[field]); raw[0] ^= 1
        altered[field] = raw.hex()
        changed = transcript(altered)
        if changed == t:
            raise AssertionError(f"unbound field {field}")
        changed_salt = sha(label("VPLUS-ENROLL-HKDF-SALT-V1") + changed)
        changed_key = mac(mac(changed_salt, shared), label("VPLUS-ENROLL-POP-KEY-V1") + b"\x01")
        if hmac.compare_digest(mac(changed_key, label("VPLUS-ENROLL-POP-PROOF-V1") + changed), proof):
            raise AssertionError(f"proof did not bind {field}")
    for field, replacement in (("host_ipv4", "100.64.10.3"), ("client_ipv4", "100.64.10.4"), ("enrollment_port", 47990),
                               ("discovery_peer", "udp://elsewhere.example:11010")):
        altered = dict(v); altered[field] = replacement
        if transcript(altered) == t:
            raise AssertionError(f"unbound field {field}")
    print("PASS: Node vector, Python transcript/HKDF/HMAC, OpenSSL X25519, and field binding")


if __name__ == "__main__":
    main()
