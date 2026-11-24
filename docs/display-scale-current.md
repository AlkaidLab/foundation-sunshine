# Current streaming display scale

Adjust the display captured by the caller's active video session without a monitor picker. The session is identified by the paired GameStream HTTPS client certificate (TLS), never by `uniqueid`.

## GET

`GET /display-scale-options?target=current`

Success includes the existing scale fields plus `target: "current"`, the captured `display_name`, and its `device_id`.

## POST

`POST /display-scale`

```json
{
  "target": "current",
  "scale_percent": 150
}
```

The server resolves the captured display at request time and applies the scale by its `device_id`. The response reports which display was changed (`display_name`, `device_id`), previous/current scale, supported steps, `effective_immediately`, and `restart_required`.

## Errors

HTTP 409 with `error_code`:

- `no_active_stream`: this client has no running video session.
- `display_target_unavailable`: capture is still initializing, uses window capture (no fixed monitor), or the display cannot be resolved. Retry later.

An invalid `target` value returns HTTP 400 `invalid_request`. `target=current` never falls back to the primary display or another client's session. Non-Windows hosts return HTTP 501 `unsupported_platform`.
