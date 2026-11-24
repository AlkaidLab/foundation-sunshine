# Per-device EasyTier credentials on Windows — transaction design draft

**Design and evidence only.** There is no implementation of the credential
store, durable transaction, core readiness check or crash recovery described here.
The isolated invitation state machine does not authorize any device and has
no credential-installation API. No existing credential, network or Windows ACL
was changed while preparing this document.

`credential_transaction` is a pure in-memory reducer with fake-store and fake-
core tests for the proposed ordering. It does not persist data, run a core,
enforce a network gate, verify a key or certificate, or authenticate its caller.
Its inputs represent assertions that a future trusted adapter must prove. The
fake tests do not establish Windows filesystem, process or network guarantees.
The reducer currently rejects reuse of either a revoked EasyTier public key or
a revoked GameStream certificate fingerprint, including under a different
device ID. It has no tombstone-retirement flow; re-enrollment therefore needs
a new GameStream identity as well as a new EasyTier key. Supporting reuse of an
old GameStream certificate would require an explicit, reviewed policy and
durable re-pairing transaction rather than silently removing the tombstone.

The intended security boundary is an owner-approved phone public key bound to
the verified GameStream client certificate and intended Sunshine host. It
requires the strict-direct data path, authenticated enrollment and provisional
pairing isolation described in `easytier-enrollment-v1-draft.md`; none is
established merely by writing a credential JSON file.

## Current implementation and constraints

- Windows `platf::appdata()` currently resolves to `config` beside the
  Sunshine executable (`src/platform/windows/misc.cpp`). Sunshine writes
  `remote-connect.toml` there with `file_handler::write_file` and the permanent
  network secret in plaintext (`src/remote_connect/easytier.cpp`). That helper
  truncates and writes through `std::ofstream`; it is not a durable atomic
  transaction. `config::update_config` similarly rewrites `sunshine.conf`
  through the same helper (`src/config.cpp`). Neither is suitable as the
  transaction journal without further work.
- `runtime_t::start` regards a created child process as a successful start;
  `runtime_t::running` checks process liveness, not whether the intended
  credentials, policy or direct-route restrictions loaded. Current code uses
  an independently installed EasyTier executable without binding its bytes to
  an audited version. It disables the unauthenticated management RPC via a
  deny whitelist, and clears inherited `ET_*` overrides.
- In the locally inspected EasyTier 2.6.4 source,
  `peers/credential_manager.rs` loads a credential file at manager creation.
  A missing or malformed file leaves an empty in-memory map and only logs a
  warning; process liveness is therefore not evidence that the intended set
  was loaded. Its management API saves with `std::fs::write`, not this proposed
  transaction. Public-key-only entries worked in local tests, but the JSON is
  an internal, version-specific format rather than a supported stable API.
- A 2.6.4 entry includes base64 X25519 public key, groups, `allow_relay`,
  `allowed_proxy_cidrs`, `reusable`, `expiry_unix` and `created_at_unix`;
  absent `secret` is accepted by that source. `reusable=false` participates
  in duplicate-peer ownership logic; it is **not** a one-time enrollment
  token and does not replace the application state machine. Wall-clock
  credential expiry alone is not a reliable revocation guarantee if the host
  clock is changed. The exact schema and semantics must be verified for the
  pinned runtime, including reconnection and expiry.
- In `peer_conn.rs`, proof of the permanent network secret is considered
  before the per-device trusted-key path. The old remote QR exposed that
  secret. **Adding a public-key list cannot revoke someone who already has the
  old network secret.** A secure migration must explicitly rotate the root
  network identity/secret, or use a reviewed core policy that rejects the
  shared-secret admission path. Either option can disconnect existing devices
  and needs a user-visible migration plan; it must not silently run as a side
  effect of an enrollment attempt.
- Sunshine's `src/confighttp/clients.cpp` unpair paths terminate GameStream
  sessions and remove client certificates, but there is not yet an association
  between those certificates and EasyTier public credentials. Removing a
  GameStream client therefore does not currently revoke an EasyTier key.
