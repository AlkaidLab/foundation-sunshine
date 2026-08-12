# Tray Provider Protocol

Sunshine Core owns stream state and privileged actions. A user-session tray
provider owns native menu presentation, notifications, language, and links into
the desktop GUI. The provider is replaceable and is not required to be built
with Core.

## Transport and authentication

- Protocol version: `1`
- Base URL: the local Sunshine Web UI endpoint, normally
  `https://127.0.0.1:<core-port + 1>`
- Payloads: JSON, except the SSE endpoint
- Access: normal Web UI authentication applies. During first configuration,
  requests from localhost are accepted while no username exists. Remote
  unauthenticated requests are never accepted by this exception.
- A tray provider must not follow a remote host selected by a desktop window.

`GET /api/tray/state` returns the current snapshot. `instance_id` changes when
Core restarts and `revision` increases whenever published state changes.
Consumers must ignore unknown fields and capabilities.

`GET /api/tray/events` is an SSE stream. A `tray-state` event contains the same
JSON object as the state endpoint. Comment frames are keepalives. Providers
should reconnect after disconnect and may perform an infrequent state request
as a connection health check. If `events-v1` is absent, polling the state
endpoint is the compatibility fallback.

## State capabilities

- `state-v1`: state snapshot and stable instance identity
- `events-v1`: SSE state changes
- `sessions-v1`: active client session snapshots with stable IDs and names
- `actions-v1`: action endpoint
- `operations-v1`: asynchronous operation lifecycle in state
- `notification-ack`: non-pairing notification acknowledgement
- `pairing`: pairing notification with the `open_pin` action
- `vdd`: virtual-display state and actions
- `shutdown`: graceful Core and service-wrapper shutdown

The `owner` field describes the packaged tray strategy (`gui`, `core`, or
`disabled`). The packaged GUI uses its existing single-instance guard. Version
1 deliberately does not add a second ownership heartbeat.

When `sessions-v1` is advertised, the state includes a `sessions` array:

```json
{
  "sessions": [
    { "id": 12, "client_name": "Living Room TV" }
  ]
}
```

Providers derive connection and disconnection notifications by comparing
session IDs between consecutive snapshots. The first snapshot and a changed
`instance_id` establish a new baseline and must not produce notifications.
This preserves client names and handles multiple concurrent clients without a
second polling loop.

On Windows, the service wrapper starts the packaged GUI agent in the active
signed-in user's session after Core starts. Failed launches are retried while
Core remains alive, and the GUI's single-instance guard absorbs duplicate
logon and service-start attempts. A zero GUI process exit code suppresses
service-initiated launches for the current Core lifecycle. The wrapper still
performs a low-frequency lookup for an instance started outside the service,
such as an elevated or updater-launched replacement, and resumes normal process
supervision after attaching to it. A new Core lifecycle or active console
session also restores service-initiated launches. Nonzero exits remain crashes
and are restarted with bounded backoff.

The GUI keeps its tray while Core is unavailable. It switches to a distinct
disconnected presentation, disables Core-dependent commands, and keeps local
recovery and diagnostic commands available. On Windows, the disconnected menu
can restart the Sunshine service. Recovery remains pending until the GUI
receives a new accepted Core state response from its monitor. On Windows this
is a local packaged-GUI command, not a Core `/api/tray/action`: the Tauri backend
uses the existing elevated PowerShell service-restart path. Its command result
only confirms that the restart was dispatched; it does not declare Core ready
and is not exposed to remote tray providers.

The 30-second recovery timeout starts after that restart command returns. Only
an accepted `GET /api/tray/state` snapshot or SSE `tray-state` event received
after the attempt began completes recovery. SSE keepalives, connection setup,
error responses, cached state, and tray-action responses do not. Timeout clears
the active attempt and re-enables manual retry; it does not cancel an OS restart
already in progress or stop background Core monitoring.

## Actions and operations

`POST /api/tray/action` accepts:

```json
{ "action": "vdd_create", "enabled": true, "notification_id": 12 }
```

Only fields relevant to the selected action are required. Supported actions
are `vdd_create`, `vdd_destroy`, `vdd_toggle_keep_enabled`,
`vdd_toggle_headless_create`, `clear_app`, `reset_display_device_config`,
`restart`, `shutdown`, and `notification_ack`. While
`vdd.awaiting_confirmation` is true,
the GUI answers with `vdd_confirm_keep`, an `enabled` decision, and the matching
`operation_id`.

VDD creation is non-interactive inside Core. A graphical provider must obtain
user confirmation before requesting it. After creation and topology setup, Core
starts a 20-second confirmation deadline and publishes it through tray state.
The GUI presents the timed keep dialog in the user session. A negative decision
or missing confirmation closes the VDD; Core owns the deadline so rollback still
works if the GUI exits. Matching operation IDs prevent an old timer or dialog
from closing a newer VDD.

Long-running actions return an `operation_id`; their current `running`,
`succeeded`, or `failed` result is published in the state `operation` object.
Operations execute serially and are joined during Core shutdown.

## Signed client fingerprint rules

The packaged GUI may act as a low-frequency transport for warning-only client
fingerprint rules:

- `GET /api/tray/client-fingerprint-rules` returns Core's active rule status.
- `POST /api/tray/client-fingerprint-rules` accepts
  `{ "envelope": "<signed envelope JSON>" }`.

The endpoint is local and uses the same authentication policy as tray actions.
Core treats the provider and request body as untrusted: it independently
verifies the pinned X.509 signature, schema, expiry, monotonic revision, and
size limits before persisting and activating the candidate. The provider must
not interpret rules or make client classification decisions.

## Future providers

Version 1 keeps runtime ownership intentionally simple: the packaged GUI is
single-instance and Core exposes no registration or heartbeat endpoint. If a
second tray implementation creates a real ownership conflict, introduce that
coordination in a later protocol version with enforceable semantics. Consumers
must not infer ownership from undocumented process or port probes.

## Compatibility

- New provider with old Core: use state polling/SSE and actions according to
  advertised capabilities.
- Old provider with new Core: existing state, event, and action behavior remains
  available.
- GUI-only installation: the provider may run independently but reports Core as
  disconnected until a compatible local Core is installed and running.
