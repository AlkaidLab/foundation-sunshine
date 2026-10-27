# Current streaming display scale

Adjust the display captured by the caller's active video session without a monitor picker. Use the paired GameStream HTTPS client certificate; identity comes from TLS, never from `uniqueid`.

## GET

`GET /display-scale-options?target=current`

Success includes the existing scale fields, `target: "current"`, actual `display_name`, and a nonempty canonical `device_id`. Preserve nullable current/recommended percentages and the server-provided supported steps.

## POST

`POST /display-scale`

```json
{
  "target": "current",
  "device_id": "canonical device_id returned by GET",
  "display_name": "display_name returned by GET",
  "scale_percent": 150
}
```

`device_id` is required. `display_name` is optional but must match when sent. Success returns the target identity, previous/current scale, supported steps, `effective_immediately`, and `restart_required`; acceptance may precede effect.

## Errors and target rules

HTTP 409 uses `no_active_stream`, `ambiguous_stream`, `display_target_unavailable`, or `display_target_changed` in `error_code`. Clients close/disable the control, retry when capture is ready, or reload options and ask for a new choice according to the error. Missing or invalid POST identity is HTTP 400 `invalid_request`.

`target=current` never falls back to the primary display, launch-time target, another client's session, or a display list. Window capture has no fixed monitor and is unavailable; non-Windows hosts return HTTP 501 `unsupported_platform`.
