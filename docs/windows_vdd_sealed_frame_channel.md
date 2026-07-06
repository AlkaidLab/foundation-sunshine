# Windows VDD Sealed Frame Channel

This document defines the v2 Sunshine <-> ZakoVDD frame channel. The goal is to keep the fast producer-owned borrowed-frame path while removing the legacy globally named `Global\ZakoVDD_*` object exposure.

## Goals

- Keep VDD as the owner of the source frame ring.
- Let Sunshine borrow the latest complete producer frame with no CPU readback.
- Stop exposing predictable global names for metadata, frame-ready events, and textures.
- Keep Sunshine's legacy named-object consumer as a fallback for old drivers.
- Keep new drivers sealed-only by default. Legacy named export must be explicitly enabled with `LEGACYNAMEDFRAMECHANNEL=1`.
- Make strict mode fail closed rather than silently falling back to named objects.

## Host Modes

`SUNSHINE_VDD_FRAME_CHANNEL` controls host behavior:

- `auto`: probe sealed support, use sealed borrow when attach succeeds, otherwise fall back to hardened legacy named objects for old drivers or drivers with legacy export explicitly enabled.
- `legacy`: skip sealed probing and use hardened legacy named objects. This requires an old driver or a new driver with `LEGACYNAMEDFRAMECHANNEL=1`.
- `sealed`: require sealed caps, open, and attach to succeed. Do not fall back to legacy names.

## Runtime Telemetry

Sunshine logs both the transport mode and the final structured selection:

```text
[vdd_capture] opened sealed monitor ... channel=sealed selection=sealed_opened access=duplicated_handles
[vdd] backend ready ... channel=sealed selection=sealed_opened sealed_supported=true producer_slots=3 generation=2
```

`channel` describes the active capture transport:

- `sealed`: sealed IOCTL path with duplicated unnamed handles.
- `legacy_named_hardened`: legacy named-object path with restricted access.
- `sealed_probe_supported_legacy_capture`: sealed caps were present but host fell back to legacy capture.

`selection` is the stable machine-readable reason:

- `sealed_opened`: sealed caps, open, metadata attach, texture import, and keyed-mutex attach succeeded.
- `forced_legacy`: `SUNSHINE_VDD_FRAME_CHANNEL=legacy`.
- `caps_unsupported`: driver/interface does not expose sealed frame-channel caps.
- `caps_failed`: capability query failed or returned unusable caps.
- `open_unsupported`: open IOCTL was not supported by the driver.
- `open_failed`: open IOCTL reached the driver but failed.
- `attach_failed`: open succeeded but Sunshine rejected metadata/textures/keyed mutexes.
- `sealed_required_failed`: `SUNSHINE_VDD_FRAME_CHANNEL=sealed` failed closed.
- `legacy_opened`: fallback legacy named path opened successfully.

Operationally, healthy sealed sessions should show:

- `channel=sealed`
- `selection=sealed_opened`
- `sealed_supported=true`
- `generation=N` stable during a channel lifetime
- no `frame channel open failed`, `sealed frame channel attach failed`, or `unable to read stable metadata`

## IOCTLs

The authoritative ABI is `src/display_device/vdd_control_ioctl.h`.

`IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS`

The driver returns `VDD_FRAME_CHANNEL_CAPS` with:

- `Version = VDD_FRAME_CHANNEL_CAPS_VERSION`
- `Flags` containing all required bits:
  - `VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW`
  - `VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES`
  - `VDD_FRAME_CHANNEL_FLAG_STRICT_DACL`
- `MaxSharedSlots <= VDD_FRAME_CHANNEL_MAX_SLOTS`
- `MetadataSize >= sizeof(shared_frame_metadata_t)` as used by Sunshine.

`IOCTL_VDD_OPEN_FRAME_CHANNEL`

Sunshine sends `VDD_FRAME_CHANNEL_OPEN_REQUEST`:

- `MonitorIndex`: VDD monitor index resolved by Sunshine.
- `TargetProcessId`: Sunshine process ID. Returned handle values must be valid in this process.
- `RequiredFlags`: security bits the driver must satisfy.
- `DesiredSlots`: compatibility guard. `0` means driver default; nonzero must match the driver's advertised `MaxSharedSlots`.
- `AdapterLuid*`: Sunshine D3D11 device adapter. Driver must reject mismatches.

The driver returns `VDD_FRAME_CHANNEL_OPEN_RESPONSE`:

- `MetadataHandle`: unnamed file mapping handle duplicated into Sunshine.
- `FrameReadyEventHandle`: unnamed event handle duplicated into Sunshine.
- `Slots[i].TextureHandle`: unnamed D3D11 shared texture handles duplicated into Sunshine.
- `SlotCount`: number of valid slot handles.
- `Flags`: must include `RequiredFlags`.

Sunshine opens the current driver with `DesiredSlots=3`. The diagnostic probe defaults to `DesiredSlots=0` so operators can query the active driver without knowing the producer ring size in advance.

