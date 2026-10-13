# Features and setup

[Project home](../readme/README.en.md) · [Documentation](README.md) · [简体中文](../zh-CN/features.md)

Complete the [first connection in the README](../readme/README.en.md#quick-start) before configuring these features. Each topic explains its purpose, activation, and verification together.

## Video and displays

### HDR and color

Phones, tablets, and TVs have different brightness capabilities. Foundation uses values reported by the client to set a luminance target for that session, preserving the PC's local HDR and color settings by default when using a physical display.

PQ / HLG output, per-frame scene analysis, HDR10+ and HDR Vivid dynamic information are supported. Dolby Vision provides Profile 8.1 / 8.4 negotiation and host metadata injection. Paired devices can have individual brightness settings. Virtual displays can use Windows HDR Calibration results, and physical-display ICC overrides can be restored after a session.

**Enable and check:**

1. Confirm the client, operating system, and display support HDR, then enable it in the client.
2. Select the display and HDR behavior in Sunshine's video settings. Install the virtual display driver first if using a Foundation display.
3. Prefer client-reported brightness in paired-device settings. Use manual brightness or an installed color profile when calibration is needed.
4. Enable scene-adaptive HDR, inspect host runtime status, and check the client's actual format and fallback information.

Host negotiation or injection status does not confirm display output. Dynamic HDR presentation also depends on the client, operating system, display, and encoding path.


### Foundation virtual displays ZakoDisplay

**No physical monitor is required: create a virtual display and remotely use the PC desktop and games.** [ZakoDisplay](https://github.com/qiin2333/zako-vdd) supports HDR, custom resolution and refresh rate, session-based display creation and cleanup, and per-client display settings.

Choose a virtual-only display, keep the physical display with a virtual extension, make the target display primary, or activate only the selected display. Set portrait orientation for a phone, a suitable resolution for a tablet, or disable the physical displays during remote use.

**Enable and check:**

1. Open virtual display settings in the Foundation Windows desktop GUI and check the driver. If it was omitted during installation or needs repair, use this page to install or repair it.
2. Confirm it is available; reboot Windows if the status requests it.
3. Select the Foundation display in the setup wizard or display settings, then configure resolution, refresh rate, and display layout.
4. Connect from Moonlight, check the image size and Windows layout, and check local display restoration when the session ends.

Supporting clients can also control rotation, Windows scaling, and session resolution. Per-client display settings do not create isolated Windows desktops.


## Capture encoding and performance

Zako Direct takes frames directly from the virtual display. Supported paths borrow GPU frames to reduce capture-stage copies; copying is used when handoff is unavailable, and Desktop Duplication is a fallback if direct capture initialization fails.

Windows Graphics Capture (WGC), Desktop Duplication, and AMD Display Capture are also available. WGC can capture a specific application or frame-interpolation window. AMD Display Capture does not support virtual displays.

**Enable and check:**

1. For a Foundation virtual display, select **Zako Direct** in the capture-method setting.
2. For a single window, select WGC and the window target, then enter its title. WGC requires user mode rather than a SYSTEM service.
3. Select a video codec supported by the client decoder, then inspect the actual capture method and encoder in the logs. Return to automatic capture and default encoding when troubleshooting.

### NVIDIA image enhancements

RTX HDR converts SDR games to HDR output. DLSS NR is an optional pre-encoding image enhancement with live adjustment. Both use additional GPU time.

Install required components in the desktop GUI's image enhancement manager, then enable them per application. Confirm a normal connection before enabling each enhancement and comparing picture quality and latency. RTX HDR does not process native HDR or HLG input. Check availability in the GUI.


Encoding supports NVIDIA NVENC, AMD AMF, Intel Quick Sync, and software fallback, with available video formats depending on the GPU and client. Foundation optimizes capture and encoding, live video adjustments, and network adaptation. Check actual behavior using host status and client performance statistics.

## Audio and microphone forwarding

Receive PC audio on your client and forward the client's microphone to the PC for voice chat or calls. Opus supports up to 7.1.4 channels; PCM, AC3, and E-AC3 currently support up to 5.1. Packet-loss recovery and continuous audio during silence address network disturbance and playback interruptions.

**Enable and check:**

1. Play audio on the host PC and confirm Windows audio works locally.
2. Select the host audio output in Sunshine. Start with **stereo / Opus** on the client.
3. Once it works, select channels and codecs matching the speakers or receiver. PCM uses more bandwidth; AC3 / E-AC3 passthrough depends on the client and receiving hardware.
4. For the remote microphone, check components and devices in the host device center, grant client microphone permission, enable forwarding, and test with a recording application on the PC.


## Desktop control panel

Alongside Sunshine's WebUI, Foundation provides a Windows desktop GUI for device components, game libraries, runtime status, logs, and updates.

- **Devices and drivers:** Virtual displays have a dedicated settings page; the device center manages controller, microphone, virtual mouse, and USB transport components. Some components can be selected during host installation.
- **Game libraries:** Scan Steam, Epic, GOG, or local folders, add applications in batches, and manage covers.
- **Runtime management:** Inspect sessions and resource usage, manage host service operation, and check logs and updates.
- **Diagnostics and assistance:** Local log analysis, with optional model-backed log diagnosis, title cleanup, and cover assistance after configuring a model service.

Open it through the Foundation Sunshine shortcut on Windows. A browser WebUI supports regular configuration; driver installation and local scanning require the desktop GUI.


## Keyboard touch and controllers

Precision touchpads, client-side cursors, and text-input context require client support. Pen barrel roll is a protocol extension; owning a pen does not establish client compatibility.


### DualSense and virtual controllers

Foundation Sunshine provides a DualSense device component. Install it in **Device center → Controllers** to expose a native DualSense on the host PC, allowing supported games to use adaptive triggers, audio haptics, lighting, and touchpad features as they would with a USB-connected controller. Feedback also requires a compatible client and connection method.

**Enable and check:**

1. Install the DualSense component in the device center and confirm it is available.
2. Choose a global controller type or a per-game DualSense override. Xbox 360 and DualShock 4 are also available.
3. Connect the physical controller to the client device, start the remote session, and check buttons and sticks first.
4. Test feedback in a game that supports the relevant features. Audio haptics need matching transport components; follow the device center prompts.

Type changes apply to newly created virtual controllers. For unwanted desktop or menu scrolling after installing the DS5 component, see the [FAQ](faq.md#windows-desktop-or-menus-scroll-after-installing-the-simulated-ds5-component).


## Desktop integration and session control
Clipboard sync supports text and images in both directions. Shared folders expose selected directories with paired-device permissions. Enable the capability and configure directories and permissions on the host, then use the client's clipboard or host-files entry. USB forwarding needs transport components and explicit device authorization.



Supporting clients can adjust bitrate, frame rate, and resolution during a connection. Adaptive bitrate and video loss protection use client feedback; they do not configure network reachability between the devices.

Complete the local connection first, then enable adaptive bitrate or adjust video settings. Start by lowering bitrate for an unstable network, record loss and latency, and adjust one setting at a time. Preset host commands, sleep, optional app cleanup after the last video session, and Webhook notifications are additional session settings.


## Other integrations and experimental features

- **Moonlight Web:** The desktop GUI can download, configure, and start a separate third-party browser client. Sunshine's management webpage is not itself a remote desktop client.
- **QR pairing:** An experimental LAN feature requiring a scanning client. New users should follow PIN pairing in the README.
- **PyroWave:** An experimental video path requiring matching client and runtime components.

See [development](development.md) for implementation and build entry points.
