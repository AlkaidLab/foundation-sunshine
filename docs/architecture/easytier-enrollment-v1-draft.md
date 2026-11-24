# Accountless EasyTier enrollment v1 — protocol draft

**Draft only. No production endpoint, QR format, transport, or credentials
are implemented by this document.** The existing long-lived-secret remote
invite is disabled. This proposal needs independent security review, complete
strict-direct transport enforcement, transaction recovery, platform tests, and
the integration tests below before it can be enabled. Do not use its public
test vectors as real credentials.

The goal is to bind a phone-generated EasyTier X25519 key and a GameStream
client certificate to the intended Sunshine host without creating an account.
It uses a short-lived bootstrap network, pinned host TLS, proof of possession,
and a separate human confirmation. A copied QR code is a bearer credential:
X25519 proof alone does **not** establish that its holder is the intended phone.
There is no claim that a direct-peer API rules out an external proxy.

## Security prerequisites

- A QR is displayed only in an authenticated host administration session.
  Its certificate hash is SHA-256 of the complete DER-encoded leaf certificate
  used by the enrollment TLS server. The phone must compare that exact hash in
  the TLS trust decision and abort on mismatch, missing certificate, or TLS
  failure; it must never retry without the pin. Verification of the TLS
  handshake proves possession of that certificate's private key. The host must
  ensure that certificate is the identity expected for later GameStream
  pairing, and certificate rotation must invalidate old invitations.
- An independently verified strict-direct transport must cover **both** send
  and receive paths, authenticate the expected peer/connection, reject route
  changes and unknown paths, and handle network loss by failing closed. Until
  it exists, neither enrollment nor a media no-relay claim may be enabled.
  Discovery servers can see IP addresses and control metadata.
- Run a separate no-TUN bootstrap instance with a separate random network,
  default-deny ACLs, relay forwarding disabled, and only one enrollment TCP
  service accessible. That service binds to host loopback; all other host
  TCP/UDP services, exit nodes, and subnet proxies remain inaccessible. The
  existing ACL test is local only; per-device ACLs and real network isolation
  remain unproven. Remote management RPC must not be exposed.
- All tokens and private keys must be absent from URLs sent to servers, logs,
  command-line arguments, analytics, crash reports and backups. The QR itself
  necessarily contains its one-time token; handle images, clipboard, and UI
  history as sensitive. The permanent EasyTier network secret is never in the
  QR or sent to the phone.

## Inputs and encoding

Protocol version is the single byte `0x01`. Random bytes use a CSPRNG. The
host generates a 32-byte invitation token, 16-byte bootstrap ID, 32-byte
challenge and ephemeral X25519 key pair. The phone generates its own X25519
key pair and uses the exact DER certificate for its GameStream identity. The
QR contains the token, bootstrap ID, host certificate hash, two bootstrap
IPv4 addresses, enrollment port, discovery peer and an expiry **display**
value. The host challenge and host ephemeral public key arrive only after the
phone has established pinned TLS.

All fixed byte strings in this section have exactly the specified lengths.
`label(s)` is the US-ASCII bytes of `s` followed by one `0x00` byte. No fields
are implicit, text-normalized, omitted, or reordered. Integer fields are
unsigned big-endian. IPv4 fields are the four network-order octets, not their
text representation. The peer is the exact US-ASCII byte sequence decoded
from the QR, 1–512 bytes, each in `0x21..0x7e`, beginning with `udp://`.
Validate its URI host and nonzero port separately, and do not normalize the
bytes used below. The enrollment port is 1–65535. QR parsing must reject
duplicates, unknown versions, malformed/overlong fields and unsupported
addresses; display time is not an authorization check.

The exact proof transcript `T` is the concatenation of:

