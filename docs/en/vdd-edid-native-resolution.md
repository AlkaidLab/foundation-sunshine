# EDID native resolution

[Project home](../readme/README.en.md) · [Features and setup](features.md) · [简体中文](../zh-CN/vdd-edid-native-resolution.md)

Some games filter fullscreen choices using native EDID dimensions or aspect
ratio even when Windows exposes additional modes. This Windows setting can
improve compatibility. It requires a compatible ZakoVDD release implementing
the native-size option in `CREATEMONITOR`; a valid custom EDID takes precedence.

## Select and verify a native resolution

1. In the Windows WebUI, open **Audio/Video → Virtual Display → Advanced → EDID native resolution**.
2. Choose **Default (1920×1080)** or a configured **Advertised Resolution**, including a custom size. Adding a resolution does not automatically select it as native.
3. If you changed the advertised resolution or FPS lists, use **Apply** to reload Sunshine's mode list. For an EDID-only change, use **Save**.
4. Turn off **VDD keep-enabled mode** in the system tray and disable **VDD reuse** so Sunshine can remove and recreate the virtual monitor. Choose **Quit Session** in Moonlight and reconnect.
5. Restart the affected game and check its fullscreen resolution choices. The session/client still chooses the actual stream mode.

If VDD is the only display, Sunshine's normal session cleanup protects it from
removal. In that configuration, reconnecting alone cannot apply an EDID change:
the saved preference remains pending until the monitor is safely recreated.
Session-only EDID refresh is not supported in this case. These instructions do
not require or recommend forcibly destroying the only display.

Save alone does not reload the running host's advertised mode list. If a newly
added size has not been applied, Sunshine can still send its old list through
SETMODES; the driver correctly rejects a native size absent from its actual list
and uses the original EDID. This does not require restarting the Windows adapter.

## Selection limits

Default always preserves the original EDID. Explicit selections must remain in
the configured list and be representable as an EDID detailed timing at one of
the configured frame rates (rounded as for the session mode list). Invalid or
unencodable entries remain visible but disabled. Removing the selected entry
keeps it visible as unavailable and blocks saving until the user explicitly
selects Default or another valid size; it never silently selects a different size.

Dimensions are limited to 1..4095 and the pixel clock to the EDID detailed timing
field. Consequently a configured 5120x1440 mode is not selectable as this native
timing, and 4K with only 120/144 Hz available is not encodable. The virtual mode
list itself is not restricted by this UI setting.

## Saving and compatibility

The host persists `vdd_edid_native_resolution=default|WIDTHxHEIGHT` only in
Sunshine's configuration. When creating a monitor, it reads the saved preference
and passes an explicit size through `CREATEMONITOR`. Default omits the optional
size and preserves the original EDID. No EDID preference is written to the
driver XML or registry; a missing VDD XML does not prevent saving this choice.

Older API clients that omit the field retain the saved Sunshine preference.
Partial saves retain omitted resolution/FPS lists; explicitly submitted lists,
including empty lists, replace the saved values. If a list is absent from both
the request and saved configuration, the host's current advertised list is used.
Startup command-line overrides
take precedence when creating a monitor, while Save updates the on-disk choice.
The API validates list membership, dimensions and clock feasibility. If an older
client changes either advertised mode list, the retained preference must still
be supported. Otherwise the request is rejected before either configuration file
is changed; explicitly select Default or another supported native size before
removing its size or encodable refresh rate. Existing XML persistence of the
driver's general mode/GPU settings is independent of the EDID preference.
API saves validate and write both configurations under the same lock. If the
Sunshine write fails, the original XML bytes are restored before another save
can proceed. Empty mode lists are valid and use the driver's fallback modes.
Rollback failures are logged; this does not provide recovery from process or
machine failure during a save.

### API request format

`POST /api/config` saves a complete configuration. The EDID preference, mode
lists and existing protected fields have preservation rules; other omitted
unprotected fields, such as `adapter_name`, are removed and may reset host/GPU
settings. Send the complete configuration to retain those fields.

Send `resolutions` and `fps` as strings. This is a list-field fragment of a
complete request:

```json
{"resolutions":"[1920x1080]","fps":"[60]"}
```

An explicitly empty list is the string `"[]"`; JSON arrays are not supported
for these fields.

### Driver compatibility

Requires a signed ZakoVDD release supporting the optional native size in
`CREATEMONITOR`, bundled with Sunshine. Older drivers ignore the optional group.
The driver independently checks its actual mode list and chooses an encodable supported refresh nearest
60 Hz. A valid custom EDID takes precedence. Mode ordering and the IddCx
preferred index remain unchanged. Internal monitor recreation retains the
original create request, including its native-size preference.