## Metadata Consistency

`shared_frame_metadata_t::MetadataSequence` is a packed compatibility field:

- high 16 bits: `ChannelGeneration`
- low 16 bits: seqlock counter
- low bit set: producer is updating metadata
- low bit clear: metadata is stable

The field reuses the old reserved slot, so metadata remains 128 bytes. Sunshine reads metadata with a stable snapshot helper and rejects torn reads. If `ChannelGeneration` changes during capture, Sunshine requests reinitialization to avoid mixing old textures with new producer metadata.

## Driver Contract

The driver remains the source-frame owner.

Required behavior:

- Create metadata mapping, frame-ready event, and frame textures without global object names by default.
- Create `Global\ZakoVDD_*` legacy named objects only when the explicit driver setting `LEGACYNAMEDFRAMECHANNEL=1` is present.
- Apply a restrictive DACL before any handle duplication.
- Validate the caller and target process before duplicating handles.
- Duplicate handles into `TargetProcessId`; the raw numeric handle values returned in the response must be valid only in that process.
- Use the same shared metadata layout and keyed-mutex protocol as the legacy producer-owned ring.
- Reject adapter LUID, format, size, version, or slot-count mismatches.
- On any partial failure, close all duplicated handles before returning an error.

## Sunshine Host Contract

The current host implementation:

- Probes caps through `query_frame_channel_caps()`.
- Opens the channel through `open_frame_channel()`.
- Maps sealed metadata and opens textures with `ID3D11Device1::OpenSharedResource1()`.
- Validates metadata, slot count, texture desc, shader-resource flags, keyed mutex, and adapter LUID.
- Closes all returned handles after importing resources.
- Falls back only in `auto` mode. `sealed` mode fails closed.

## Validation Checklist

Minimum driver-side validation before enabling sealed mode by default:

- `SUNSHINE_VDD_FRAME_CHANNEL=sealed` succeeds with a current driver.
- `SUNSHINE_VDD_FRAME_CHANNEL=legacy` still works with old drivers, and with new drivers only after `LEGACYNAMEDFRAMECHANNEL=1`.
- `SUNSHINE_VDD_FRAME_CHANNEL=auto` uses sealed on new drivers and legacy on old drivers.
- Handle enumeration does not reveal `Global\ZakoVDD_Frame_*`, `Global\ZakoVDD_Meta_*`, or `Global\ZakoVDD_FrameReady_*` for default new-driver sealed sessions.
- Driver restart, monitor mode change, HDR format change, and Sunshine restart do not leak handles or stuck keyed-mutex slots.
- Borrowed frame telemetry remains stable: low `consumer_acquire_timeouts`, low held-slot drops, and no persistent fallback churn.

## Operator Commands

Build the Sunshine host and tools:

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
cmake --build build --target sunshine vdd-frame-channel-probe check-vdd-ioctl-abi -j 4
```

Check that the duplicated ABI header is in sync with the driver repo:

```powershell
cmake --build build --target check-vdd-ioctl-abi
```

Enable new-driver legacy named export only when old Sunshine builds must connect:

```powershell
New-Item -Path 'HKLM:\SOFTWARE\ZakoTech\ZakoDisplayAdapter' -Force | Out-Null
New-ItemProperty -Path 'HKLM:\SOFTWARE\ZakoTech\ZakoDisplayAdapter' -Name LEGACYNAMEDFRAMECHANNEL -PropertyType DWord -Value 1 -Force
```

Return to sealed-only default:

```powershell
New-ItemProperty -Path 'HKLM:\SOFTWARE\ZakoTech\ZakoDisplayAdapter' -Name LEGACYNAMEDFRAMECHANNEL -PropertyType DWord -Value 0 -Force
```

Probe sealed caps only:

```powershell
.\build\tools\vdd-frame-channel-probe.exe
```

Open the sealed channel for monitor 0 and print a stable metadata snapshot:

```powershell
.\build\tools\vdd-frame-channel-probe.exe --open --monitor 0 --slots 3
```

Expected output includes:

```text
caps: version=1 flags=0x7 max_slots=3 metadata_size=128
open: version=1 flags=0x7 slots=3 metadata_size=128 ...
metadata: ... frame=... slot=.../3 meta_seq=... generation=... luid=...
```

Run a local smoke/stress pass against an active stream:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\vdd_sealed_channel_stress.ps1 -Iterations 5 -RequireSunshineSealedLog
```

The stress script verifies:

- sealed IOCTL open succeeds
- metadata snapshot contains `generation` and slot telemetry
- recent Sunshine logs contain `channel=sealed`
- recent Sunshine logs contain `selection=sealed_opened`
- no structured sealed failure selection is present

If the probe returns `OPEN_FRAME_CHANNEL failed: 21`, the driver is installed but no active VDD producer/swapchain is currently available. Start a Moonlight stream or otherwise activate the VDD monitor, then retry.
