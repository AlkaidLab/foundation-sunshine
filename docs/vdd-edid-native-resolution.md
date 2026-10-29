# EDID native resolution

On Windows, open **Audio/Video → Virtual Display → Advanced → EDID native
resolution**. The dropdown contains **Default (1920×1080)** plus the configured
**Advertised Resolutions**, including custom entries. Adding a resolution does
not automatically select it as native. After changing the advertised resolution
or FPS lists, use Apply to reload Sunshine's configured mode list, then choose
Quit Session in Moonlight and reconnect to load the new EDID. For an EDID-only
change, Save before quitting and reconnecting. Disable VDD reuse for this
reconnect if Sunshine would otherwise keep the existing virtual display.
Restart the affected game.

Save alone does not reload the running host's advertised mode list. If a newly
added size has not been applied, Sunshine can still send its old list through
SETMODES; the driver correctly rejects a native size absent from its actual list
and uses the original EDID. This does not require restarting the Windows adapter.

Some games filter fullscreen choices using native EDID dimensions or aspect
ratio even when Windows exposes additional modes. This preference can improve
compatibility; the session/client still chooses the actual stream mode.

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

The host persists `vdd_edid_native_resolution=default|WIDTHxHEIGHT` in Sunshine
and `vdd_settings.edid.EdidNativeResolution` in the driver XML on Web UI save.
Default is sent explicitly to reset a prior selection; older API clients that
omit the field leave the XML preference intact. The API validates list membership,
dimensions and clock feasibility before persisting explicit values.

Requires a ZakoVDD release implementing `EdidNativeResolution`, bundled with
Sunshine. No driver binary/version is replaced in this proposal. The driver
independently checks its actual mode list and chooses an encodable supported
refresh nearest 60 Hz. A valid custom EDID takes precedence. Mode ordering and
the IddCx preferred index remain unchanged.