| Order | Field | Bytes |
| --- | --- | ---: |
| 1 | `label("VPLUS-ENROLL-POP-V1")` | 20 |
| 2 | protocol version | 1 |
| 3 | `SHA256(invitation_token)` | 32 |
| 4 | host TLS certificate DER SHA-256 | 32 |
| 5 | bootstrap ID | 16 |
| 6 | host challenge | 32 |
| 7 | host ephemeral X25519 public key | 32 |
| 8 | phone X25519 public key | 32 |
| 9 | phone GameStream client certificate DER SHA-256 | 32 |
| 10 | host bootstrap IPv4 | 4 |
| 11 | phone bootstrap IPv4 | 4 |
| 12 | enrollment TCP port | 2 |
| 13 | peer byte length | 2 |
| 14 | discovery peer | length in field 13 |

Thus `len(T) = 241 + len(peer)`. The phone and host compute:

```text
S    = X25519(phone_private, host_ephemeral_public)
     = X25519(host_ephemeral_private, phone_public)
salt = SHA256(label("VPLUS-ENROLL-HKDF-SALT-V1") || T)
PRK  = HMAC-SHA256(key=salt, message=S)
K    = HMAC-SHA256(key=PRK, message=label("VPLUS-ENROLL-POP-KEY-V1") || 0x01)
P    = HMAC-SHA256(key=K, message=label("VPLUS-ENROLL-POP-PROOF-V1") || T)
C    = SHA256(label("VPLUS-ENROLL-SAS-V1") || T || P)
```

This is HKDF-SHA-256 extract plus one expand block for the 32-byte `K`.
Reject X25519 errors, low-order inputs yielding an all-zero shared secret,
and malformed or wrong-length inputs. Compare `P` in constant time. `P`
proves possession of the EasyTier private key, **not** of the GameStream
certificate private key, and is not an authorization decision.

The proposed bootstrap network name is lowercase hex of the 16-byte ID. Its
secret is lowercase hex of the following 32-byte value:

```text
bootstrap_PRK    = HMAC-SHA256(key=bootstrap_id, message=invitation_token)
bootstrap_secret = HMAC-SHA256(key=bootstrap_PRK,
                    message=label("VPLUS-ENROLL-BOOTSTRAP-SECRET-V1") || 0x01)
```

This is a separate HKDF-SHA-256 domain from `K`; it is never the permanent
network secret. Bootstrap configuration limits and encoding must be verified
against the pinned EasyTier version before use. All requests carrying the
token or proof use an encrypted request body, never a URL/query parameter.

## Challenge, approval and lifecycle

1. Host issues one random invitation under its synchronized invitation state
   with a maximum lifetime of 120 seconds measured by host monotonic time.
   Replacing, cancelling, or restarting invalidates it and stops bootstrap
   access. The QR's wall-clock expiry is informational only.
2. Phone checks the pinned TLS certificate, submits the token in the encrypted
   body, and receives a fresh host ephemeral public key and challenge. The
   server associates that challenge with this invitation and TLS connection,
   permits at most one active challenge and bounds it to the lesser of 30
   seconds and remaining invitation lifetime. The server burns the challenge
   before processing its first proof; reuse, connection replacement, timeout,
   unknown invitation or malformed input fails closed. Implement limits for
   attempts and simultaneous connections so copied QRs cannot cause unbounded
   work; a copied QR can still cause denial of service.
3. Phone returns its X25519 public key, GameStream certificate hash, and `P`.
   Host checks the transcript and proof and creates a single pending candidate
   for the current invitation. It must not add the key to the permanent
   credential store or permit streaming yet.
4. Both sides display `C[0:8]` as 16 lowercase hex characters grouped `4-4-4-4`.
   The owner compares **all** groups on the phone and authenticated host UI,
   then explicitly approves on the host. A name or first claimant is not
   sufficient. Approval is bound to this exact invitation, candidate and
   transcript; mismatches, rejected approval or timeout invalidate it. This
   reduces copied-QR substitution only if the owner checks both displays; it
   does not prevent denial of service, device compromise, or a user approving
   an attacker. Rate-limit failed attempts and prohibit silent reapproval.
5. Under a single serialized transaction, verify the candidate is still
   approved and unexpired, consume the token exactly once and bind the client
   key and certificate hash. A cancellation that wins before the transition
   to committing aborts it. Once committing starts, cancellation must report
   that it is too late rather than claim success; it may schedule an explicit
   revocation after commit. The current `enrollment_token_store_t` is only a
   single-use primitive: it does not implement this transaction or revoke a
   claim already returned to its caller.
