# AGENTS.md — foundation-sunshine (linux-migration branch)

AlkaidLab/qiin2333 fork of LizardByte/Sunshine (Windows streaming enhancements).
This workspace ports those enhancements to Arch Linux on the `linux-migration` branch.
Remote: `mine` = user's fork (QiE2035, push target), `origin` = AlkaidLab upstream.

**Target environments:** Arch Linux with **KDE Plasma (Wayland)** and **niri** are the
primary desktops; a generic path is preferred over a desktop-specific one whenever both
exist (PipeWire/PulseAudio for audio, wlr-output-management / X11 / niri IPC before
anything KDE-only). Compositor-specific backends must probe for their tool and degrade
gracefully when it is absent — never assume kscreen-doctor exists.

**Read before touching sensitive areas:**
- `LINUX_MIGRATION_REPORT.md` — what was ported, how, with file/line evidence
- `LINUX_PORT_GAPS.md` — remaining gap backlog (partial / feasible / non-portable)

## Build / test / package

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_ASSETS_DIR=share/sunshine -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/sunshine \
  -DSUNSHINE_ENABLE_CUDA=OFF -DBUILD_TESTS=ON
ninja -C build sunshine
cd build && ctest          # baseline: 12/13 pass; Audio/MouseHID/Encoder fail headless (expected)
```

Packaging (`packaging/arch-local/`, installs prebuilt tree, no compile in makepkg):
bump `pkgrel` → `cmake -B build ...` (refreshes the binary version stamp — the user
checks the `Sunshine version:` log line) → `ninja -C build sunshine` → `makepkg -f`.
Never run makepkg while ninja is still linking. Never commit `packaging/arch-local/{pkg,src,*.pkg.tar.zst}`.
User installs packages themselves (sudo needs a password).

## Platform rules (Windows CI builds this repo — do not break it)

- `SUNSHINE_TARGET_FILES` (common.cmake) compiles on **Windows too**. New Linux-only
  sources go in `cmake/compile_definitions/linux.cmake` (`PLATFORM_TARGET_FILES`).
- Tests are GLOB'd on all platforms and `build_win` runs `BUILD_TESTS=ON`: a
  Linux-only test file must be excluded under `if (WIN32)` in `tests/CMakeLists.txt`.
- Windows impls stay inside `#if defined(_WIN32)`; Linux impls after `#else`.
  Gotcha: in `vdd_utils.cpp` the entire Windows half sits in `#if defined(_WIN32)`
  from line 1 — Linux-side `#include`s go after the `#else`, not at the top.
- Un-gating a Windows-only feature for Linux (include move, real capability report)
  is fine only if the Windows branch is byte-identical afterwards.
- New shared-code behaviors must be gated or verified as Windows-neutral; audit with
  `git diff <merge-base>..HEAD -- <shared file>` before pushing.

## Architecture boundaries

- Cross-platform session logic: `src/display_device/` (session.cpp, settings.cpp).
  Platform backends: `src/platform/linux/display_device.cpp` (kscreen-doctor), Windows in
  `src/platform/windows/display_device/`.
- VDD (virtual display) Linux backend: `src/display_device/vdd_utils.cpp` `#else` half +
  `src/platform/linux/vdd_edid.*`. VDD state is DRM-persistent — `adopt_orphan_vdd_locked()`
  recovers it after restart; never recreate what `live_virtual_display_connector()` can adopt.
- Tray owns a single main-thread event loop on Linux. Any shutdown/restart raised off the
  main thread must wake it via `system_tray::end_tray()` (init_tray installs a shutdown
  watcher — keep it). `tray_update()` from helper threads is the established pattern.
- HDR analyzer feeds `encode_device_t::hdr_luminance_stats`; constants come from
  `video_hdr_metadata.h` (`detail::st2084_*`) — do not re-derive PQ math
  (widespread fractions `c3=2399/...`, `m2=×32` are wrong; 100 nits ⇒ signal 0.5081).

## Known gotchas

- Sunshine runs with file capabilities ⇒ `AT_SECURE`: `secure_getenv` hides
  `XDG_RUNTIME_DIR`, so `sd_bus_open_user()` fails in-process — use
  `platf::sdbus::open_user_bus()` (`src/platform/linux/sdbus_session.h`).
- `popen` children inherit listening sockets and can hang forever — use the bounded
  `vdd_utils::run_logged()` (fork/exec, closes fds, 10s kill).
- kscreen-doctor needs the session env (`WAYLAND_DISPLAY` + bus); it rejects decimal
  refresh in `mode.WxH@refresh` — fractional rates go by mode id.
- NVIDIA: status-forced connectors emit no hotplug (CRTC must be force-assigned via
  pidfd DRM-master borrow) and forced-off reads as plain "disconnected" in sysfs —
  enumeration must consult `vdd_utils::offlined_physical_connectors()`.
- `BOOST_LOG` `"...sv"` literals need `using namespace std::string_view_literals`.
- Only one sunshine instance at a time (ports 47984/47989/47990/48010).

## Windows parity conventions (enforced — the user audits these)

- **Canonical identifiers live in shared headers; never re-hardcode them.**
  `ZAKO_NAME`/`VDD_NAME` (globals.h), ST2084 constants (`video_hdr_metadata.h`
  `detail::st2084_*`; the fractions "c3=2399/4096×32" and "m2=×32" found in the wild
  are wrong), per-client physical-size classes, VDD state strings
  (`classify_vdd_state`), clipboard wire constants. If a Linux file spells one of
  these as a literal, that is a bug.
- **Port semantics, not just the happy path.** When mirroring a Windows behavior,
  diff the Windows implementation's acceptance bounds, defaults, and tolerances and
  reproduce them (e.g. `parse_vdd_resolution` accepts any positive WxH with an
  x/X separator — do not invent extra clamps). Feasibility filtering belongs where
  the Windows side has it (driver / EDID generator).
- **User-visible naming must match Windows.** The virtual display's EDID name is
  `ZAKO_NAME` ("Zako HDR"); anything Linux renames differently is a divergence.
  Note KDE prepends the PnP vendor letters ("UQD") decoded from the EDID
  manufacturer ID — inherent to EDID, not a naming mismatch.
- After porting, grep the touched Linux files for literals duplicating shared
  constants and for leftover divergences; the user spot-checks names and numbers.

## Conventions

- Commit per logical step; tag each feature milestone (`v0.1-linux-base` …
  `v0.8.1-linux-tray-vdd-state` ladder — continue it). User reads Chinese.
- Commit messages: English, conventional-commit style (`feat(linux): …`).
- Do not delete or "clean up" the two MD docs above; update them when landing work.
