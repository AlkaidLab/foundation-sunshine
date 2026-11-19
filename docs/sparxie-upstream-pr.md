# Upstream proposal for Sparxie: a session without the touch interface (`touch_ui_enabled`)

Status: patch written and tested on a local branch (`feat/pc-ui-session` in `/scratch/sparxie-src`, worktree `/scratch/sparxie-pcui`, one commit on `origin/main` 031efe2; the older local `main` of that checkout is a different history and was not used). Nothing was published.
Sparxie's licence is a proprietary draft: the patch is offered to the maintainer (ShadowLemoon) as a proposal, and is not copied anywhere else.

## Why

Sparxie's Genshin and Star Rail runtime is `winTEuser/Genshin_StarRail_fps_unlocker` (MIT, from `34736384/genshin-fps-unlock`).
In that project the game's mobile interface is optional: `bool Use_mobile_UI = 0;` is the default and the interface is only installed with
the `-EnableMobileUI` argument (Star Rail's `Hksr_ENmobile_get_Ptr` runs only when `Use_mobile_UI` is set; Genshin's Mobile UI hook table is
only scanned and filled under `if (Use_mobile_UI)`).

Sparxie's `sparxie_hoyo_bootstrap` (commit `ef92d9f`, "bootstrap 补齐触屏载荷") sets `Use_mobile_UI = 1` for both games and fails the whole
session when a touch pattern is missing. So the frame-rate unlock, the background frame rate, the priority and the Genshin preset mapping can
only be used together with the touch interface. A person who plays with keyboard, mouse or a gamepad and only wants the unlock has no way to
get it from Sparxie. The runtime supports it; only the option is missing.

## The change (27 files in one commit: +730/-21, of which 600 lines are tests)