6. Persist only the client's EasyTier public credential in a Windows
   access-controlled store. Make a durable transaction/journal and atomic file
   replacement, then apply it to the exact audited EasyTier build. If core
   restart or activation fails, roll back safely; after a crash, reconcile pending
   state before enabling access. Do not acknowledge durable success until the
   active core and stored state agree. The current upstream credential file
   format is an unstable internal interface and may require restart.
7. Complete the normal GameStream pairing flow. Before any media/input
   authorization, bind the certificate actually proven by that flow to field
   9 and verify the paired Sunshine server certificate matches field 4. The
   provisional network credential must not grant video, input, or unrelated
   services in the meantime. Such per-device staging ACLs and integration with
   the current GameStream PIN flow are **not yet demonstrated**. On any
   mismatch or failure, revoke the provisional credential and close access.

The existing four-digit QR PIN is a GameStream compatibility value, not the
host identity proof or the bearer invitation token. Current Sunshine code
consumes a global preset PIN for PC/LAN classifications, and neither that
consumption nor legacy cancellation is tied to an invitation generation. The
protocol above cannot be activated until generation-aware coordination and
the actual bootstrap-source classification have been implemented and tested.

An isolated in-memory `enrollment_session_t` experiment consumes the token
earlier than step 5: immediately after a valid proof, before owner approval.
It holds the token store privately under the session lock and exposes only a
confirmed-but-not-authorized state, with no credential or commit API. This
fails closed but lets a copied QR and valid proof consume the invitation and
cause denial of service before approval. A future transactional protocol must
review and reconcile this behavior with steps 3–6 before any endpoint uses it.
The experiment checks deadlines on operations; future integration must also
schedule teardown so an idle expired challenge's ephemeral private key is
erased promptly. The experiment does not authenticate the caller-provided local
connection ID or provide the required TLS, administrator identity, durable
storage, revocation, or EasyTier activation.
Its numeric invitation generation is unique only within the current object;
any future network API needs a separate unpredictable server/session epoch so
a stale administrative request cannot match a new invitation after restart.

## Android key storage and scope

VPlus currently has `minSdk 22`. The Android Keystore `KeyGenParameterSpec`
used for AES wrapping is available from API 23, so the first safe implementation
must fail closed for secure remote enrollment on API 22. Other app features
may continue. An API 22 RSA wrapping alternative needs its own design and
device tests; it must not silently fall back to plaintext. Generate X25519
material using a vetted implementation, encrypt it at rest with an
Android-Keystore-protected key, exclude ciphertext and secrets from cloud and
device-transfer backups, and clear temporary byte buffers where possible.
Because the existing JNI accepts TOML, private bytes will still exist in the
app/JNI memory during use; this is not a hardware-held X25519 operation.
Existing VPlus GameStream RSA identity uses app-private `client.key`, not this
Keystore design. The VPlus working tree now excludes that key, its certificate
and the `config-sync` snapshot directory from cloud backup and device transfer;
existing cloud backups are not automatically deleted. Migration, recovery and
real backup/restore behavior still require separate testing.

## Reproducible test vector

`tests/fixtures/remote-connect/enrollment-v1.json` contains one public,
non-production vector with RFC 7748 X25519 test keys and synthetic certificate
hashes. From the repository root:

```text
node tools/easytier-enrollment-v1/generate_vectors.mjs
python tools/easytier-enrollment-v1/verify_vectors.py
```

The verifier compares Node's generated JSON with the checked-in fixture,
independently computes the transcript and HKDF/HMAC with Python's standard
library, checks X25519 in both directions using local OpenSSL, and tests field
binding. This is not an Android/OpenSSL interoperability test or a TLS test.
Before implementation, add shared-vector tests to the actual Kotlin and C++
implementations; test low-order keys, malformed framing, exact time boundaries,
replays, simultaneous claim/cancel/approve, duplicated QR fields, wrong TLS
pin, wrong GameStream certificate, crash/rollback, revocation, and actual
mobile-network traffic with packet-path evidence and no VPN.
