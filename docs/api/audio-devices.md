# Audio devices

Status: implemented in src/audio_devices.cpp (shaping, shared by every platform) and
src/platform/windows/audio_devices.cpp (the MMDevice reader), served from
src/confighttp/diagnostics.cpp.

## Purpose

The Web UI and the Panel let a person pick the playback device the host captures
(`audio_sink`) and the device that takes the audio while a stream runs
(`virtual_sink`). They need the real list of devices, with the known virtual
ones marked and the state of VB-CABLE, which the host's microphone backend writes
to. Before this route the host had no API that listed audio devices.

## Authentication

The route uses the existing Web UI authentication (`confighttp::authenticate`),
like `/api/microphone/status`.

## GET /api/audio/devices

Reads the render and capture endpoints through the MMDevice API
(`IMMDeviceEnumerator::EnumAudioEndpoints`, `GetDefaultAudioEndpoint`). It only
reads: no endpoint is opened for audio, no default is changed.

Response (`version` 1):

```json
{
  "success": true,
  "version": 1,
  "supported": true,
  "render": [
    {
      "id": "{0.0.0.00000000}.{32805c76-...}",
      "name": "Speakers (Steam Streaming Speakers)",
      "adapter": "Steam Streaming Speakers",
      "description": "Speakers",
      "state": "active",
      "form_factor": "speakers",
      "default": true,
      "default_communications": true,
      "virtual": "steam_streaming"
    }
  ],
  "capture": [],
  "vb_cable": {
    "installed": true,
    "driver_present": true,
    "driver_disabled": true,
    "state": "disabled",
    "render_id": "{0.0.0.00000000}.{54e965e0-...}",
    "render_name": "CABLE In 16 Ch (VB-Audio Virtual Cable)",
    "capture_id": "{0.0.1.00000000}.{2e394e2b-...}",
    "capture_name": "CABLE Output (VB-Audio Virtual Cable)"
  }
}
```

- `render` and `capture` list the endpoints that are active, disabled or unplugged;
  the default endpoint comes first, then the active ones by name. Endpoints Windows
  only remembers (state not present) are left out.
- `default_output` says whether the system has a default playback device: `{"present": bool, "reason": "", "render_active": n, "host_waiting": bool}`.
  `reason` is empty when a device is the default, otherwise `no_endpoint` (no playback device exists: a server, a cloud machine, a VM),
  `none_active` (devices exist but all are disabled or unplugged) or `no_default` (active devices, none is the default).
  `host_waiting` is true while a stream runs and its audio capture waits for a device: the stream goes on without sound and capture starts
  by itself when a device appears (`IMMNotificationClient`: added, made active, or made the default; a poll covers a missed event).
  The host writes one log line for the cause per audio session. Linux and macOS always answer `present: true`.
- `state`: `active`, `disabled`, `unplugged`.
- `form_factor` is the `EndpointFormFactor` of the endpoint as a word: `speakers`,
  `line_level`, `headphones`, `microphone`, `headset`, `handset`,
  `digital_passthrough`, `spdif`, `digital_display`, `remote_network`, `unknown`.
- `default` is the default endpoint of its direction for the console role,
  `default_communications` for the communications role.
- `virtual` is `null` for a physical or unknown device, or one of `vb_cable`
  (the VB-Audio Virtual Cable the microphone backend uses), `vb_audio` (another
  VB-Audio product), `steam_streaming`, `virtual_microphone` (the host's own
  USB/IP microphone).
- `vb_cable.state`: `ready` (the render and the capture side are active),
  `partial` (one side is active), `disabled` (the driver is installed but its
  device is disabled in the Device Manager, CM_PROB_DISABLED code 22: Windows then
  lists its endpoints as not present), `unavailable` (known, nothing usable, for
  another reason: an endpoint disabled or unplugged, another driver problem),
  `missing` (not installed; endpoints Windows still remembers from a removed
  device do not count once the device tree has been read and holds no VB-CABLE).
  `installed` is true when the driver's device is in the device tree or a VB-CABLE
  endpoint is active, disabled or unplugged. `driver_present` and `driver_disabled`
  come from the device tree (SetupAPI, `CM_Get_DevNode_Status`; read only, the host
  never enables the device); both are false when the tree could not be read, and
  then the endpoints decide alone (remembered endpoints count as `unavailable`).
  VB-CABLE is recognised the way the microphone backend finds it: by the adapter
  name `VB-Audio Virtual Cable`.

Linux and macOS answer `{"success": true, "version": 1, "supported": false, ...}`
with empty lists and `vb_cable.state` `unsupported`.

If the enumerator cannot be created the answer is
`{"success": false, "supported": true, "error_code": "AUDIO_ENUMERATOR_UNAVAILABLE"}`.

## Component management

On Windows, the Control Panel's component page separates checking, downloading,
installing and uninstalling VB-CABLE. The audio devices API remains read-only.
Installation uses the verified Pack43 archive. Device detection is independent
of the installed pack version. Windows may require a restart to finish a driver
operation; the panel does not restart it.

## Sources

Listed in docs/authority/source-index.tsv: `ms-mmdevice-api-20261112`,
`ms-enumaudioendpoints-20261112`, `ms-getdefaultaudioendpoint-20261112`,
`ms-endpointformfactor-20261112`, `ms-device-state-constants-20261112`,
`ms-immnotificationclient-20261116`, `ms-ondefaultdevicechanged-20261116`, `ms-ondevicestatechanged-20261116`, `ms-registerendpointnotificationcallback-20261116`, `ms-winverifytrust-20261112`, `ms-pnputil-command-syntax-20261112`, `ms-cm-get-devnode-status-20261112`, `ms-setupdigetclassdevsw-20261112`, `ms-device-manager-error-messages-20261112`.
