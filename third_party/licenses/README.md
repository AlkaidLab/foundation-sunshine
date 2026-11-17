# Bundled third-party licenses

The Windows installer and the portable ZIP ship the files in this directory
under `licenses/`. `manifest.json` is the review record for every binary that
`cmake/packaging/FetchDriverDeps.cmake` downloads: the upstream tag, the CMake
version pins, the repository the asset comes from, the SHA-256 of the vendored
license text, and the package paths that receive it.

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| ViGEmBus | 1.22.0 | BSD-3-Clause | <https://github.com/nefarius/ViGEmBus/tree/v1.22.0> |
| usbip-win2 | 0.9.8.1 | BSD-2-Clause | <https://github.com/vadimgrn/usbip-win2/tree/v.0.9.8.1> |
| nefcon | 1.18.74 | MIT | <https://github.com/nefarius/nefcon/tree/v1.18.74> |
| ZakoVDD | 0.18.2 (latest), 0.15.10 (Windows 10) | MIT | <https://github.com/qiin2333/zako-vdd> |
| Zako Virtual Mouse driver | 1.3.2 | Proprietary, binary redistribution with a host package permitted | <https://github.com/AlkaidLab/ZakoVirtualMouse/tree/v1.3.2> |
| zlib (`zlib1.dll`) | toolchain-provided, version not pinned | Zlib | <https://github.com/madler/zlib> |

The license files are verbatim copies from the tag named in the table. The
manifest pins their digests, so changing a text without repeating the review
fails `scripts/ci/check-third-party-licenses.py`. That check also fails when a
new fetch call appears in `FetchDriverDeps.cmake` without a manifest entry, or
when a version pin no longer matches the reviewed license.

License identifiers follow the SPDX License List (source index key
`spdx-license-list-20261117`). The installer and archive layout follow the
CMake install and CPack Archive documentation (`cmake-install-20261117`,
`cmake-cpack-archive-20261117`); the Inno file entries follow the `[Files]`
section documentation (`inno-setup-files-20261117`).

The zlib license text is the v1.3.1 text, but the `zlib1.dll` in the package is
the one the build toolchain supplies; its version is not pinned, so neither the
manifest nor `NOTICE` asserts a DLL version.

`NOTICE` at the repository root names the same components and their copyright
holders. The Inno installer and the CPack ZIP both copy this directory.

## Other packaged binaries

The audit also covers the binaries that are copied from the source tree rather
than fetched by `FetchDriverDeps.cmake`. MinHook, the NVIDIA RTX Video runtime
notices and the NVIDIA DLSS/optical-flow notices already ship next to their
binaries. The following helper programs have no license text in this directory
yet and are not covered by the manifest check:

| File | Identification | Status |
| --- | --- | --- |
| `tools/DevManView.exe` | DevManView 1.80, NirSoft, copyright 2009-2023 Nir Sofer | Freeware; the exact redistribution terms still need reviewing |
| `tools/restart64.exe` | Custom Resolution Utility helper | Provenance and version not recorded |
| `tools/SetDpi.exe` | SetDpi helper | Provenance and version not recorded |
| `tools/device-toggler.exe` | .NET helper, version 0.0.0.0 | Provenance not recorded |
| `tools/setreg.exe` | .NET helper, version 0.0.0.0 | Provenance not recorded |
| `assets/gui/WebView2Loader.dll` | Microsoft WebView2 loader | Supplied by the control-panel build; its terms are handled there |