One new optional setting, `touch_ui_enabled`, default **true** (today's behaviour), carried through every layer:

| Layer | Change |
|---|---|
| `config.json` (`Sparxie.Contracts` `HoyoProfileSettings`) | `TouchUiEnabled` (bool, default `true`) → JSON `hoyo.touchUiEnabled`. A profile without the field is a touch profile. |
| `broker.proto` `HoyoSettings` | `optional bool touch_ui_enabled = 11;` (proto3 `optional`: *absent* is distinguishable from `false`, so an older client that never sends it keeps the touch interface). |
| `ProfileSnapshotMapper`, `GameSession.ToModel` | copy the field; the receiving side reads `!HasTouchUiEnabled \|\| TouchUiEnabled`. The SessionHost gets it through the existing `SPARXIE_PROFILE_JSON` (protobuf JSON keeps an explicit `false`). |
| `ProfileManager` / `ProfileMutation` (`LauncherCore`) | `TouchUiEnabled` mutation, copied by `Clone`. No Desktop UI change (the editor can add a switch later; the field already round-trips). |
| `hoyo_touch_core_abi.h` `HoyoLaunchRequest` | two `int32_t` appended at the end: `touch_ui_disabled`, `reserved0`. x64 size 72 → 80. `HOYO_LAUNCH_REQUEST_SIZE_V1 = 72`. The C field is stored **inverted** (0 = install the touch interface, today's behaviour), so a zero-initialised request from a caller that does not know the field is still a touch session; the profile/proto/JSON side stays positive (`touch_ui_enabled`, default true). |
| `hoyo_adapter.cpp` | `hoyo_create_session` accepts `size >= 72`; a V1 caller gets `touch_ui_disabled = 0`. Only the bytes the caller owns are copied (`memcpy` of `min(size, sizeof)`), the V1 tail padding (offset 68, where the new field sits) is never read for a V1 caller. `touch_ui_disabled` must be 0 or 1. |
| `main.cpp` `sparxie_hoyo_bootstrap` | new trailing parameter `touch_ui_enabled`; `want_touch_ui` replaces the hard-coded `Use_mobile_UI = 1`. See below. |
| `BrokerService.Ping` | `PingResponse.capabilities` (new `repeated string`, field 3) lists `"touch-ui-switch"`. |
| package | `capabilities.json` (`{"schema":1,"capabilities":["touch-ui-switch"]}`) beside the binaries in `app-<version>`, for tools that start Sparxie and must know before they run it. |

`HOYO_ABI_VERSION` stays 1: the struct only grows at its end and the size field already says what the caller provided, so there is no
incompatible change. (`hoyo_get_abi_version` is unchanged.)

### What `touch_ui_enabled = false` does in the runtime

* `Use_mobile_UI = 0`, which is the upstream default.
* Star Rail: `Hksr_ENmobile_get_Ptr` (the Bootui pointer, which needs `GameAssembly.dll`) is not called, so `injectarg.Bootui` stays 0 and `inject_patch` writes no UI pointer.
* Genshin: the Mobile UI scan block is skipped and `GI_Func` stays all-zero, which is exactly what upstream `main()` does with `Use_mobile_UI = 0` (`injectarg.PfuncList = &GI_Func` is still set, so the FPS patch is installed as before). The "touch pattern outdated → fail the session" check applies only when the touch interface was requested (`want_touch_ui && !Use_mobile_UI`).
* The FPS unlock path, background frame rate, priority and the Genshin 30/60 preset mapping are unchanged.
* `touch_ui_enabled = false` together with `fps_unlock_enabled = false` installs nothing. `hoyo_create_session` refuses it (`HOYO_ERR_CONFIG_INVALID`), the Broker refuses it (`touchUiEnabled` and `fpsUnlockEnabled` cannot both be false), and `bootstrap` itself returns 1.
* Small addition used by the tests: `sparxie_hoyo_last_stage()` (exported) reports the stage bootstrap reached (2 touch scan, 3 FPS scan, 4 patch, 7 engine module load). The adapter maps a failure to the matching `HOYO_STAGE_*` (before, every failure said "touch scan"); `HOYO_STAGE_LOAD_ENGINE = 7` is new.

## Compatibility

| Client / Host | Broker / Host / DLL | Result |
|---|---|---|
| old launcher or GUI (no field) | new | field absent → touch interface, as today |
| new, field `true` or absent | old | the old Broker ignores field 11 and starts the touch interface (same as before) |
| new, field `false` | old | the old Broker ignores it **and starts the touch interface**: callers must check `Ping.capabilities` for `touch-ui-switch` first (the host launcher does and refuses with exit code 11) |
| new SessionHost, old `HoyoTouchCore.dll` | – | the old DLL accepts the larger `size`, ignores the tail, starts the touch interface (shipped together, so not expected) |
| V1 `HoyoLaunchRequest` (72 bytes) | new DLL | accepted; `touch_ui_disabled` defaults to 0 (touch interface) |
| `config.json` with `touchUiEnabled` | old GUI | System.Text.Json ignores the unknown member; the old GUI drops it on its next save |

## Tests (all in the patch)

* `LauncherCore.Tests`: profile → snapshot (true/false), protobuf-JSON round trip keeps an explicit `false`, a legacy snapshot has the field absent, `ProfileManager` mutation and clone, ZZZ refuses the setting.
* `SessionHost.Tests` (new project, `InternalsVisibleTo`): snapshot → model (absent = true), model → `HoyoLaunchRequest` (`TouchUiDisabled` 0/1), the struct's size and offsets (80 / 64 / 68 / 72).
* `HoyoAbi.Tests`: V1-size and too-small requests, `touch_ui_disabled` 2 rejected, "PC interface + no unlock" rejected, the existing pure-touch request (unlock off, switch untouched) still accepted, and a launch against a fake Star Rail directory shows the switch reaches the runtime: with the touch interface the session fails at the touch scan (stage 2, `GameAssembly.dll` missing), without it the scan is skipped and the session fails later at the engine module load (stage 7).
* `Broker.Tests`: `ProfileSnapshotValidator` cases; a real Broker with a fake SessionHost (`SPARXIE_SESSIONHOST_EXE`) shows `touch_ui_enabled` false, true and absent arriving in `SPARXIE_PROFILE_JSON`; `Ping` lists `touch-ui-switch`.
* `Config.Tests`: the package audit requires `capabilities.json`.

### Run on a Windows VM (2026-11-11, from an ssh session, nothing in a desktop session)

* Unit/ABI/integration tests of the patched tree: LauncherCore 34, SessionHost 7, HoyoAbi 20, Broker 51 (incl. the fake-SessionHost cases), Config 28, Zzz 6, all passing (native DLL built with MSVC 14.44, .NET 10.0.400).
* Launcher → patched Broker → fake SessionHost: a profile with `touchUiEnabled: false` reaches the host as `false`, `true` as `true`, a profile without the field is sent as `true`.
* Launcher → the previously installed preview.9 Broker (no `capabilities`): the `false` profile is refused before any host starts; the `true` profile starts and ends normally.
* Launcher → patched Broker → real patched SessionHost and `HoyoTouchCore.dll` with a fake `StarRail.exe` (no `GameAssembly.dll`/`UnityPlayer.dll`): the touch profile fails at stage 2 (touch scan), the PC-interface profile skips that and fails at stage 7 (engine module load) — the switch changes what the runtime does.

## What is not part of the patch

* No Sparxie Desktop UI. A switch "touch interface" in the profile editor is the natural follow-up (the field and `ProfileMutation.TouchUiEnabled` are ready).
* The real game check (Genshin / Star Rail on a PC with the PC interface and an unlocked frame rate) needs a person at the machine; the tests above cover the data path and the runtime branch, not the game's behaviour.
* The same anti-cheat caveat as the rest of Sparxie applies; this change does not add any new injection, it removes one.

## Proposed pull request text (Chinese, for the maintainer)

**标题**：feat: 新增 `touch_ui_enabled`，支持不带触屏界面的会话（键鼠 / 手柄也能用帧率解锁）

**说明**：
HoyoTouchCore 的上游运行时（Genshin_StarRail_fps_unlocker）里，移动端界面本来是可选的（`Use_mobile_UI` 默认 0，`-EnableMobileUI` 才开启）。
`sparxie_hoyo_bootstrap` 把它固定成了 1，所以帧率解锁、后台帧率、优先级这些功能只能和触屏界面一起用。这个补丁加一个可选设置
`touch_ui_enabled`（默认 `true`，行为和现在完全一样）：`false` 时不安装触屏载荷（原神 Mobile UI 钩子表 / 星铁 Bootui），游戏保持 PC 界面，其余功能照常。

**改动**：
- `HoyoProfileSettings.TouchUiEnabled` / `hoyo.touchUiEnabled`；`broker.proto` `HoyoSettings` 新增 `optional bool touch_ui_enabled = 11`（字段缺省按 `true`）。
- `HoyoLaunchRequest` 末尾追加 `touch_ui_disabled`（取反存放，全零 = 现有行为）、`reserved0`（x64 72 → 80 字节）；`hoyo_create_session` 接受 `size >= 72`，旧调用方缺省为 0，ABI 版本不变。
- bootstrap 增加 `touch_ui_enabled` 参数，替换写死的 `Use_mobile_UI = 1`；两项都关（无触屏、无 FPS 解锁）时没有可安装的内容，拒绝启动。
- `Ping` 返回 `capabilities: ["touch-ui-switch"]`，包内附 `capabilities.json`，方便外部工具判断当前 Sparxie 是否支持。
- 新增 `sparxie_hoyo_last_stage()`，让失败阶段更准确（此前一律报「触屏扫描」）。
- 没有改 Sparxie 界面；单元测试、ABI 测试、Broker 与假 SessionHost 联测均已补充。

**兼容性**：旧客户端不发该字段 = 触屏（现状）；旧 Broker 会忽略该字段，所以外部工具应先看 `Ping.capabilities`。

**未验证**：真实游戏里 PC 界面 + 帧率解锁的实机效果。
