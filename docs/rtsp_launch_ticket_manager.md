# RTSP launch ticket manager

Sunshine separates the HTTPS launch transaction from the later RTSP setup.
The launch ticket manager is the single owner of state between those two
protocol phases.

## Lifecycle

```text
/launch or /resume
        |
        v
     pending  -- first RTSP request authenticated --> claimed
        ^                                         |
        |---- request socket closes before ANNOUNCE|
                                                  v
                           successful ANNOUNCE --> active stream
                                                  (ticket removed)
```

Both `pending` and `claimed` tickets expire. A claimed ticket is released back
to `pending` when an RTSP request socket closes before the stream is activated.
This is required because GameStream uses a separate TCP connection for each
RTSP request.

## Identity and compatibility

- Encrypted RTSP is claimed by successfully verifying the first request's
  AES-GCM authentication tag. The HTTPS and RTSP source addresses are only a
  candidate-order hint and are not identity.
- Plaintext legacy RTSP is matched by a unique observed source address.
- The historical single-pending-ticket fallback remains for legacy clients
  whose HTTPS and RTSP routes differ.
- Multiple plaintext candidates behind the same visible address fail closed;
  Sunshine never guesses and risks attaching one client's keys to another.

## Transaction invariant

`/launch` and `/resume` return success only after a ticket has been accepted by
the bounded registry. If registration fails, the endpoint returns an explicit
busy/capacity response. An application started solely for a failed `/launch`
transaction is rolled back.

## Resource bounds

The registry has a fixed global pending-ticket limit and a per-client/legacy
peer limit. Ticket expiration bounds registry lifetime, while each accepted
RTSP socket has an initial-handshake deadline to prevent idle connections from
holding resources indefinitely.

## Operational signal

The local sessions API reports `pending_sessions` separately from active stream
sessions. A healthy launch normally moves from one pending ticket to an active
session within one RTSP handshake interval.
