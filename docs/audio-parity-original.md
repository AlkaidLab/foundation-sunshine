# Audio and microphone: parity with the original Foundation Sunshine

Checked on 2026-11-12 against the upstream sources (read only; behaviour is described here, no code is copied):

- host and Web UI: <https://github.com/qiin2333/foundation-sunshine>, `master` at 6225dcb (2026-11-12), called **host-up** below;
- control panel: <https://github.com/qiin2333/sunshine-control-panel>, `main` at 502a671, called **panel-up**.

Our product is a superset of the original (the native-superset principle): nothing the original offers may be missing or harder to reach.

## 1. What the original offers

| # | Capability | Where (file:lines) |
|---|---|---|
| 1 | `audio_sink`: which playback device is captured. A text field; the help sends the user to `tools\audio-info.exe` for ids. Empty = the system default. | host-up `src_assets/common/assets/web/configs/tabs/AudioVideo.vue:148-171`; default `src/config.cpp:518`; read `src/config.cpp:1488` |
| 2 | `virtual_sink`: the device the host switches to while streaming (Windows). Empty = Steam Streaming Speakers. **On the page itself**, not behind an advanced section. | `AudioVideo.vue:174-187`; `src/config.cpp:519,1489`; logic `src/audio.cpp:498-560`, `src/platform/windows/audio.cpp:875-899`; docs `docs/configuration.md:783-811` |
| 3 | `keep_sink_default`: lock the speaker, i.e. keep the selected device the Windows default for the whole stream and select it again when another program changes it (Windows). Also on the page. | `AudioVideo.vue:189-195`; `src/config.cpp:520,1490`; `src/audio.cpp:536-548`; `src/platform/windows/audio.cpp:1022`; docs `docs/configuration.md:813-826` |
| 4 | `install_steam_audio_drivers`: install the Steam Streaming Speakers driver when it is missing (Windows). On the page. | `AudioVideo.vue:200-209`; `src/config.cpp:1499`; `src/platform/windows/audio.cpp:1518-1585,1750-1752` |
| 5 | `stream_audio` and `stream_mic` switches. | `AudioVideo.vue:214-228`; `src/config.cpp:1497-1498` |
| 6 | Microphone mode `microphone_redirect_backend`: a select with four modes, automatic / USB/IP (experimental) / VB-Cable / off. The default is `vb_cable`. Visible on the page. | `AudioVideo.vue:229-252`; `src/config.cpp:521,1491-1496` |
| 7 | VB-Cable download: a button (shown for the VB-Cable and automatic modes) that opens `https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip` after a confirmation. | `AudioVideo.vue:72,254-266,400-405` |
| 8 | VB-Cable install, the part that actually installs: `install-vsink.bat`, run **by hand as administrator** (checks `net session`), detects an existing install (registry `HKLM\SOFTWARE\VB-Audio\Cable`, audio endpoints, MMDevices), downloads the zip with `Invoke-WebRequest`, unpacks it with `Expand-Archive` and runs `VBCABLE_Setup_x64.exe /S`. No signature or hash check. The Windows installer ships the script and says to run it as administrator; the portable installer asks Y/N and runs it. | `src_assets/windows/misc/vsink/install-vsink.bat:4-9,13-35,39-55`; `cmake/packaging/sunshine.iss.in:287-291`; `src_assets/windows/misc/install_portable.bat:132-152` |
| 9 | The host itself, when the microphone is used and VB-Cable is missing: downloads and unpacks the same zip into the temp directory and logs "run VBCABLE_Setup_x64.exe as administrator". It does not install. | `src/platform/windows/mic_write.cpp:509-574` |
| 10 | Microphone test button and status block. | `AudioVideo.vue:268-300`; `src/confighttp/diagnostics.cpp` (`/api/microphone/test`, `/api/microphone/status`) |
| 11 | MAT live (spatial audio) settings on the audio page. | `AudioVideo.vue:197`, `configs/tabs/audiovideo/MatLiveSettings.vue` |
| 12 | Per-app or per-client speaker choice, or "start the stream with speaker X": **none.** The only per-stream audio facts are the client's "play on host" choice (`localAudioPlayMode`, `src/nvhttp.cpp:377,567`) and `SUNSHINE_CLIENT_HOST_AUDIO` for scripts (`src/nvhttp.cpp:271`). "Speaker X at stream start" is `virtual_sink` (item 2) plus the lock (item 3). | `src/nvhttp.cpp`, `src/audio.cpp:498-560` |
| 13 | Panel: the microphone page **shows** the configured and the active backend (read only), a test button and a diagnostics fold; it has **no** backend picker, no VB-Cable install or enable and no speaker settings: a button opens the Web UI (`/config/#av`) for all of that. | panel-up `src/renderer/components/controllersHub/MicrophonePanel.vue:18-19,22-27,110-116`; `src-tauri/src/controller_hub.rs:52,63` |

## 2. Parity table

State of **our** product before this round (feat/audio-devices at `1f7d1fa08` / Panel `11f868f`), then what was done.

| # | Original | Ours before (Web / Panel) | Verdict | Now |
|---|---|---|---|---|
| 1 audio_sink | text field | picker over the real devices, typed id kept / same page in the frame | **better**, but titled 音频采集设备 | renamed 采集哪个扬声器 |
| 2 virtual_sink | on the page | **behind 高级** (an expert row) / same | **worse** | on the page, 串流时切换到的扬声器, picker over the devices |
| 3 keep_sink_default | on the page | **behind 高级**, titled 强制保持音频输出设备 / same | **worse** | on the page, 锁定扬声器 |
| 4 install_steam_audio_drivers | on the page | **behind 高级** / same | **worse** | on the page |
| 5 stream_audio, stream_mic | switches | same | same | same |
| 6 microphone mode | select, all four modes | a list row (dialog) whose names were sentences; Panel: a four-button group | **worse** on the Web page (the modes were one click deeper) | four buttons, 麦克风方式, on the microphone page of the Web UI and of the Panel; the line below says what the chosen mode does |
| 7 VB-Cable download | confirm, then the zip link | confirm, then the zip link (shown for every backend); Panel: the VB-Audio page | same | kept as the manual way |
| 8 VB-Cable install | script run by hand as administrator, silent, no signature check | **missing**: only the download | **missing** | **install** from the row, asked first: the host downloads from VB-Audio, checks the Authenticode signature (publisher Vincent Burel / VB-Audio) and runs the setup silently; **enable** a disabled device with `pnputil /enable-device`, asked first |
| 9 host auto-download at mic start | downloads and unpacks, asks for a manual install | the same code | same | the install above replaces the manual step |
| 10 mic test and status | yes | yes | same | same |
| 11 MAT live | on the audio page | on the display advanced page (`DisplayAdvanced.vue`) | different place; not changed | not changed |
| 12 per-app / per-client speaker | none | none | same | none (not in the original) |
| 13 Panel microphone page | read-only backend, opens the Web UI | backend picker, state and test | **better** | VB-CABLE row with install, enable and download, device manager |

## 3. Differences kept on purpose

- Our default `microphone_redirect_backend` is `auto` (USB/IP first, VB-Cable as the fallback); the original's is `vb_cable`.
- The install runs inside the host service (already elevated), not in a script run by hand; the UI asks first and the request must carry `confirm: true`. The signature check is new.
- Not verified on a Windows test host by installing: nothing was installed or enabled there (see the report); the steps are tested with fakes, and the signature check ran against the setup program of the pack that is already on that machine.
