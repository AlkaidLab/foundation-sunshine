# Saving legacy VDD settings

[Development](development.md) · [简体中文](../zh-CN/vdd-settings-persistence.md)

The configuration API saves advertised resolutions, refresh rates and the GPU
selection in Sunshine's configuration and the existing VDD mode/GPU XML.
Previously an XML failure could be ignored, or a later host-file failure could
leave only the XML updated. Concurrent requests could interleave those writes.

The save now stages XML and writes the host file under the existing configuration
mutex. It reports an XML failure to the caller without committing the host file.
If the host write fails, it restores the original XML bytes before releasing the
mutex. Unrelated XML settings are retained. Missing XML is a no-op: the API still
saves Sunshine settings without creating a driver configuration file.

This is best-effort error recovery, not a crash-safe transaction across two files.
Rollback failures are logged. Inspect the host log if the API reports a save
failure; resolve the file-access problem and retry. The change does not restart
the host, reinstall a driver or recreate a display.

## Mode-list values

For `resolutions` and `fps`, an omitted field, an empty string, JSON null, or the
string `"null"` retains the saved list. If that list is also absent/empty/null on
disk, the host's current advertised runtime list is used. Both configuration
files receive the same effective values. An explicit bracketed empty list,
`"[]"`, clears a list and leaves the driver to use its existing fallback modes.
Later omitted/blank/null values preserve that intentionally empty list.

These list fields remain strings, for example:

```json
{"resolutions":"[1920x1080,3840x2160]","fps":"[60,120]"}
```

This is a field fragment, not a complete API request. `POST /api/config` still
replaces other unprotected settings; send the complete configuration to retain
those fields. Existing protected/client-field rules remain unchanged.

This save-path change has no EDID preference, monitor-creation protocol or signed
driver dependency. It can be reviewed independently of the EDID-native-size option.
