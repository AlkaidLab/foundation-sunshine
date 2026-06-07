# Plugin Lifecycle Host Plan

## Problem

Sunshine already has useful session lifecycle seams, but platform-specific work is still wired directly into core code. For example, `stream.cpp` calls `platf::streaming_will_start()` when the first non-control-only session starts and `platf::streaming_will_stop()` when the last non-control-only session ends. Windows then performs several built-in actions from `platform/windows/misc.cpp`, including NVIDIA profile handling.

That shape works for built-in platform behavior, but it is not the right long-term home for optional tuning features such as NVIDIA Control Panel stream optimizations. Those features should be isolated, independently upgradeable, visible in the UI, and easy to disable when they misbehave.

## Direction

Introduce a lifecycle plugin host in Sunshine core. The host emits typed lifecycle slots and executes installed plugin executables that subscribe to those slots. Sunshine remains responsible for timing, ordering, logging, and failure isolation. Plugins own optional behavior.

The first target plugin is the NVIDIA Control Panel optimizer. It should eventually move from core nvprefs calls into an official Windows plugin executable.

## Existing Session Seams

The current session-level lifecycle is already good enough for a first plugin host:

- `stream.first_session.starting`: the first non-control-only stream session is starting.
- `stream.last_session.stopping`: the last non-control-only stream session is stopping.

These map directly to the existing `running_non_control_only_sessions` gates in `stream.cpp`. Control-only sessions should not fire these slots because they do not represent media streaming.

Additional lifecycle slots are useful for crash recovery and cleanup:

- `sunshine.startup.recover`: startup recovery before normal service work begins.
- `sunshine.shutdown.restoring`: final shutdown cleanup before process teardown.
- `stream.dynamic_params.changed`: mid-stream FPS changes. Broader high-frequency encoder parameter changes should use a separate contract rather than spawning executable lifecycle plugins.

## MVP Scope

The first implementation should be intentionally small:

- Add a `plugin::lifecycle` host module.
- Scan an installed plugin directory for `plugin.json` manifests.
- Allow plugins to subscribe to lifecycle slots.
- Execute plugin `entry` executables with `--event <slot>`, a temporary JSON payload file, and an optional JSON result file.
- Capture plugin exit codes, structured plugin results, and recent invocation history without failing Sunshine's own session lifecycle.
- Wire current startup/shutdown and first-session/last-session points to lifecycle slots.
- Leave marketplace registry, downloads, signatures, and full log viewing for later phases.

## Installed Plugin Layout

Installed plugins live under Sunshine's config path:

```text
<config>/plugins/<plugin-id>/
  plugin.json
  plugin.exe
  config.json
```

The first host also checks the application-local path for packaged plugins:

```text
assets/plugins/<plugin-id>/
  plugin.json
  plugin.exe
```

Config-path plugins override packaged plugins with the same id.

## Manifest Shape

```json
{
  "api_version": 1,
  "id": "com.alkaidlab.nvidia-control-panel-optimizer",
  "name": "NVIDIA Control Panel Optimizer",
  "version": "1.0.0",
  "min_host_version": "2026.0.0",
  "package": {
    "publisher": "AlkaidLab",
    "channel": "stable",
    "trust": "official"
  },
  "platforms": ["windows"],
  "entry": "sunshine-plugin-nvprefs.exe",
  "slots": [
    "sunshine.startup.recover",
    "stream.first_session.starting",
    "stream.dynamic_params.changed",
    "stream.last_session.stopping",
    "sunshine.shutdown.restoring"
  ],
  "permissions": [
    "driver.nvidia-profile",
    "filesystem.programdata",
    "stream.context.read"
  ],
  "capabilities": [
    "stream.lifecycle.read",
    "driver.nvidia-profile.write",
    "filesystem.programdata.write",
    "filesystem.plugin-state.write"
  ],
  "actions": [
    {
      "id": "recover",
      "title": "Run recovery",
      "event": "sunshine.startup.recover",
      "icon": "fa-history",
      "danger": false
    }
  ],
  "timeout_ms": 5000
}
```

Unknown fields are ignored so future marketplace metadata can be added without breaking old hosts.

## Execution Protocol

For each slot, Sunshine creates a payload file and invokes the plugin executable:

```text
plugin.exe --event stream.first_session.starting --payload C:\...\payload.json --result C:\...\result.json
```

The payload is JSON:

```json
{
  "host": {
    "name": "Sunshine",
    "protocol_version": 1
  },
  "event": "stream.first_session.starting",
  "session": {
    "control_only": false,
    "first_non_control_session": true,
    "client_fps": 60
  },
  "app": {
    "name": "Example Game",
    "cmd": "\"C:/Games/Example Game/game.exe\" --fullscreen"
  }
}
```

