# Image enhancement

This is the shared control plane for independent image enhancement capabilities.
`config.*` owns component selection, validation, version leases and maintenance;
`api.*` exposes those operations. Neither capability requires the other.

Windows implementations live in `src/platform/windows/image_enhancement/`:

- `backend_factory.*`: dispatches the selected backend through the neutral
  pre-encode filter contract.
- `rtx_hdr/`: NVIDIA RTX Video HDR, including its loader and isolated adapter.
- `dlss_nr/`: NVIDIA DLSS neural rendering, including its loader, isolated
  adapter and optional optical-flow provider.

## Compatibility boundary

The source directory and C++ namespace are `image_enhancement`. Existing external
identifiers intentionally retain their names: `/api/hdr-enhanced/*`,
`hdr_enhanced.json`, `hdr_enhanced.maintenance.json`, the `alkaidlab.nvidia_*`
backend IDs, DLL exports and `tools/hdr_enhanced/nvidia_*` installation paths.
They are persisted contracts shared with the Control Panel. Renaming internal
code must not orphan installed runtimes, invalidate settings or require users
to import their DLLs again. The packaging rules and configuration tests retain
those legacy paths explicitly.
