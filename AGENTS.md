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
sccache --start-server 2>/dev/null || true      # optional but big win on rebuilds
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_ASSETS_DIR=share/sunshine -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/sunshine \
  -DSUNSHINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -1 | tr -d .)" \
  -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache \
  -DCMAKE_CUDA_COMPILER_LAUNCHER=sccache -DBUILD_TESTS=ON
ninja -C build sunshine
cd build && ctest          # baseline: 24 suites, 23 pass; the single Encoder SetUpTestSuite
                           # failure inside test_sunshine is headless-only (needs cap_sys_admin)
                           # (665 cases, 660 pass, 5 skip, 0 assertion failures)
```

CUDA is the default (`SUNSHINE_ENABLE_CUDA` defaults to ON on Linux; a missing
toolkit just skips the CUDA sources). Setting `CMAKE_CUDA_ARCHITECTURES` to the
local GPU keeps CUDA objects to one architecture; the fallback ladder is slow and
architectures the toolkit removed are filtered out automatically. The CUDA build
dlopen()s `libcuda.so.1` (NVIDIA driver) at runtime and links cudart statically —
no CUDA toolkit is needed to run it, and nothing changes for Moonlight clients.

CUDA adds the NVFBC path and removes the NVENC GPU->RAM->GPU round trip; the
ordinary KMS/Wayland/X11 paths stay compiled in. `SUNSHINE_BUILD_DIR=<dir>
makepkg -f` packages another build tree instead of `build/`.

Session-bus features (clipboard host, GNOME display config, the KWin foreground
producer) are genuinely optional: `cmake/compile_definitions/linux.cmake` defines
`SUNSHINE_HAVE_SDBUS` only when libsystemd is found, and those modules degrade to
inert halves instead of breaking the build. Never include `<systemd/sd-bus.h>`
outside such a guard — `src/platform/linux/sdbus_session.h` and
`mutter_display.h` are the patterns to copy. Verify with a fake pkg-config that
hides libsystemd (recipe and results: LINUX_MIGRATION_REPORT.md §6.25).

Packaging (`packaging/arch-local/`, installs prebuilt tree, no compile in makepkg):
`cmake -B build ...` (refreshes the binary version stamp) → `ninja -C build sunshine`
→ `makepkg -f`. The PKGBUILD derives `pkgver` from that stamp itself
(`<YYYY>.<MMDD>.<HHMMSS>.<commit>`, the string the `Sunshine version:` log line
prints), so the package name always names the build it contains; keep `pkgrel` at 1
and bump it only for packaging-only changes. It also re-links automatically when
the stamp and HEAD disagree. Never run makepkg while ninja is still linking. Never
commit `packaging/arch-local/{pkg,src,*.pkg.tar.zst}`.
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
  `git diff <merge-base>..HEAD -- <shared file>` before pushing, or run
  `python3 tools/audit-windows-parity.py [<base>..<head>]`, which lists every changed
  line that lands inside a Windows-only preprocessor region and fails when a
  `src/platform/linux/` source leaks into the shared build files.

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
- NVIDIA: forcing a connector on through its `status` node emits only a
  **connector-level** uevent, which KWin ignores — kwin 6.8
  `src/backends/drm/drm_backend.cpp:174` monitors `("drm", "drm_minor")` and
  `:190-191` drops every sysname that is not a `cardN` primary node, so the output
  stays invisible to KScreen ("Output with name or uuid DP-2 not found") and the
  session dies later as a 503 on the mode set. Write `change` to
  `/sys/class/drm/<card>/uevent` after forcing the connector
  (`notify_compositor()` in vdd_utils.cpp), and treat "KScreen lists the output" as
  the only proof it is usable — a connector left `enabled` while `disconnected`
  keeps a stale encoder that proves nothing. That activation is one-shot: racing it
  with the pidfd master-steal SETCRTC makes KWin fail its atomic commit and drop
  the output permanently. Compositor first, steal only for compositors that cannot
  be observed (`wait_for_compositor_output` in vdd_utils.cpp). Forced-off still
  reads as plain "disconnected" in sysfs — enumeration must consult
  `vdd_utils::offlined_physical_connectors()`.
- kscreen-doctor exits 0 on failures (`Output ... not found`, `applying config
  failed!`) — verdicts must parse the output text, never the exit code alone.
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
  `v0.9.3-linux-upstream-sync` ladder — continue it). User reads Chinese.
- Commit messages: English, conventional-commit style (`feat(linux): …`).
- Do not delete or "clean up" the two MD docs above; update them when landing work.