The plugin returns:

- `0`: success
- `1`: plugin handled the request unsuccessfully; log and continue
- `2`: configuration or permission error; log and continue
- `3`: retry requested; future hosts may schedule a retry

When `--result` is present, the plugin may write a JSON object with user-facing status:

```json
{
  "status": "success",
  "message": "Recovery completed.",
  "changes": ["restored-profile"]
}
```

Stdout and stderr are captured in later phases. The current host records the command status, exit code, timeout, duration, and plugin result object.

## Platform v1 Model

The lifecycle host is the first layer of a larger plugin platform:

- `slots` describe automatic lifecycle subscriptions.
- `actions` describe user-triggered operations. The Web UI renders actions from the manifest and never hard-codes lifecycle event names.
- `capabilities` describe security-sensitive behavior for future install/update prompts.
- `package` carries publisher, channel, and trust metadata for future marketplace integration.
- `history.json` stores recent invocations under the plugin's config directory.

The v1 plugin form remains executable-based to avoid C++ ABI coupling and to keep plugin crashes isolated from Sunshine core.

## Failure Policy

Plugins must not block core streaming indefinitely. Each plugin invocation has a timeout. A timeout or non-zero exit is logged and Sunshine continues. Built-in platform lifecycle behavior continues to run even when plugins fail.

Security-sensitive plugin behavior is opt-in. Marketplace and signature enforcement are intentionally separate follow-up work; the MVP only executes installed manifests from trusted local directories.

## Migration Plan

Phase 1: Lifecycle host

- Add host module and manifest parser.
- Wire startup, shutdown, first-session start, and last-session stop slots.
- Add documentation and simple developer guidance.

Phase 2: Official NVIDIA optimizer plugin

- Add a Windows-only `sunshine-plugin-nvprefs.exe` target that implements the lifecycle execution protocol.
- Package an official enabled-by-default manifest under `assets/plugins/com.alkaidlab.nvidia-control-panel-optimizer/` so existing optimizer behavior remains active after migration.
- Add a config schema so the GUI can render plugin settings before the marketplace exists.
- Move stream-time NVIDIA profile optimizer logic into `sunshine-plugin-nvprefs.exe`.
- Keep undo files owned by the plugin.
- Subscribe to startup recovery, first-session start, dynamic params change, last-session stop, and shutdown restore.
- Remove automatic core nvprefs lifecycle calls once the official plugin path is validated.
- Keep the manual `restore-nvprefs-undo` command as a recovery entry point during migration.

Phase 3: Web UI installed-plugin management

- Show installed plugins, enabled state, version, slots, capabilities, platform support, and executable availability.
- Show declared actions, capabilities, last run, and recent invocation history.
- Provide enable/disable, plugin config editing, and action execution.
- Keep NVIDIA optimizer settings in plugin config so optional stream tuning does not grow the core encoder settings surface.
- Defer full stdout/stderr log viewing until the host has a larger plugin log retention policy.

Phase 4: Marketplace

- Add remote registry metadata.
- Download and install official plugins.
- Validate signatures and show permission changes before updates.

## PR Strategy

Keep infrastructure separate from the NVIDIA optimizer migration:

1. PR A: lifecycle plugin host and docs.
2. PR B: official NVIDIA optimizer plugin using the host.
3. PR C: Web UI installed plugin management.
4. PR D: marketplace registry and update flow.

This keeps the core change reviewable and gives the NVIDIA optimizer a clean plugin boundary instead of deepening core coupling.

## Current Implementation Status

- Phase 1 is implemented in `src/plugin.cpp` and wired into startup, shutdown, first-session start, and last-session stop.
- Phase 2 is implemented for Windows with `sunshine-plugin-nvprefs.exe`, packaged manifest/schema assets, plugin-owned undo-file lifetime, and opt-in stream-time NVIDIA profile optimization.
- Phase 3 has an installed-plugin API and Web UI page for listing plugins, enabling/disabling them, editing schema-backed config, running declared actions, and showing recent invocation history.
- Phase 4 has a first marketplace scaffold: `plugin-registry/`, listing schema, registry index generator, GitHub issue template, registry CI, marketplace documentation, and a browse-only Sunshine marketplace endpoint/UI path.
- External plugin author guidance lives in `docs/plugin_development.md`.
- Remaining work is stdout/stderr log visibility, one-click install/update, signature or attestation enforcement, publisher verification, and rollback-aware plugin package management.