- EasyTier is explicitly stopped on the normal `runtime_t::stop` path. The
  inspected launcher does not set Windows `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
  The locally installed Boost.Process v1 Windows group implementation enables
  breakaway. Do not assume the child dies when Sunshine crashes, or that its
  in-memory credential set changes when a file is rewritten.

## Storage boundary and record

Use a dedicated service-owned directory on a supported local filesystem, with
a protected DACL installed **at directory and file creation**. Grant the
actual configured Sunshine service identity the required rights, and restrict
other local users; account for standalone mode explicitly. Administrators,
SYSTEM and other processes under the same service identity are outside this
boundary. An ordinary user-writable portable-install directory must not be
silently treated as a privileged store.

Create a restrictive security descriptor before `CreateFileW(..., CREATE_NEW,
...)`, write to a unique same-directory temporary file, verify ownership,
permissions, parent and final object identities, write fully, and call
`FlushFileBuffers` before a handle-based same-volume replacement. Reject
unexpected reparse points/junctions, hard links, path escapes and concurrent
replacement; validate every relevant parent component, not only the final
name. Parent-directory protection and access-check assumptions require actual
Windows tests. `src/confighttp/local_auth_token.cpp` illustrates creating a
restricted temporary file before writing and flushing it, but is not proof of
all of these path and crash-safety properties. A fixed `.tmp` name or setting
ACLs only after writing introduces avoidable races.

The authoritative versioned record should contain a monotonically increasing
generation, transaction ID/phase, and complete old/new **public** snapshots.
Each device entry binds an internal device ID, X25519 public key, GameStream
certificate DER SHA-256, administrative status/tombstone, and explicitly
validated policy/expiry. Reject malformed, duplicate, excessive, unknown-version
or conflicting records. Do not persist a phone private key, invitation token,
challenge, proof, or comparison code. The version-specific EasyTier JSON is a
derived artifact, not an independent source of truth. A checksum can detect
accidental corruption but is not authentication against an attacker able to
replace the file.

Keep the host root secret separately protected under a documented service
identity and DACL. DPAPI may help protect at-rest bytes, but machine-scope
DPAPI does not by itself isolate users who can read the blob, and EasyTier
must receive usable secret material at runtime. Account for service versus
interactive identity, backup/restore, process inspection by privileged users,
temporary buffers, logging and environment inheritance. Never claim the
private root secret is protected merely because the public-key store is.

Do not claim that a rename or `MOVEFILE_WRITE_THROUGH` alone makes multiple
files transactionally durable on every Windows filesystem. Establish supported
filesystem/volume constraints, retain verifiable old/new records, and test
write, flush, replacement, metadata and simulated power-loss failures. Recovery
must reject ambiguity and leave the remote service closed.

## Addition transaction

The following ordering is a candidate contract, not current behavior:

1. Serialize all per-device mutations with one service transaction lock. Check
   the approved invitation generation, verified proof, exact public key,
   host pin and expected GameStream certificate under that lock. The current
   invitation state machine does **not** provide this transaction handoff.
2. Persist a complete `ADD_PREPARED` record containing both previous stable
   state and the proposed public-only candidate. It is not an authorization
   acknowledgement. Keep access closed on any write or verification failure.
3. Before replacing any credentials, close the affected business-data gate;
   stop the old core and confirm its job is empty. A proposed hardened launcher
   needs non-breakaway containment and kill-on-job-close, with verified child
   image and no inherited management access. Test host crash and orphan
   scenarios rather than relying on a PID or normal destructor.
4. Derive the candidate-version EasyTier file from the prepared snapshot,
   atomically publish it and start the exact trusted core. Keep media/input
   disabled. GameStream pairing, if needed before commit, must be confined to
   the exact candidate and pairing-only service. The current ACL/forwarding
   code has **not** proven such per-device provisional isolation. If that gate
   cannot be enforced, do not execute this transaction.
5. Verify readiness through a reviewed local, authenticated channel bound to
   the launched process, including the expected configuration/credential
   generation, strict-direct policy and the intended certificate identity.
   The channel must not re-enable unauthenticated EasyTier RPC. Complete and
   verify GameStream certificate proof and host pin. A reported PID, liveness,
   log line or port-open check is insufficient.
6. Persist and verify a stable committed record for the new generation; only
   then open the business-data gate and report success. An activation failure
   before commit requires stopping the candidate core, restoring the prior
   derived file, restarting and verifying the old core. If safe rollback or
   readiness cannot be established, remain stopped and report failure. The
   cancellation point is defined before transition to committing; later cancel
   must report too late or invoke a separate revocation transaction.

On startup, inspect the record **before launching or exposing any core**. An
uncommitted addition is rolled back to the old stable snapshot; an ambiguous,
corrupt or unsupported record leaves the service disabled. This requires a
verified guarantee that a candidate core from the previous process cannot
survive a crash or keep serving while rollback runs. A crash after stable commit
but before acknowledgement must return a recoverable, idempotent result tied
to the transaction ID, rather than duplicating the device.

## Revocation is different

Revocation must not restore the removed key merely to preserve availability:

1. Close the target's business-data gate and terminate its active GameStream
   sessions. If the target cannot be isolated reliably, stop the whole core.
   Persist a `REVOKE_PENDING` tombstone and target identity before acknowledging
   revocation; if writing fails, do not report success.
2. Stop the old core and confirm it exited. Publish a derived credential set
   excluding that public key and start only the reviewed replacement core.
   Verify the removed key cannot connect, existing authorized keys have the
   intended access, and no old child or stream still serves traffic.
3. Mark the tombstone stable and acknowledge only after enforcement is known.
   If restart, verification or rollback machinery fails, retain the durable
   revocation intent and remain stopped. On crash, startup must apply the
   tombstone before any core starts; it must **never** fall back to an older
   snapshot that reauthorizes the revoked device. Other devices may be
   interrupted until recovery completes.

The same semantics apply when a GameStream device is removed or all devices
are unpaired. This requires a stable identity association and coordinated
session shutdown across both subsystems; it is not implemented today.

## Smallest safe implementation and verification slices

1. Implement a pure in-memory transaction reducer with typed records and a
   fake persistence/core adapter. No filesystem, ACL, network listener or
   real credential changes. Test concurrent mutation, stale generation,
   compare-and-swap, cancellation at each phase, crash at every transition,
   corrupt records, idempotent restart, and that failed revocation can never
   select a snapshot containing the revoked key. Its results are not an
   activated device.
2. Add a separately tested deterministic serializer/parser for bounded,
   public-only snapshots and the pinned 2.6.4 format. Test unknown/missing
   fields, duplicate keys/device IDs, malformed base64 and X25519 keys, bounds,
   relay/proxy denial, timestamp edge cases, and round trips. Verify the real
   core's documented or source-pinned behavior with disposable credentials;
   do not assume an internal JSON representation is a stable API.
3. In an isolated self-created Windows test directory only, implement and
   fault-inject the secure-file adapter: restrictive descriptor at creation,
   reparse/hard-link attacks, denied access, partial writes, flush/replacement
   failures and restart recovery. Test a separate unprivileged Windows account
   and service/interactive identity, and verify cleanup only within that test
   directory. No production ACL or credential is changed by these tests.
4. Implement and test child containment and a trusted readiness contract with
   a disposable fake core; deliberately crash the host and verify no orphan
   serves old or candidate credentials. Then test pinned EasyTier binaries in
   an isolated network, including failed activation, revocation, reboot, and
   old-secret rejection. Hardware/power-loss and real mobile-path tests remain
   separate gates.

Only after these slices, authenticated enrollment, per-device provisional
isolation, and strict no-relay transport are independently validated may the
transaction be wired to product endpoints. Do not expose the candidate as
"paired" or "authorized" before the durable record, active core, and
GameStream identity all agree.
