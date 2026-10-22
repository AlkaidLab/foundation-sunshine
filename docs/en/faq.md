# FAQ and troubleshooting

[Project home](../readme/README.en.md) · [Documentation](README.md) · [简体中文](../zh-CN/faq.md)

Choose the symptom you encounter. Installation and PIN pairing are in the [README](../readme/README.en.md#quick-start); activation steps are in [features and setup](features.md).

## Stuttering dropped frames or high input latency

### 1 Establish a stable baseline

Set the client to **1080p**, **60 FPS**, and **10 Mbps**. Temporarily disable HDR, upscaling, interpolation, and other enhancements. Leave Sunshine encoding on automatic; try H.264 for codec compatibility issues. Prefer Ethernet for the host and 5 GHz or 6 GHz Wi-Fi near the router for a wireless client.

Disconnect, reconnect to **Desktop**, and test moving windows or video before a game. A lower frame rate on a static desktop is often the result of sending content updates as needed; assess stutter with moving content.

### 2 Open performance statistics and locate the bottleneck

Enable **performance statistics / overlay** in the client settings or session menu, and check host GUI status or logs. Fields and measurement methods vary by client.

| Symptom | Check first | Next step |
| --- | --- | --- |
| Network frame loss, latency, or jitter rises | Wi-Fi, available bandwidth, upload congestion | Lower bitrate; compare with wired networking or moving closer to the router, and pause large transfers |
| Network is healthy but host encoding time is high or FPS is low | GPU load, game frame rate, encoder settings | Lower game settings or session resolution, verify hardware encoding, and compare with RTX HDR / DLSS NR disabled |
| Network and host are healthy but decode time is high | Hardware decoding, video format, client temperature | Verify hardware decoding, try H.264 or HEVC, lower resolution, and check for thermal throttling over time |
| FPS appears normal but motion periodically stutters | Display refresh rate, session FPS, frame queue, synchronization | Start at a matching 60 FPS, keep the game's frame rate stable, and compare the client's default frame-pacing settings |
| The game also stutters locally | Game and host performance | Resolve local frame drops first; remote access cannot remove local stutter |

A 60 FPS frame interval is about **16.7 ms**, while 120 FPS is about **8.3 ms**. Use this scale to judge whether processing keeps up. Do not add differently measured network and decode fields and call the result complete end-to-end latency.

### 3 Increase one setting at a time

Restore 20 Mbps first, then raise resolution or frame rate. Change one option at a time and compare the same scene. Enable HDR, enhancements, and surround after the baseline works. Avoid starting with 4K, high frame rates, and maximum bitrate together.

For internet stutter, also check host upload, client download conditions, and whether the remote network path uses a direct connection or relay. Smooth LAN performance does not establish the same result over the internet.

### 4 Share reproducible results

Report host and client versions, CPU/GPU, connection methods, resolution, FPS, bitrate, and codec. Include statistics screenshots and matching logs, and state whether the problem persists with baseline settings. See the [official Moonlight statistics guide](https://github.com/moonlight-stream/moonlight-docs/wiki/Frequently-Asked-Questions#what-do-the-numbers-mean-in-the-on-screen-connection-statistics) for metric definitions.

## Windows desktop or menus scroll after installing the simulated DS5 component

If Windows desktop or menus scroll automatically after installing the simulated DS5 controller component, try [downloading and installing the latest Foundation Sunshine test release](https://github.com/AlkaidLab/foundation-sunshine/releases). Choose the newest release marked **Pre-release** and follow the [upgrade guide](upgrading.md).

If it persists after updating, report your **Windows version, Sunshine version, and reproduction steps** to help investigate.

## The PC does not appear

Use the [IP lookup steps in the README](../readme/README.en.md#3-add-the-pc-and-pair) to add the IPv4 address manually. If connection still fails, check that Sunshine is running, both devices are on the same local network, and the Windows firewall allows Sunshine traffic. Guest Wi-Fi or wireless isolation may prevent devices from reaching each other.

## Pairing works but the screen is black or launch fails

Try **Desktop** first. In the setup wizard or display settings, confirm that the selected display is active. For a virtual display, check the driver status first. If it still fails, note the time and save the corresponding logs from the logs or troubleshooting page. Include your GPU and driver version in the report.

## Video works but there is no sound

Play audio on the host PC and confirm that Windows has sound locally. Check Sunshine's selected audio output device. Set the client to **stereo / Opus**, disconnect, and reconnect. Once audio works, enable multichannel audio or other formats one at a time.

## It works at home but not elsewhere

A local network address cannot be used directly over the internet. Complete the local connection first, then configure remote access using the documentation and community resources on the [website](https://www.alkaidlab.com/) or the [China website](https://www.alkaidlab.cn/).

## Report a problem

Include host and client versions, device models, reproduction steps, and logs from the relevant time in [GitHub Issues](https://github.com/AlkaidLab/foundation-sunshine/issues). The [QQ community](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi) is available in Chinese.
