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
- `actions-v1`: action endpoint
- `operations-v1`: asynchronous operation lifecycle in state
- `provider-lease-v1`: optional provider ownership lease
- `notification-ack`: non-pairing notification acknowledgement
- `pairing`: pairing notification with the `open_pin` action
- `vdd`: virtual-display state and actions

The `owner` field describes the packaged tray strategy (`gui`, `core`, or
`disabled`). It is retained for version-1 compatibility. Active runtime
ownership is represented separately by `provider`.

## Actions and operations

`POST /api/tray/action` accepts:

```json
{ "action": "vdd_create", "enabled": true, "notification_id": 12 }
```

Only fields relevant to the selected action are required. Supported actions
are `vdd_create`, `vdd_destroy`, `vdd_toggle_keep_enabled`,
`vdd_toggle_headless_create`, `clear_app`, `reset_display_device_config`,
`restart`, and `notification_ack`.

VDD creation is non-interactive inside Core. A graphical provider must obtain
user confirmation before requesting it. Long-running actions return an
`operation_id`; their current `running`, `succeeded`, or `failed` result is
published in the state `operation` object. Operations execute serially and are
joined during Core shutdown.

## Provider lease

Lease support is optional in protocol version 1 so released GUI versions remain
compatible during migration.

1. `POST /api/tray/provider/register` with `provider_id`, `version`,
   `protocol_version`, and optional provider capabilities.
2. Core returns a private `lease_id` and `lease_duration_ms`. The lease token is
   never included in public state.
3. `POST /api/tray/provider/heartbeat` with `lease_id` before expiry.
4. `DELETE /api/tray/provider/lease` with `lease_id` for best-effort release.
   An uncleanly terminated provider is released automatically after expiry.

Only one distinct provider ID may hold an active lease. Re-registering the
same provider ID replaces its previous lease, which supports provider restart.
The public `provider` state contains only ID, version, and active status. Lease
renewal does not increment the tray state revision or rebuild the provider UI.

During the version-1 compatibility period, legacy clients may still read state
and invoke actions without a lease. A future protocol version may require the
lease for provider-owned actions after all supported packaged clients have
migrated.

## Compatibility

- New provider with old Core: use state polling/SSE and actions; do not attempt
  lease registration unless `provider-lease-v1` is advertised.
- Old provider with new Core: existing state, event, and action behavior remains
  available; no lease is required.
- GUI-only installation: the provider may run independently but reports Core as
  disconnected until a compatible local Core is installed and running.
