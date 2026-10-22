# Foundation Sunshine

[简体中文](../../README.md) · English · [Deutsch](README.de.md) · [Français](README.fr.md) · [日本語](README.ja.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../../assets/readme/hero-dark.png">
  <img src="../../assets/readme/hero.png" width="800" alt="Foundation Sunshine — HDR game streaming host for Windows with virtual displays, a Sunshine fork for Moonlight">
</picture>

[![Build](https://img.shields.io/github/actions/workflow/status/AlkaidLab/foundation-sunshine/main.yml?branch=master&label=Build)](https://github.com/AlkaidLab/foundation-sunshine/actions/workflows/main.yml) [![Latest release](https://img.shields.io/github/v/release/AlkaidLab/foundation-sunshine?include_prereleases&sort=date&label=Latest)](https://github.com/AlkaidLab/foundation-sunshine/releases) [![Downloads](https://img.shields.io/github/downloads/AlkaidLab/foundation-sunshine/total?label=Downloads)](https://github.com/AlkaidLab/foundation-sunshine/releases) [![GitHub Stars](https://img.shields.io/github/stars/AlkaidLab/foundation-sunshine?style=flat&label=Stars)](https://github.com/AlkaidLab/foundation-sunshine) [![License](https://img.shields.io/badge/License-GPL--3.0-blue)](https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE)

[Website](https://www.alkaidlab.com/) · [Website (China)](https://www.alkaidlab.cn/) · [QQ community (Chinese)](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi) · [Report an issue](https://github.com/AlkaidLab/foundation-sunshine/issues)

</div>

Foundation Sunshine is a free, open-source remote-control and game-streaming host for Windows, built as an enhanced fork of [Sunshine](https://github.com/LizardByte/Sunshine). With a Moonlight client, you can use your PC desktop, run applications, and play PC games from a phone, tablet, TV, Mac, or another computer.

From dynamic HDR and display calibration to low-latency capture and hardware encoding, Foundation keeps remote video and input close to local. Surround sound and microphone forwarding, native DualSense feedback, clipboard sync, and shared folders work alongside the session, and an independent desktop GUI offers a better experience by bringing the previously scattered settings, device components, and game libraries into one place.

**[Download the host (Sunshine)](https://github.com/AlkaidLab/foundation-sunshine/releases/latest)** · **[Download a client (Moonlight)](#clients-moonlight)** · **[Install and connect](#quick-start)**

[Highlights](#highlights) · [Features and setup](../en/features.md) · [FAQ](../en/faq.md) · [Development](../en/development.md) · [Contributing](../contributing.en.md)

## Highlights

- **Video and displays:** Scene-adaptive HDR, HDR10+ / HDR Vivid and Dolby Vision extensions, with brightness calibration. [ZakoDisplay](https://github.com/qiin2333/zako-vdd) provides HDR virtual displays and multiple display layouts without a physical monitor.
- **Capture and encoding:** NVIDIA NVENC, AMD AMF, and Intel Quick Sync, with capture and encoding optimizations for lower latency and smoother sessions. Zako Direct takes virtual display frames directly to reduce capture-stage copies.
- **Audio and microphone forwarding:** Up to 7.1.4 surround, PCM / AC3 / E-AC3, microphone forwarding, continuous audio during silence, and audio packet-loss recovery.
- **Independent desktop control panel:** Manage game libraries, displays, device components, runtime status, and updates, with log diagnostics and optional AI assistance.
- **Input and native controllers:** Precision touchpads, pen input, and client-side cursors. Install the DualSense component in the device center to bring supported games' USB-connected PC feedback, including adaptive triggers and haptics, into remote sessions.
- **Desktop integration and session control:** Bidirectional clipboard, shared folders, USB forwarding, live video adjustments, adaptive bitrate, host commands, and session controls.

See [features and setup](../en/features.md) for requirements and activation steps — the developers also hid plenty of small touches to discover.

## Downloads

You can also use the [website](https://www.alkaidlab.com/) or the [Website (China)](https://www.alkaidlab.cn/) to find downloads for your devices.

Install an application on each device: **Foundation Sunshine on the PC you want to control, and a Moonlight client on the device you will use to control it.**

### Host (Sunshine)

**Install on the Windows PC you want to control.**

If Foundation Sunshine helps you, consider giving the project a [⭐ Star](https://github.com/AlkaidLab/foundation-sunshine).

Open the [latest regular release](https://github.com/AlkaidLab/foundation-sunshine/releases/latest), expand **Assets**, and download the package whose name ends in `WindowsInstaller.exe`.

- **First installation:** Use the installer and follow the setup wizard.
- **Portable deployment (not recommended):** Choose `Portable-x64.zip`; the filename may include a version prefix. Use it only for special needs such as running multiple instances.
- **Trying new features:** Browse [pre-releases](https://github.com/AlkaidLab/foundation-sunshine/releases) and read their known issues before installing.

Start with Windows 10 22H2 or newer and an AMD, Intel, or NVIDIA GPU with hardware video encoding. Available image quality, resolution, and frame rate depend on your GPU, drivers, network, and client.

### Clients (Moonlight)

**Install on the phone, tablet, or other computer you will use to control the PC.**

**We recommend the clients listed below.** Foundation's HDR, display management, audio, and input enhancements require client support. Other clients may lack these features or encounter compatibility issues such as incorrect video output or connection errors.

These clients are crafted with care by AlkaidLab members to provide the best Foundation Sunshine experience. Support the projects you use with a **⭐ Star**.

- **Windows / Linux:** [Moonlight V+ for PC](https://github.com/qiin2333/moonlight-qt/releases)
- **macOS:** [Moonlight macOS Enhanced](https://github.com/skyhua0224/moonlight-macos-enhanced/releases) · [Moonlight V+ for PC](https://github.com/qiin2333/moonlight-qt/releases)
- **Android / Android TV:** [Moonlight V+](https://github.com/qiin2333/moonlight-vplus/releases/latest)
- **HarmonyOS:** [Moonlight V+](https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream)

#### Partner clients

- **VoidLink · iOS / iPadOS:** [About](https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm#readme) · [App Store for Mainland China, South Korea, and Vietnam](https://apps.apple.com/app/voidlink/id6747717070) · [App Store for other regions](https://apps.apple.com/app/voidlink-extreme/id6755103808) (paid store distribution)
- **VoidLink TestFlight Beta · Apple TV support:** [Join the TestFlight beta](https://testflight.apple.com/join/k7gs61mZ)
- **Crown Edition · Android:** [Download](https://github.com/WACrown/moonlight-android/releases) · [About](https://github.com/WACrown/moonlight-android#readme)
- **Baozi Edition · HarmonyOS:** [Project and downloads](https://gitee.com/smdsbz/moonlight-ohos)

## Quick start

### 1 Install the host (Sunshine)

1. On the Windows PC you want to control, open the [download page](https://github.com/AlkaidLab/foundation-sunshine/releases/latest).
2. Find **Assets** and download the file ending in `WindowsInstaller.exe`, then run it. Keep the installer components you may need; keep **Zako Display Driver** selected if you plan to use a virtual display.
3. Open the Foundation Sunshine desktop control panel and follow the setup wizard to choose language, GPU, and display. If a physical display is connected, select it first. For a headless PC, first check whether the installer installed the virtual display driver; if it is missing, open the virtual display settings in the GUI and install it, then select the Foundation display.
4. **(Optional, not recommended)** If you prefer the WebUI, open [https://localhost:47990](https://localhost:47990) in a browser on this PC. If the browser warns about the certificate, confirm the address is `https://localhost:47990` on this PC, then continue through the browser's advanced or "more options" entry.
5. Follow the prompts to create a username and password; you can use them to sign in to the WebUI management interface in the future.

### 2 Install a client (Moonlight)

On a phone, tablet, or another computer, download and install a client from the [Clients (Moonlight) list above](#clients-moonlight). This device shows the video and sends input.

- **Windows:** Choose the Windows installer on the client release page. If you pick the portable ZIP (not recommended; use it only for special needs such as multiple instances), extract and run it; the filename may include a version prefix.
- **macOS:** Download the DMG of the client you chose, and drag the app into Applications. V+ prebuilt packages are Apple Silicon only; Enhanced offers a universal package — if you know your Mac's architecture you can also pick the matching build. If macOS blocks the first launch, follow that project's installation notes.
- **Linux:** Download an AppImage matching your processor and distribution, make it executable, and run it.
- **Android:** Download the APK and open it from the download notification or a file manager; allow installation when prompted.
- **iOS / iPadOS, HarmonyOS:** Open the store link and install.

Allow local network access when the client asks. Keep both devices on the same LAN for the first connection; a wired connection is recommended for the host PC.

### 3 Add the PC and pair

1. Open the client and wait for the Windows PC to appear in the list, then select it.
2. If it does not appear, add the IP manually: on the Windows PC press `Win + R`, type `cmd` and press Enter, then run `ipconfig`. Find the **IPv4 Address** of the active Ethernet or WLAN adapter and enter it in the client's "add PC / add host" entry.
3. When the client shows a PIN, go back to the Sunshine management interface on the Windows PC and open the **PIN** page.
4. Enter the PIN shown on the client and submit it. After the success message, return to the client.

Enter the Windows PC's LAN address, for example `192.168.1.100` — not `localhost` and not the management page URL. Replace the example with your PC's actual address.

### 4 Set video options for the first connection

Open settings in your **Moonlight client**, find video or quality options, and set:

| Option | First-time value | What it means |
| --- | --- | --- |
| Resolution | 1920×1080 (1080p) | Size of the transmitted image; higher is sharper but needs more network and processing |
| Frame rate | 60 FPS | Screen updates per second; 120 FPS needs a suitable display and headroom on both ends |
| Bitrate | 20 Mbps; drop to 10 Mbps if the network is unstable | Data budget affecting compression quality and network load |
| Video codec | Automatic; choose H.264 first if you hit compatibility issues | Compression format; HEVC / AV1 need matching decoder support |
| HDR | Off first | SDR by default; configure HDR after everything works |
| Audio | Stereo / Opus | Confirm sound first, then enable surround or other formats |

Names vary slightly by client. Set resolution, frame rate, and bitrate on the client; leave the host on automatic encoding and default capture at first. Avoid enabling high resolution, high frame rate, and every enhancement at once.

### 5 Connect to the desktop

1. Open the paired PC in the client and choose **Desktop**.
2. Once the Windows desktop appears, move the mouse, open a window, and play some audio to confirm video, input, and sound.
3. If Desktop is missing, use **Restore built-in apps** on Sunshine's **Applications** page, then refresh the client's app list. This restores the default built-in entries; preserve your customizations first if you changed them.

Get through this step with default video settings, then adjust HDR, resolution, or virtual displays.

### 6 Add a game or application (optional)

**Batch scan:** On Windows, open the Foundation Sunshine desktop control panel, go to **Applications**, and click **Scan game platform libraries (Steam/Epic/GOG)**. Review the results, choose **Quick add** or **Add all**, then refresh the client list. Local folders can be added from the scan dialog too; the browser management page cannot access the desktop-side local scan.

**Manual add:** In **Applications → Add new app**, fill in the app name and **Command**, save, and refresh the client list. The command accepts a program path; wrap paths containing spaces in double quotes, for example `"D:\My Games\Game\game.exe"`.

You can also launch the built-in **Steam Big Picture** entry from the client and pick a game in Steam.

## Features and setup

Each topic brings capabilities, requirements, and setup instructions together:

[Video HDR and color](../en/features.md#hdr-and-color) · [Foundation virtual displays](../en/features.md#foundation-virtual-displays-zakodisplay) · [Capture and encoding](../en/features.md#capture-encoding-and-performance) · [Controllers and DualSense](../en/features.md#dualsense-and-virtual-controllers) · [Audio and microphone forwarding](../en/features.md#audio-and-microphone-forwarding) · [Desktop control panel](../en/features.md#desktop-control-panel)

## Help and participation

- [FAQ and troubleshooting](../en/faq.md): stutter, frame drops, connections, audio, and unwanted scrolling after installing the DualSense component.
- [Upgrading](../en/upgrading.md): regular and test releases, preserving configuration, and checking an update.
- [Documentation](../en/README.md): user and developer guides organized by task.
- [Website](https://www.alkaidlab.com/) · [Website (China)](https://www.alkaidlab.cn/) · [QQ community (Chinese)](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi).
- [Report a problem](https://github.com/AlkaidLab/foundation-sunshine/issues) · [Discussions](https://github.com/AlkaidLab/foundation-sunshine/discussions) · [Contributing](../contributing.en.md).

Thanks to [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine), [Moonlight](https://moonlight-stream.org/), [ZakoDisplay](https://github.com/qiin2333/zako-vdd), and all code, testing, and translation contributors. Licensed under [GPL-3.0](https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE).

Enjoying it? Leave a Star. Not that we were checking or anything.

## AlkaidLab development team

| [![qiin2333](https://avatars.githubusercontent.com/u/2795904?s=80)](https://github.com/qiin2333) | [![Yundi339](https://avatars.githubusercontent.com/u/38252824?s=80)](https://github.com/Yundi339) | [![ShadowLemoon](https://avatars.githubusercontent.com/u/119576779?s=80)](https://github.com/ShadowLemoon) | [![skyhua0224](https://avatars.githubusercontent.com/u/91328606?s=80)](https://github.com/skyhua0224) |
| :---: | :---: | :---: | :---: |
| [qiin2333](https://github.com/qiin2333) | [Yundi339](https://github.com/Yundi339) | [ShadowLemoon](https://github.com/ShadowLemoon) | [skyhua0224](https://github.com/skyhua0224) |

## Other contributors

Thanks to everyone contributing code, testing, and maintenance to Foundation and upstream Sunshine.

<!-- CONTRIBUTORS:START -->
<p align="center">
  <a href="https://github.com/ReenigneArcher"><img src="https://avatars.githubusercontent.com/u/42013603?v=4&amp;s=80" width="40" height="40" alt="ReenigneArcher" title="ReenigneArcher"></a>
  <a href="https://github.com/cgutman"><img src="https://avatars.githubusercontent.com/u/2695644?v=4&amp;s=80" width="40" height="40" alt="cgutman" title="cgutman"></a>
  <a href="https://github.com/apps/dependabot"><img src="https://avatars.githubusercontent.com/in/29110?v=4&amp;s=80" width="40" height="40" alt="dependabot[bot]" title="dependabot[bot]"></a>
  <a href="https://github.com/loki-47-6F-64"><img src="https://avatars.githubusercontent.com/u/3084139?v=4&amp;s=80" width="40" height="40" alt="loki-47-6F-64" title="loki-47-6F-64"></a>
  <a href="https://github.com/ns6089"><img src="https://avatars.githubusercontent.com/u/61738816?v=4&amp;s=80" width="40" height="40" alt="ns6089" title="ns6089"></a>
  <a href="https://github.com/LizardByte-bot"><img src="https://avatars.githubusercontent.com/u/108553330?v=4&amp;s=80" width="40" height="40" alt="LizardByte-bot" title="LizardByte-bot"></a>
  <a href="https://github.com/TheElixZammuto"><img src="https://avatars.githubusercontent.com/u/6505622?v=4&amp;s=80" width="40" height="40" alt="TheElixZammuto" title="TheElixZammuto"></a>
  <a href="https://github.com/psyke83"><img src="https://avatars.githubusercontent.com/u/749000?v=4&amp;s=80" width="40" height="40" alt="psyke83" title="psyke83"></a>
  <a href="https://github.com/istori1"><img src="https://avatars.githubusercontent.com/u/107304850?v=4&amp;s=80" width="40" height="40" alt="istori1" title="istori1"></a>
  <a href="https://github.com/Nonary"><img src="https://avatars.githubusercontent.com/u/27069224?v=4&amp;s=80" width="40" height="40" alt="Nonary" title="Nonary"></a>
  <a href="https://github.com/apps/github-actions"><img src="https://avatars.githubusercontent.com/in/15368?v=4&amp;s=80" width="40" height="40" alt="github-actions[bot]" title="github-actions[bot]"></a>
  <a href="https://github.com/PapyKahan"><img src="https://avatars.githubusercontent.com/u/9347985?v=4&amp;s=80" width="40" height="40" alt="PapyKahan" title="PapyKahan"></a>
  <a href="https://github.com/ABeltramo"><img src="https://avatars.githubusercontent.com/u/1122217?v=4&amp;s=80" width="40" height="40" alt="ABeltramo" title="ABeltramo"></a>
  <a href="https://github.com/FrogTheFrog"><img src="https://avatars.githubusercontent.com/u/22381748?v=4&amp;s=80" width="40" height="40" alt="FrogTheFrog" title="FrogTheFrog"></a>
  <a href="https://github.com/HomerSp"><img src="https://avatars.githubusercontent.com/u/487734?v=4&amp;s=80" width="40" height="40" alt="HomerSp" title="HomerSp"></a>
  <a href="https://github.com/kodemeister"><img src="https://avatars.githubusercontent.com/u/1043953?v=4&amp;s=80" width="40" height="40" alt="kodemeister" title="kodemeister"></a>
  <a href="https://github.com/brad-richardson"><img src="https://avatars.githubusercontent.com/u/5924884?v=4&amp;s=80" width="40" height="40" alt="brad-richardson" title="brad-richardson"></a>
  <a href="https://github.com/Doomsdayrs"><img src="https://avatars.githubusercontent.com/u/38189170?v=4&amp;s=80" width="40" height="40" alt="Doomsdayrs" title="Doomsdayrs"></a>
  <a href="https://github.com/gschintgen"><img src="https://avatars.githubusercontent.com/u/11065156?v=4&amp;s=80" width="40" height="40" alt="gschintgen" title="gschintgen"></a>
  <a href="https://github.com/Logical-sh"><img src="https://avatars.githubusercontent.com/u/5807486?v=4&amp;s=80" width="40" height="40" alt="Logical-sh" title="Logical-sh"></a>
  <a href="https://github.com/Hazer"><img src="https://avatars.githubusercontent.com/u/4046170?v=4&amp;s=80" width="40" height="40" alt="Hazer" title="Hazer"></a>
  <a href="https://github.com/diadatp"><img src="https://avatars.githubusercontent.com/u/4490283?v=4&amp;s=80" width="40" height="40" alt="diadatp" title="diadatp"></a>
  <a href="https://github.com/KuleRucket"><img src="https://avatars.githubusercontent.com/u/737724?v=4&amp;s=80" width="40" height="40" alt="KuleRucket" title="KuleRucket"></a>
  <a href="https://github.com/chewi"><img src="https://avatars.githubusercontent.com/u/35072?v=4&amp;s=80" width="40" height="40" alt="chewi" title="chewi"></a>
  <a href="https://github.com/guanzhangrtk"><img src="https://avatars.githubusercontent.com/u/44019025?v=4&amp;s=80" width="40" height="40" alt="guanzhangrtk" title="guanzhangrtk"></a>
  <a href="https://github.com/nowrep"><img src="https://avatars.githubusercontent.com/u/522831?v=4&amp;s=80" width="40" height="40" alt="nowrep" title="nowrep"></a>
  <a href="https://github.com/sitiom"><img src="https://avatars.githubusercontent.com/u/56180050?v=4&amp;s=80" width="40" height="40" alt="sitiom" title="sitiom"></a>
  <a href="https://github.com/luk1337"><img src="https://avatars.githubusercontent.com/u/6830979?v=4&amp;s=80" width="40" height="40" alt="luk1337" title="luk1337"></a>
  <a href="https://github.com/JacekJagosz"><img src="https://avatars.githubusercontent.com/u/28653965?v=4&amp;s=80" width="40" height="40" alt="JacekJagosz" title="JacekJagosz"></a>
  <a href="https://github.com/jorys-paulin"><img src="https://avatars.githubusercontent.com/u/5084309?v=4&amp;s=80" width="40" height="40" alt="jorys-paulin" title="jorys-paulin"></a>
  <a href="https://github.com/tez011"><img src="https://avatars.githubusercontent.com/u/22161268?v=4&amp;s=80" width="40" height="40" alt="tez011" title="tez011"></a>
  <a href="https://github.com/KiralyCraft"><img src="https://avatars.githubusercontent.com/u/10259742?v=4&amp;s=80" width="40" height="40" alt="KiralyCraft" title="KiralyCraft"></a>
  <a href="https://github.com/pgrunzjr"><img src="https://avatars.githubusercontent.com/u/3165068?v=4&amp;s=80" width="40" height="40" alt="pgrunzjr" title="pgrunzjr"></a>
  <a href="https://github.com/thatsysadmin"><img src="https://avatars.githubusercontent.com/u/65380846?v=4&amp;s=80" width="40" height="40" alt="thatsysadmin" title="thatsysadmin"></a>
  <a href="https://github.com/WisdomCode"><img src="https://avatars.githubusercontent.com/u/26511487?v=4&amp;s=80" width="40" height="40" alt="WisdomCode" title="WisdomCode"></a>
  <a href="https://github.com/safijari"><img src="https://avatars.githubusercontent.com/u/5191844?v=4&amp;s=80" width="40" height="40" alt="safijari" title="safijari"></a>
  <a href="https://github.com/korverdev"><img src="https://avatars.githubusercontent.com/u/84342833?v=4&amp;s=80" width="40" height="40" alt="korverdev" title="korverdev"></a>
  <a href="https://github.com/tgurr"><img src="https://avatars.githubusercontent.com/u/414984?v=4&amp;s=80" width="40" height="40" alt="tgurr" title="tgurr"></a>
  <a href="https://github.com/exetico"><img src="https://avatars.githubusercontent.com/u/3549445?v=4&amp;s=80" width="40" height="40" alt="exetico" title="exetico"></a>
  <a href="https://github.com/BigBrainAFK"><img src="https://avatars.githubusercontent.com/u/7173033?v=4&amp;s=80" width="40" height="40" alt="BigBrainAFK" title="BigBrainAFK"></a>
  <a href="https://github.com/Arbitrate3280"><img src="https://avatars.githubusercontent.com/u/26132870?v=4&amp;s=80" width="40" height="40" alt="Arbitrate3280" title="Arbitrate3280"></a>
  <a href="https://github.com/arnerak"><img src="https://avatars.githubusercontent.com/u/29173710?v=4&amp;s=80" width="40" height="40" alt="arnerak" title="arnerak"></a>
  <a href="https://github.com/detiam"><img src="https://avatars.githubusercontent.com/u/44510779?v=4&amp;s=80" width="40" height="40" alt="detiam" title="detiam"></a>
  <a href="https://github.com/dkgkdfg65"><img src="https://avatars.githubusercontent.com/u/219107372?v=4&amp;s=80" width="40" height="40" alt="dkgkdfg65" title="dkgkdfg65"></a>
  <a href="https://github.com/lns103"><img src="https://avatars.githubusercontent.com/u/49310852?v=4&amp;s=80" width="40" height="40" alt="lns103" title="lns103"></a>
  <a href="https://github.com/okkiv"><img src="https://avatars.githubusercontent.com/u/68665660?v=4&amp;s=80" width="40" height="40" alt="okkiv" title="okkiv"></a>
  <a href="https://github.com/w0utert"><img src="https://avatars.githubusercontent.com/u/30574002?v=4&amp;s=80" width="40" height="40" alt="w0utert" title="w0utert"></a>
  <a href="https://github.com/MiroKaku"><img src="https://avatars.githubusercontent.com/u/50670906?v=4&amp;s=80" width="40" height="40" alt="MiroKaku" title="MiroKaku"></a>
  <a href="https://github.com/TimmyOVO"><img src="https://avatars.githubusercontent.com/u/47740511?v=4&amp;s=80" width="40" height="40" alt="TimmyOVO" title="TimmyOVO"></a>
  <a href="https://github.com/ReeceHumphreys"><img src="https://avatars.githubusercontent.com/u/14209515?v=4&amp;s=80" width="40" height="40" alt="ReeceHumphreys" title="ReeceHumphreys"></a>
  <a href="https://github.com/mcourteaux"><img src="https://avatars.githubusercontent.com/u/845012?v=4&amp;s=80" width="40" height="40" alt="mcourteaux" title="mcourteaux"></a>
  <a href="https://github.com/Doridian"><img src="https://avatars.githubusercontent.com/u/631409?v=4&amp;s=80" width="40" height="40" alt="Doridian" title="Doridian"></a>
  <a href="https://github.com/brownj85"><img src="https://avatars.githubusercontent.com/u/35931888?v=4&amp;s=80" width="40" height="40" alt="brownj85" title="brownj85"></a>
  <a href="https://github.com/felipejfc"><img src="https://avatars.githubusercontent.com/u/285782?v=4&amp;s=80" width="40" height="40" alt="felipejfc" title="felipejfc"></a>
  <a href="https://github.com/CLF184"><img src="https://avatars.githubusercontent.com/u/57824267?v=4&amp;s=80" width="40" height="40" alt="CLF184" title="CLF184"></a>
  <a href="https://github.com/briankendall"><img src="https://avatars.githubusercontent.com/u/7917884?v=4&amp;s=80" width="40" height="40" alt="briankendall" title="briankendall"></a>
  <a href="https://github.com/abusse"><img src="https://avatars.githubusercontent.com/u/1366888?v=4&amp;s=80" width="40" height="40" alt="abusse" title="abusse"></a>
  <a href="https://github.com/perroboc"><img src="https://avatars.githubusercontent.com/u/2722773?v=4&amp;s=80" width="40" height="40" alt="perroboc" title="perroboc"></a>
  <a href="https://github.com/andygrundman"><img src="https://avatars.githubusercontent.com/u/105828?v=4&amp;s=80" width="40" height="40" alt="andygrundman" title="andygrundman"></a>
  <a href="https://github.com/CrazyStevenz"><img src="https://avatars.githubusercontent.com/u/40207026?v=4&amp;s=80" width="40" height="40" alt="CrazyStevenz" title="CrazyStevenz"></a>
  <a href="https://github.com/TrueZhuangJia"><img src="https://avatars.githubusercontent.com/u/78474576?v=4&amp;s=80" width="40" height="40" alt="TrueZhuangJia" title="TrueZhuangJia"></a>
  <a href="https://github.com/xanderfrangos"><img src="https://avatars.githubusercontent.com/u/33106561?v=4&amp;s=80" width="40" height="40" alt="xanderfrangos" title="xanderfrangos"></a>
  <a href="https://github.com/zackelia"><img src="https://avatars.githubusercontent.com/u/31748637?v=4&amp;s=80" width="40" height="40" alt="zackelia" title="zackelia"></a>
  <a href="https://github.com/zanechua"><img src="https://avatars.githubusercontent.com/u/4265429?v=4&amp;s=80" width="40" height="40" alt="zanechua" title="zanechua"></a>
  <a href="https://github.com/KawaiiZapic"><img src="https://avatars.githubusercontent.com/u/45936772?v=4&amp;s=80" width="40" height="40" alt="KawaiiZapic" title="KawaiiZapic"></a>
  <a href="https://github.com/Chang-Jin-Lee"><img src="https://avatars.githubusercontent.com/u/54494793?v=4&amp;s=80" width="40" height="40" alt="Chang-Jin-Lee" title="Chang-Jin-Lee"></a>
  <a href="https://github.com/bushshrub"><img src="https://avatars.githubusercontent.com/u/9291792?v=4&amp;s=80" width="40" height="40" alt="bushshrub" title="bushshrub"></a>
  <a href="https://github.com/RickAndTired"><img src="https://avatars.githubusercontent.com/u/53553229?v=4&amp;s=80" width="40" height="40" alt="RickAndTired" title="RickAndTired"></a>
  <a href="https://github.com/rafaelsgirao"><img src="https://avatars.githubusercontent.com/u/7544832?v=4&amp;s=80" width="40" height="40" alt="rafaelsgirao" title="rafaelsgirao"></a>
  <a href="https://github.com/radugrecu97"><img src="https://avatars.githubusercontent.com/u/39441011?v=4&amp;s=80" width="40" height="40" alt="radugrecu97" title="radugrecu97"></a>
  <a href="https://github.com/PVermeer"><img src="https://avatars.githubusercontent.com/u/23457923?v=4&amp;s=80" width="40" height="40" alt="PVermeer" title="PVermeer"></a>
  <a href="https://github.com/devusb"><img src="https://avatars.githubusercontent.com/u/4951663?v=4&amp;s=80" width="40" height="40" alt="devusb" title="devusb"></a>
  <a href="https://github.com/LXY1226"><img src="https://avatars.githubusercontent.com/u/8820178?v=4&amp;s=80" width="40" height="40" alt="LXY1226" title="LXY1226"></a>
  <a href="https://github.com/sethicis"><img src="https://avatars.githubusercontent.com/u/4684016?v=4&amp;s=80" width="40" height="40" alt="sethicis" title="sethicis"></a>
  <a href="https://github.com/SnowSwordScholar"><img src="https://avatars.githubusercontent.com/u/73566969?v=4&amp;s=80" width="40" height="40" alt="SnowSwordScholar" title="SnowSwordScholar"></a>
  <a href="https://github.com/Max-Might"><img src="https://avatars.githubusercontent.com/u/119780?v=4&amp;s=80" width="40" height="40" alt="Max-Might" title="Max-Might"></a>
  <a href="https://github.com/zoeyjodon"><img src="https://avatars.githubusercontent.com/u/76182954?v=4&amp;s=80" width="40" height="40" alt="zoeyjodon" title="zoeyjodon"></a>
  <a href="https://github.com/skryvel"><img src="https://avatars.githubusercontent.com/u/120495777?v=4&amp;s=80" width="40" height="40" alt="skryvel" title="skryvel"></a>
  <a href="https://github.com/sirius128"><img src="https://avatars.githubusercontent.com/u/68372698?v=4&amp;s=80" width="40" height="40" alt="sirius128" title="sirius128"></a>
  <a href="https://github.com/qingm36"><img src="https://avatars.githubusercontent.com/u/76875055?v=4&amp;s=80" width="40" height="40" alt="qingm36" title="qingm36"></a>
  <a href="https://github.com/manteuffel723"><img src="https://avatars.githubusercontent.com/u/40317957?v=4&amp;s=80" width="40" height="40" alt="manteuffel723" title="manteuffel723"></a>
  <a href="https://github.com/jensaymoo"><img src="https://avatars.githubusercontent.com/u/45825226?v=4&amp;s=80" width="40" height="40" alt="jensaymoo" title="jensaymoo"></a>
  <a href="https://github.com/jcj83429"><img src="https://avatars.githubusercontent.com/u/5402600?v=4&amp;s=80" width="40" height="40" alt="jcj83429" title="jcj83429"></a>
  <a href="https://github.com/iMakeSoftware"><img src="https://avatars.githubusercontent.com/u/20089877?v=4&amp;s=80" width="40" height="40" alt="iMakeSoftware" title="iMakeSoftware"></a>
  <a href="https://github.com/exalented"><img src="https://avatars.githubusercontent.com/u/9328864?v=4&amp;s=80" width="40" height="40" alt="exalented" title="exalented"></a>
  <a href="https://github.com/everything411"><img src="https://avatars.githubusercontent.com/u/32010059?v=4&amp;s=80" width="40" height="40" alt="everything411" title="everything411"></a>
  <a href="https://github.com/brycerocky"><img src="https://avatars.githubusercontent.com/u/56776312?v=4&amp;s=80" width="40" height="40" alt="brycerocky" title="brycerocky"></a>
  <a href="https://github.com/aseligmann"><img src="https://avatars.githubusercontent.com/u/40468411?v=4&amp;s=80" width="40" height="40" alt="aseligmann" title="aseligmann"></a>
  <a href="https://github.com/ahmadalli"><img src="https://avatars.githubusercontent.com/u/2167301?v=4&amp;s=80" width="40" height="40" alt="ahmadalli" title="ahmadalli"></a>
  <a href="https://github.com/aetheryx"><img src="https://avatars.githubusercontent.com/u/25572140?v=4&amp;s=80" width="40" height="40" alt="aetheryx" title="aetheryx"></a>
  <a href="https://github.com/notentered"><img src="https://avatars.githubusercontent.com/u/1017032?v=4&amp;s=80" width="40" height="40" alt="notentered" title="notentered"></a>
  <a href="https://github.com/360sym"><img src="https://avatars.githubusercontent.com/u/63459534?v=4&amp;s=80" width="40" height="40" alt="360sym" title="360sym"></a>
  <a href="https://github.com/d-s-x"><img src="https://avatars.githubusercontent.com/u/5136335?v=4&amp;s=80" width="40" height="40" alt="d-s-x" title="d-s-x"></a>
  <a href="https://github.com/Xythus"><img src="https://avatars.githubusercontent.com/u/1338909?v=4&amp;s=80" width="40" height="40" alt="Xythus" title="Xythus"></a>
  <a href="https://github.com/Crashdummyy"><img src="https://avatars.githubusercontent.com/u/15139403?v=4&amp;s=80" width="40" height="40" alt="Crashdummyy" title="Crashdummyy"></a>
  <a href="https://github.com/cgarst"><img src="https://avatars.githubusercontent.com/u/125395?v=4&amp;s=80" width="40" height="40" alt="cgarst" title="cgarst"></a>
  <a href="https://github.com/apps/copilot-swe-agent"><img src="https://avatars.githubusercontent.com/in/1143301?v=4&amp;s=80" width="40" height="40" alt="Copilot" title="Copilot"></a>
  <a href="https://github.com/CodyManess"><img src="https://avatars.githubusercontent.com/u/47895180?v=4&amp;s=80" width="40" height="40" alt="CodyManess" title="CodyManess"></a>
  <a href="https://github.com/Clutchnp"><img src="https://avatars.githubusercontent.com/u/105556048?v=4&amp;s=80" width="40" height="40" alt="Clutchnp" title="Clutchnp"></a>
  <a href="https://github.com/cfalas"><img src="https://avatars.githubusercontent.com/u/34488895?v=4&amp;s=80" width="40" height="40" alt="cfalas" title="cfalas"></a>
  <a href="https://github.com/cfajardo"><img src="https://avatars.githubusercontent.com/u/104530852?v=4&amp;s=80" width="40" height="40" alt="cfajardo" title="cfajardo"></a>
  <a href="https://github.com/cazzoo"><img src="https://avatars.githubusercontent.com/u/1876754?v=4&amp;s=80" width="40" height="40" alt="cazzoo" title="cazzoo"></a>
  <a href="https://github.com/caveat-makes-software"><img src="https://avatars.githubusercontent.com/u/59033732?v=4&amp;s=80" width="40" height="40" alt="caveat-makes-software" title="caveat-makes-software"></a>
  <a href="https://github.com/cathyjf"><img src="https://avatars.githubusercontent.com/u/508485?v=4&amp;s=80" width="40" height="40" alt="cathyjf" title="cathyjf"></a>
  <a href="https://github.com/gamozolabs"><img src="https://avatars.githubusercontent.com/u/13505090?v=4&amp;s=80" width="40" height="40" alt="gamozolabs" title="gamozolabs"></a>
  <a href="https://github.com/BeeLeDev"><img src="https://avatars.githubusercontent.com/u/60373320?v=4&amp;s=80" width="40" height="40" alt="BeeLeDev" title="BeeLeDev"></a>
  <a href="https://github.com/angelnu"><img src="https://avatars.githubusercontent.com/u/4406403?v=4&amp;s=80" width="40" height="40" alt="angelnu" title="angelnu"></a>
  <a href="https://github.com/amarshall"><img src="https://avatars.githubusercontent.com/u/153175?v=4&amp;s=80" width="40" height="40" alt="amarshall" title="amarshall"></a>
  <a href="https://github.com/Aaron-Rumpler"><img src="https://avatars.githubusercontent.com/u/80610725?v=4&amp;s=80" width="40" height="40" alt="Aaron-Rumpler" title="Aaron-Rumpler"></a>
  <a href="https://github.com/kelvie"><img src="https://avatars.githubusercontent.com/u/5893?v=4&amp;s=80" width="40" height="40" alt="kelvie" title="kelvie"></a>
  <a href="https://github.com/KarlVogel"><img src="https://avatars.githubusercontent.com/u/4101575?v=4&amp;s=80" width="40" height="40" alt="KarlVogel" title="KarlVogel"></a>
  <a href="https://github.com/justindhillon"><img src="https://avatars.githubusercontent.com/u/145078271?v=4&amp;s=80" width="40" height="40" alt="justindhillon" title="justindhillon"></a>
  <a href="https://github.com/Tea23"><img src="https://avatars.githubusercontent.com/u/1298028?v=4&amp;s=80" width="40" height="40" alt="Tea23" title="Tea23"></a>
  <a href="https://github.com/jhgarner"><img src="https://avatars.githubusercontent.com/u/7400280?v=4&amp;s=80" width="40" height="40" alt="jhgarner" title="jhgarner"></a>
  <a href="https://github.com/insanemal"><img src="https://avatars.githubusercontent.com/u/215062?v=4&amp;s=80" width="40" height="40" alt="insanemal" title="insanemal"></a>
  <a href="https://github.com/Calinou"><img src="https://avatars.githubusercontent.com/u/180032?v=4&amp;s=80" width="40" height="40" alt="Calinou" title="Calinou"></a>
  <a href="https://github.com/hgaiser"><img src="https://avatars.githubusercontent.com/u/716138?v=4&amp;s=80" width="40" height="40" alt="hgaiser" title="hgaiser"></a>
  <a href="https://github.com/Han-cy830"><img src="https://avatars.githubusercontent.com/u/279452837?v=4&amp;s=80" width="40" height="40" alt="Han-cy830" title="Han-cy830"></a>
  <a href="https://github.com/hadicharara"><img src="https://avatars.githubusercontent.com/u/46264409?v=4&amp;s=80" width="40" height="40" alt="hadicharara" title="hadicharara"></a>
  <a href="https://github.com/c3m3gyanesh"><img src="https://avatars.githubusercontent.com/u/11159228?v=4&amp;s=80" width="40" height="40" alt="c3m3gyanesh" title="c3m3gyanesh"></a>
  <a href="https://github.com/himekifee"><img src="https://avatars.githubusercontent.com/u/65988663?v=4&amp;s=80" width="40" height="40" alt="himekifee" title="himekifee"></a>
  <a href="https://github.com/theofficialgman"><img src="https://avatars.githubusercontent.com/u/28281419?v=4&amp;s=80" width="40" height="40" alt="theofficialgman" title="theofficialgman"></a>
  <a href="https://github.com/ekchang"><img src="https://avatars.githubusercontent.com/u/10266208?v=4&amp;s=80" width="40" height="40" alt="ekchang" title="ekchang"></a>
  <a href="https://github.com/e-dong"><img src="https://avatars.githubusercontent.com/u/16567905?v=4&amp;s=80" width="40" height="40" alt="e-dong" title="e-dong"></a>
  <a href="https://github.com/Gara-Dorta"><img src="https://avatars.githubusercontent.com/u/1507774?v=4&amp;s=80" width="40" height="40" alt="Gara-Dorta" title="Gara-Dorta"></a>
  <a href="https://github.com/ehabajmal"><img src="https://avatars.githubusercontent.com/u/33014707?v=4&amp;s=80" width="40" height="40" alt="ehabajmal" title="ehabajmal"></a>
</p>
<!-- CONTRIBUTORS:END -->

[View contribution history](https://github.com/AlkaidLab/foundation-sunshine/graphs/contributors)
