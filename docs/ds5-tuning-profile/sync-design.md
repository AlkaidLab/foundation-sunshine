# ALK-DS5T/1 tuning sync over the 0x5516 control channel

Status: design only.  This branch does not implement the control channel, the
message codec, or cross-host persistence.  Keys 102-104 stay unavailable until
the channel lands and this design is accepted.

## Message set

The tuning payload travels as one opaque `0x5516` control message (Platform
session-control message type 17, attribute 21).  It is negotiated in
`HELLO`/`WELCOME` with the tuning feature bit; a peer that does not advertise
the bit is never sent a tuning message and reports local-only values instead
of pretending to sync.  The feedback extension uses `0x5515` for its output
fields; the two messages never share a body.

The body is the fixed 48-byte header plus bounded entries from design section
16.6: `version=1`, `op` (HELLO, PATCH, SNAPSHOT, RESET, ACK, NACK), `scope`,
`layer`, `device_index`, `flags`, `entry_count`, `revision`, `base_revision`,
`controller_uid`, `app_id_hash`, `part_index`, `part_count`, `result_code`,
`unknown_key_count`, then `u16 key_id`, `u8 type`, `u8 value_len`, value.
Values are little-endian; the envelope remains the existing version-1 TLV in
network byte order.  A message is at most 2048 bytes and 128 entries; larger
profiles use `MORE`/`LAST` fragments with independent validation.

## Layer merge

Both ends keep the layer order from section 16.3: CORE_DEFAULT,
HOST_POLICY, HOST_PER_CLIENT, CLIENT_GLOBAL, CLIENT_PER_HOST, APP_PROFILE,
CONTROLLER_PROFILE, SESSION_OVERRIDE.  A layer is applied only when its scope,
authority and identity hash match; repeated keys in one message reject the
whole message.  `LOCKED` stops the fold.  Missing entries inherit; stale
`revision` values do not overwrite a newer layer.

## Keys 102-104

- `ds5.sync.allowHostOverrides` (client-owned, default false) gates Host
  writes to client-owned keys.  It is checked on the client before merging a
  Host PATCH; a refused write returns `NOT_AUTHORIZED` and leaves the local
  value unchanged.
- `ds5.sync.allowClientOverrides` (host-owned, default false) gates Client
  writes to host-owned keys.  It is checked on the host before merging a
  Client PATCH, with the same fail-closed result.
- `ds5.sync.maxRateHz` (host-owned, default 20 Hz) coalesces tuning messages
  by `(scope, layer, controller_uid, app_id_hash)`: the newest value per key
  wins in one window.  `URGENT` bypasses the window once for a safety or
  release action.  The rate limit never delays input or feedback traffic.

Overrides do not let a peer write into the other peer's layer space.  A Host
write still lands in a Host layer, a Client write still lands in a Client
layer, and `SUGGESTION` values never enter the effective value.

## State machine

1. Negotiate the feature bit.  Without it: local layers only, UI shows the
   other side as unsupported.
2. Send `HELLO` with the current layer revisions.  A peer answers with its
   accepted scope/layer set or `UNSUPPORTED_VERSION`.
3. Send `PATCH` or `SNAPSHOT` with `ACK_REQUESTED`.  The receiver validates,
   merges, clamps to descriptor ranges and device limits, applies locally,
   then answers `ACK` with the effective revision and applied count, or
   `NACK` with `OUT_OF_RANGE`, `TYPE_MISMATCH`, `STALE_REVISION`,
   `UNAVAILABLE`, `NOT_AUTHORIZED`, `UNSUPPORTED_VERSION`, `BUSY` or
   `MALFORMED`.
4. A rejected message changes nothing.  The sender keeps the previous
   effective value, marks the item `not synced`, and retries only after a new
   revision or an explicit user action.
5. Session end discards `SESSION_OVERRIDE`, restores the persistent layers,
   clears rumble and adaptive triggers, and applies the `releaseOnEnd` light
   behaviour.  A dropped link does not delete persisted layers.

## Host integration points

The host needs a tuning channel owner that (a) exposes the feature bit during
session negotiation, (b) owns the per-layer revision map, (c) calls the same
`store_t` validation and runtime publication used by the local settings API,
and (d) enforces keys 102-104 before the merge.  Input and feedback report
threads keep reading only the existing atomics; message decode, merge and
persistence stay on the control thread.  No new allocation, lock or syscall
is added to the report path.

## Verification required before enabling keys 102-104

- codec round-trip, truncation, oversized entry, duplicate key, unknown key,
  non-finite float, bad type/length and fragment-boundary tests;
- revision ordering and stale-write rejection across two stores;
- override-gate denial for each direction, with the value unchanged;
- rate-cap coalescing and one-shot `URGENT` bypass;
- disconnect during a PATCH, restart with persisted layers, and a peer that
  never negotiated the feature bit.
