# RTX HDR build configuration

Sunshine uses CMake to locate or acquire the NVIDIA RTX Video SDK and configure the optional MSVC adapter DLL.
Ninja builds and fingerprints the adapter before compiling Sunshine. Running a separate PowerShell command is not required,
including for a new build directory.

## Build modes

| CMake option | Behavior |
| --- | --- |
| `SUNSHINE_RTX_HDR=AUTO` (default) | Enable when the SDK and required tools are available; otherwise build Sunshine without RTX HDR. |
| `SUNSHINE_RTX_HDR=ON` | Require the SDK and adapter. Configuration or build failures stop the build. |
| `SUNSHINE_RTX_HDR=OFF` | Do not locate, download, configure or build the adapter. |

The adapter and NVIDIA runtime are not Sunshine startup dependencies. Missing files, an incompatible ABI,
or a missing Microsoft Visual C++ runtime disable RTX HDR without preventing Sunshine from starting.
The first-party adapter DLL is added to the Sunshine package; the NVIDIA runtime and Microsoft runtime are not.

## SDK inputs

Set `RTX_VIDEO_SDK_URL` to the authorized GitHub Release asset URL. A private download also requires the
`RTX_VIDEO_SDK_TOKEN` environment variable; the existing `DRIVER_DOWNLOAD_TOKEN` name is accepted as a fallback.
CMake reads the asset's `sha256:` digest from the authenticated
GitHub Release response; developers and CI do not maintain a separate SDK path or digest variable.

The archive and extracted SDK are stored under `build/_driver_deps/rtx-video-sdk`. After the first successful
configure, Ninja-triggered CMake regeneration can reuse the locally saved release lookup and verified archive
without inheriting the private token. The non-secret Release URL is retained in `CMakeCache.txt` for that
regeneration; the token and GitHub digest are not stored there.

The artifact cache is separated by GitHub's archive SHA-256. Downloads use temporary files, up to three attempts, a 30-second
inactivity timeout, and total per-attempt limits of 60 seconds for metadata and 300 seconds for the archive.
Extraction is marked complete only after the required SDK files are found. A failed or mismatched download
is never used as a build input. Do not publish the private SDK cache through public CI artifacts or caches.

Never pass the access token through `-D`, a preset, or a committed file. CMake removes the token from its
child-process environment before compiler detection and does not save it to its cache or generated files.
Do not enable expanded CMake tracing while supplying credentials. For local builds, scope the token to the
configure command rather than permanently exporting it into every build process.

## Toolchain

The host remains MinGW UCRT64. The adapter requires Visual Studio 2022 C++ Build Tools and a Windows SDK.
`RTX_VIDEO_NGX_APPLICATION_ID` selects the NGX application ID; the default is the existing development ID 0.
The adapter uses the Microsoft Visual C++ 2015-2022 Redistributable (x64) installed on the host system.

Example:

```bash
cmake -S . -B build -G Ninja -DSUNSHINE_RTX_HDR=ON
ninja -C build -j 10
```

Keep the other CMake options required by your normal Sunshine build. The standalone
`scripts/build-nvidia-rtx-video-adapter.ps1` remains available for adapter-only diagnostics, not as a required
step in the main build.

## CI policy

- Official master builds and official signed releases require `ON`.
- Same-repository PRs may use private SDK credentials and default to `AUTO`.
- A manual development-branch build can select `AUTO`, `ON` or `OFF` with the `rtx_hdr` input.
- Fork PRs and fork CI builds use `OFF` and receive no project SDK credentials. Fork developers can still
  build with their own legitimately obtained SDK outside that CI policy.

Credentials are supplied only to the configure step. Compilation runs in a separate step without them.
The PR source repository must also be the official repository; targeting the official repository alone
does not establish that trust. No `pull_request_target` execution of PR code is needed.
