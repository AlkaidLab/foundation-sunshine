# Pending pairing requests

Status: implemented in src/nvhttp/pairing.cpp and
src/confighttp/pairing.cpp.

## Purpose

Pairing is rare, but when a device is waiting for its PIN the host UI must
come forward immediately. The host already keeps the pairing session and its
PIN state in src/nvhttp/pairing.cpp; this API exposes only the fact that a
request is waiting, with enough information to show a device name and a
deadline. It never exposes the client certificate, the client-supplied
uniqueid, or the source address.

The endpoint is a long poll by default. A panel can wait on one request and
react within a second without a short polling interval.

## Authentication

Both routes use the existing Web UI authentication
(confighttp::authenticate):

- the configured Basic-auth credentials for non-local requests;
- the existing localhost/partner policy;
- the existing Origin, Referer and Sec-Fetch-Site checks for browser
  requests.

An unauthenticated first-run request is redirected to /welcome. A cross-site
browser request is rejected with 403 Forbidden.

## GET /api/pairing/pending

Returns the current pending set.

Query parameters:

| name | value | meaning |
|---|---|---|
| wait | 0..25 seconds | Long-poll window. Absent or 0 returns immediately. Values above 25 are capped at 25. |
| since | unsigned integer | The generation from a previous response. When it equals the current generation, the request waits for the next change. |

Response:

    {
      "pending": true,
      "generation": 42,
      "requests": [
        {
          "id": "6f1c9e44-4f4f-4f5e-9a4c-0f7f8e1d2a3b",
          "device_name": "Living Room",
          "started_at": 1793750000,
          "expires_at": 1793750300
        }
      ]
    }

Fields:

- pending: true when requests is non-empty.
- generation: an additive cursor for long polling. It changes when a
  request is added, resolved, rejected or expires. Clients that omit since
  can still use the endpoint; the cursor lets subsequent calls wait for the
  next change instead of returning the same snapshot repeatedly.
- id: an opaque server-generated UUID. It is deliberately independent of
  the client uniqueid and of the certificate hash. Use it for
  DELETE /api/pairing/pending/{id} and for the optional id on the PIN
  request.
- device_name: the pair request's clientname, sanitised to valid UTF-8,
  with C0/C1 controls and bidi controls removed, whitespace collapsed, at most
  64 bytes, and "Unknown device" when the result is empty. Render it as text;
  do not inject it as HTML.
- started_at, expires_at: Unix time in seconds. The default lifetime is
  the existing five-minute pending-PIN session timeout.

Long-poll rules:

1. No wait, or wait=0: return the current snapshot immediately.
2. wait=N with since equal to the current generation: wait up to N
   seconds for a change, then return the latest snapshot.
3. wait=N with a different since: return immediately; the client is
   behind and already has a newer snapshot to render.
4. wait=N without since and with pending requests: return immediately so
   the first paint never waits for a change.
5. wait=N without since and with no pending requests: wait for the first
   request or the timeout.

The response is always 200 OK with Cache-Control: no-store. Waiters are
bounded to eight process-wide and two per source address; when the bound is
reached the handler returns the current snapshot immediately rather than
creating an unbounded thread.

Typical loop:

    let since = 0;
    for (;;) {
      const query = since
        ? '?wait=25&since=' + since
        : '?wait=25';
      const response = await fetch('/api/pairing/pending' + query, {
        credentials: 'same-origin',
      });
      const state = await response.json();
      since = state.generation;
      renderPending(state.requests);
    }

## DELETE /api/pairing/pending/{id}

Rejects or cancels one pending request. The waiting pairing client receives
the GameStream XML response with status_code=403 and paired=0, and the
session is removed.

Successful response:

    { "status": true }

An unknown or already expired id returns 404 Not Found with a JSON error.
The id is restricted to 1-64 characters of [A-Za-z0-9_-] by the route.

## POST /api/pairing/pin

