# Sunshine Plugin Development

This document describes the Sunshine Plugin Platform v1 contract for external plugin authors.

Plugins are executable programs discovered through a `plugin.json` manifest. Sunshine invokes them for lifecycle events and user-triggered actions, passes a JSON payload file, and optionally reads a JSON result file after the plugin exits.

## Plugin Layout

Installed plugins use one directory per plugin id:

```text
plugins/<plugin-id>/
  plugin.json
  plugin.exe
  config.schema.json
```

Official packaged plugins may also be installed under Sunshine assets:

```text
assets/plugins/<plugin-id>/
  plugin.json
  plugin.exe
  config.schema.json
```

User-installed plugins under the config directory override packaged plugins with the same id.

## Manifest

`plugin.json` is the stable contract between Sunshine, the Web UI, and the plugin executable.

```json
{
  "api_version": 1,
  "id": "com.example.my-plugin",
  "name": "Example Plugin",
  "version": "1.0.0",
  "min_host_version": "2026.0.0",
  "package": {
    "publisher": "Example",
    "channel": "stable",
    "trust": "community"
  },
  "description": "Short human-readable plugin description.",
  "enabled": true,
  "platforms": ["windows"],
  "entry": "example-plugin.exe",
  "slots": [
    "sunshine.startup.recover",
    "stream.first_session.starting",
    "stream.last_session.stopping",
    "sunshine.shutdown.restoring"
  ],
  "capabilities": [
    "stream.lifecycle.read",
    "filesystem.plugin-state.write"
  ],
  "actions": [
    {
      "id": "recover",
      "title": "Run recovery",
      "description": "Perform a safe recovery operation.",
      "event": "sunshine.startup.recover",
      "icon": "fa-history",
      "danger": false
    }
  ],
  "config_schema": "config.schema.json",
  "default_config": {
    "enabled_feature": true
  },
  "timeout_ms": 5000
}
```

Required fields:

- `api_version`: plugin platform contract version. Use `1`.
- `id`: globally unique reverse-DNS style plugin id.
- `name`: display name.
- `version`: plugin version.
- `entry`: executable path relative to the plugin directory.
- At least one of `slots` or `actions`.

Recommended fields:

- `min_host_version`: minimum Sunshine version expected by the plugin.
- `package.publisher`: plugin publisher name.
- `package.channel`: update channel, such as `stable` or `preview`.
- `package.trust`: `official`, `verified`, or `community`.
- `capabilities`: declared capabilities used for review, install prompts, and marketplace safety.
- `config_schema`: JSON Schema file rendered by the Web UI.
- `default_config`: default plugin config object.

Legacy `permissions` may still be present, but new plugins should prefer `capabilities`.

## Lifecycle Slots

Sunshine v1 supports these lifecycle slots:

- `sunshine.startup.recover`: startup recovery before normal service work begins.
- `stream.first_session.starting`: the first non-control-only stream is starting.
- `stream.dynamic_params.changed`: selected stream parameters changed during an active stream. Platform v1 currently emits this for FPS changes.
- `stream.last_session.stopping`: the last non-control-only stream is stopping.
- `sunshine.shutdown.restoring`: final cleanup before Sunshine exits.

Control-only sessions do not fire stream start/stop lifecycle slots.

## Actions

Actions are user-triggered operations rendered by the Web UI. The Web UI does not know the underlying event; it only posts the action id back to Sunshine.

```json
{
  "id": "recover",
  "title": "Run recovery",
  "description": "Restore plugin-managed state.",
  "event": "sunshine.startup.recover",
  "icon": "fa-history",
  "danger": false
}
```

Action rules:

- `id` must be stable within the plugin.
- `event` is passed to the plugin as `--event`.
- `danger: true` should be used for destructive or disruptive actions.
- `icon` is an optional Font Awesome class name used by the Web UI.

## Invocation

Sunshine invokes plugin executables like this:

```text
plugin.exe --event <event> --payload <payload.json> --result <result.json>
```

Arguments:

- `--event`: lifecycle slot or action-backed event.
- `--payload`: path to a JSON payload file.
- `--result`: path where the plugin may write a JSON result object.

Plugins should ignore unknown future arguments when possible.

Exit codes:

- `0`: success.
- `1`: plugin handled the event unsuccessfully.
- `2`: configuration or permission error.
- `3`: retry requested. Future hosts may schedule retries.

Sunshine logs failures and continues its own lifecycle. Plugins must not assume they can block streaming indefinitely.

## Payload

Payload files are JSON objects. The host may add fields over time, so plugins should ignore unknown fields.

```json
{
  "host": {
    "name": "Sunshine",
    "protocol_version": 1
  },
  "event": "stream.first_session.starting",
  "paths": {
    "assets_dir": "C:/Program Files/Sunshine/assets",
    "config_file": "C:/Users/user/AppData/Local/Sunshine/sunshine.conf",
    "config_dir": "C:/Users/user/AppData/Local/Sunshine"
  },
  "plugin": {
    "id": "com.example.my-plugin",
    "name": "Example Plugin",
    "version": "1.0.0",
    "root_dir": "C:/Users/user/AppData/Local/Sunshine/plugins/com.example.my-plugin",
    "config": {
      "enabled_feature": true
    }
  },
  "session": {
    "client_name": "Living Room",
    "control_only": false,
    "launch_session_id": 42,
    "first_non_control_session": true,
    "last_non_control_session": false,
    "client_fps": 60
  },
  "app": {
    "id": "12345678-1234-1234-1234-123456789abc",
    "name": "Example Game",
    "cmd": "\"C:/Games/Example Game/game.exe\" --fullscreen"
  },
  "manual": true,
  "action": {
    "id": "recover",
    "title": "Run recovery",
    "event": "sunshine.startup.recover",
    "danger": false
  }
}
```

`manual` and `action` are present only for user-triggered actions.

`session` is present for stream lifecycle events. `client_fps` is the host's current best view of the client frame rate and may be used by plugins that tune per-stream behavior. `app` is `null` when Sunshine cannot associate the lifecycle event with a configured app. When present, `app.cmd` is the launch command string; plugins should parse it defensively and tolerate quoted paths, arguments, and missing executables.

## Result

When `--result` is provided, plugins should write a JSON object:

```json
{
  "status": "success",
  "message": "Recovery completed.",
  "changes": [
    "restored-profile"
  ]
}
```

Recommended result fields:

- `status`: `success`, `failed`, `warning`, or a plugin-specific string.
- `message`: short user-facing status message.
- `changes`: optional array of affected resources.
- `details`: optional structured diagnostic object.

Sunshine stores recent invocation records in the plugin history file and includes the plugin result in that history.

## Configuration

`default_config` from the manifest is merged with plugin-local and user-local `config.json` files. The merged object is passed in `payload.plugin.config`.

Plugins should treat config as immutable during invocation. To persist plugin state, write under the plugin-specific config directory:

```text
<Sunshine config dir>/plugins/<plugin-id>/
```

The host reserves these filenames:

- `plugin.json`
- `config.json`
- `state.json`
- `history.json`

Use plugin-specific names for other state files.

## Security And Review

Executable plugins run with the same user context Sunshine uses for plugin invocation. Capabilities are currently declarative, but future marketplace flows will use them for install prompts, update prompts, and trust review.

Declare every capability your plugin relies on. Do not hide privileged behavior behind generic capability names.

Suggested capability naming:

- `stream.lifecycle.read`
- `driver.nvidia-profile.write`
- `filesystem.plugin-state.write`
- `filesystem.programdata.write`
- `process.spawn`
- `network.http.client`

## Compatibility

Plugin Platform v1 is intentionally executable-based:

- no C++ ABI dependency
- language independent
- crash isolated from Sunshine core
- suitable for official and community marketplace distribution

Future platform versions may add provider plugins for capture, encoder, input, or network extension points. Those deeper integrations will use a separate contract.