This is the same contract as the existing POST /api/pin, with one optional
field:

    {
      "pin": "1234",
      "name": "Living Room",
      "id": "6f1c9e44-4f4f-4f5e-9a4c-0f7f8e1d2a3b"
    }

- pin: 4-8 digits, as before.
- name: 1-256 characters, as before.
- id: optional; 1-64 characters of [A-Za-z0-9_-]. When present, the PIN
  is applied to that pending request. When absent, the oldest pending request
  is selected, preserving the existing /api/pin behaviour.

The response keeps the existing shape: {"status": true|false} plus an
error string when the request is rejected. A missing or expired id yields
status=false; it does not change the HTTP status of the legacy endpoint.

POST /api/pin remains unchanged and accepts the same optional id.

## Limits and failure behaviour

- Pending entries expire with the existing five-minute session timeout. Both
  the delayed task and the next snapshot purge send 408 to the waiting
  client and remove the session.
- At most four pending entries per source address and eight total are kept.
  New pairing requests beyond the limit receive the GameStream XML 429
  response; the API itself does not create entries.
- At most twelve pending entries are created per source address in a rolling
  60-second window.
- A second getservercert with the same client uniqueid receives 409
  instead of replacing the waiting session.
- Logs contain only a short request-id prefix, the device-name byte length and
  the source address. Client certificate bytes are not logged.

## Compatibility and adapters

The GameStream pairing protocol is unchanged. The new routes are a host-side
management API and do not change the wire protocol or the Moonlight client
behaviour. This capability is not a transport or a codec; an Adapter for a
different external protocol would translate that protocol's pending-pairing
state into this shape, but no such Adapter is implemented here.

## Tests

tests/unit/test_pairing_pending.cpp is built as
pairing_pending_unit_tests and covers:

- listing, opaque ids, sanitisation, timestamps and PIN-to-chosen-request;
- expiry with the session timeout and the waiting client's 408;
- multiple simultaneous requests and rejection with 403;
- the legacy no-id PIN selecting the oldest request;
- the long poll returning early on change and honouring its timeout;
- the per-source pending limit;
- authentication and cross-site rejection;
- validation of the optional id and the legacy PIN/name limits.

The focused suite links the pairing registry, the route module and their
direct dependencies, so it does not pull the full host graph.

## Sources

| key | source | sections used |
|---|---|---|
| ds5-rfc9110-http-20261031 | RFC 9110, HTTP Semantics | 9.3.1 (GET), 9.3.5 (DELETE), 15.5.4 (403), 15.5.5 (404), 15.5.9 (408), 15.5.10 (409) |
| pair-rfc9111-caching-20261103 | RFC 9111, HTTP Caching | 5.2.2.5 (no-store) |
| pair-rfc6585-status-20261103 | RFC 6585, Additional HTTP Status Codes | 4 (429 Too Many Requests) |
| ds5-rfc8259-json-20261031 | RFC 8259, JSON | 1, 2 and 4 (JSON framing) |
| pair-rfc3629-utf8-20261103 | RFC 3629, UTF-8 | 4 (UTF-8 byte sequences) |
| pair-uax44-unicode-20261103 | Unicode UAX #44 | 5.7.1 (General_Category, Cc/Cf) |
| pair-msvc-condition-variable-20261103 | Microsoft Learn, condition_variable class | class members, wait_for, notify_all |
| pair-libstdcxx-concurrency-20261103 | libstdc++ manual, concurrency | thread-safety and locking requirements |

## Performance

There is no hot path in a streaming sense. A snapshot walks at most eight
sessions, and each pending client costs one session object plus at most one
long-poll thread. The long-poll cap (eight total, two per address) bounds
threads and memory; a waiter that cannot get a slot receives the current
snapshot immediately. Building the response does one JSON serialisation and
one HTTP write per request. No polling interval is required, so an idle host
does not generate periodic requests.
