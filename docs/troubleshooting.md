# Troubleshooting

## General

### Forgotten Credentials
If you forgot your credentials to the web UI, try this.

@tabs{
  @tab{General | ```bash
    sunshine --creds {new-username} {new-password}
    ```
  }
  @tab{AppImage | ```bash
    ./sunshine.AppImage --creds {new-username} {new-password}
    ```
  }
  @tab{Flatpak | ```bash
    flatpak run --command=sunshine dev.lizardbyte.app.Sunshine --creds {new-username} {new-password}
    ```
  }
}

@tip{Don't forget to replace `{new-username}` and `{new-password}` with your new credentials.
Do not include the curly braces.}

### Web UI Access
Can't access the web UI?

1. Check firewall rules.

### Controller works on Steam but not in games
One trick might be to change Steam settings and check or uncheck the configuration to support Xbox/Playstation
controllers and leave only support for Generic controllers.

Also, if you have many controllers already directly connected to the host, it might help to disable them so that the
Sunshine provided controller (connected to the guest) is the "first" one. In Linux this can be accomplished on USB
devices by finding the device in `/sys/bus/usb/devices/` and writing `0` to the `authorized` file.

### Network performance test

For real-time game streaming the most important characteristic of the network
path between server and client is not pure bandwidth but rather stability and
consistency (low latency with low variance, minimal or no packet loss).

The network can be tested using the multi-platform tool [iPerf3](https://iperf.fr).

On the Sunshine host `iperf3` is started in server mode:

```bash
iperf3 -s
```

On the client device iperf3 is asked to perform a 60-second UDP test in reverse
direction (from server to client) at a given bitrate (e.g. 50 Mbps):

```bash
iperf3 -c {HostIpAddress} -t 60 -u -R -b 50M
```

Watch the output on the client for packet loss and jitter values. Both should be
(very) low. Ideally packet loss remains less than 5% and jitter below 1ms.

For Android clients use
[PingMaster](https://play.google.com/store/apps/details?id=com.appplanex.pingmasternetworktools).

For iOS clients use [HE.NET Network Tools](https://apps.apple.com/us/app/he-net-network-tools/id858241710).

If you are testing a remote connection (over the internet) you will need to
forward the port 5201 (TCP and UDP) from your host.

### Packet loss (Buffer overrun)
If the host PC (running Sunshine) has a much faster connection to the network
than the slowest segment of the network path to the client device (running
Moonlight), massive packet loss can occur: Sunshine emits its stream in bursts
every 16ms (for 60fps) but those bursts can't be passed on fast enough to the
client and must be buffered by one of the network devices inbetween. If the
bitrate is high enough, these buffers will overflow and data will be discarded.

This can easily happen if e.g. the host has a 2.5 Gbit/s connection and the
client only 1 Gbit/s or Wi-Fi. Similarly, a 1 Gbps host may be too fast for a
client having only a 100 Mbps interface.

As a workaround the transmission speed of the host NIC can be reduced: 1 Gbps
instead of 2.5 or 100 Mbps instead of 1 Gbps. (A technically more advanced
solution would be to configure traffic shaping rules at the OS-level, so that
only Sunshine's traffic is slowed down.)

Sunshine versions > 0.23.1 include improved networking code that should
alleviate or even solve this issue (without reducing the NIC speed).

### Packet loss (MTU)
Although unlikely, some guests might work better with a lower
[MTU](https://en.wikipedia.org/wiki/Maximum_transmission_unit) from the host.
For example, a LG TV was found to have 30-60% packet loss when the host had MTU
set to 1500 and 1472, but 0% packet loss with a MTU of 1428 set in the network card
serving the stream (a Linux PC). It's unclear how that helped precisely, so it's a last
resort suggestion.

## Linux

### Hardware Encoding fails
Due to legal concerns, Mesa has disabled hardware decoding and encoding by default.

```txt
Error: Could not open codec [h264_vaapi]: Function not implemented
```

If you see the above error in the Sunshine logs, compiling *Mesa* manually, may be required. See the official Mesa3D
[Compiling and Installing](https://docs.mesa3d.org/install.html) documentation for instructions.

@important{You must re-enable the disabled encoders. You can do so, by passing the following argument to the build
system. You may also want to enable decoders, however that is not required for Sunshine and is not covered here.
```bash
-Dvideo-codecs=h264enc,h265enc
```
}

@note{Other build options are listed in the
[meson options](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/meson_options.txt) file.}

### KMS Streaming fails
If screencasting fails with KMS, you may need to run the following to force unprivileged screencasting.

```bash
sudo setcap -r $(readlink -f $(which sunshine))
```

@note{The above command will not work with the AppImage or Flatpak packages. Please refer to the
[AppImage setup](md_docs_2getting__started.html#appimage) or
[Flatpak setup](md_docs_2getting__started.html#flatpak) for more specific instructions.}

### KMS streaming fails on Nvidia GPUs
If KMS screen capture results in a black screen being streamed, you may need to
set the parameter `modeset=1` for Nvidia's kernel module. This can be done by
adding the following directive to the kernel command line:

```bash
nvidia_drm.modeset=1
```

Consult your distribution's documentation for details on how to do this. (Most
often grub is used to load the kernel and set its command line.)

### AMD encoding latency issues
If you notice unexpectedly high encoding latencies (e.g. in Moonlight's
performance overlay) or strong fluctuations thereof, this is due to
[missing support](https://gitlab.freedesktop.org/drm/amd/-/issues/3336)
in Mesa/libva for AMD's low latency encoder mode. This is particularly
problematic at higher resolutions (4K).

Only the most recent development versions of mesa include support for this
low-latency mode. It will be included in Mesa-24.2.

In order to enable it, Sunshine has to be started with a special environment
variable:

```bash
AMD_DEBUG=lowlatencyenc sunshine
```

To check whether low-latency mode is being used, one can watch the `VCLK` and
`DCLK` frequencies in `amdgpu_top`. Without this encoder tuning both clock
frequencies will fluctuate strongly, whereas with active low-latency encoding
they will stay high as long as the encoder is used.

### Gamescope compatibility
Some users have reported stuttering issues when streaming games running within Gamescope.

## macOS

### FFmpeg ABI Mismatch (SIGSEGV in av_hwframe_ctx_init)

**Symptom:**
- Server crashes with SIGSEGV during encoder initialization
- Crash occurs in `av_hwframe_ctx_init` function
- Debug shows `AVHWDeviceContext->type` has incorrect value

**Root Cause:**
Sunshine compiles with system Homebrew FFmpeg headers (`/opt/homebrew/include`) but links against bundled FFmpeg static libraries (`third-party/build-deps/dist/Darwin-arm64/lib`). The structure layouts differ between FFmpeg versions, causing memory corruption.

**Solution:**
Ensure bundled FFmpeg headers have priority in CMake configuration. This is already fixed in `cmake/compile_definitions/common.cmake`:

```cmake
# FFmpeg bundled headers must come BEFORE system headers
include_directories(BEFORE SYSTEM ${FFMPEG_INCLUDE_DIRS})
```

If you encounter this issue, verify your CMake configuration and rebuild from scratch:
```bash
rm -rf build
cmake -B build -G Ninja -S .
ninja -C build
```

### Encoder Initialization Failure (Invalid pixel format -1)

**Symptom:**
- Error: `Invalid video pixel format: -1`
- Error: `Picture size 2000x0 is invalid`
- All encoders (VideoToolbox and software) fail to initialize

**Root Cause:**
The `sunshine_colorspace_t` struct in `src/video_colorspace.h` had no default values, leading to uninitialized members when the struct was created.

**Solution:**
This is already fixed with default values in the struct definition:

```cpp
struct sunshine_colorspace_t {
  colorspace_e colorspace = colorspace_e::rec709;
  bool full_range = false;
  unsigned bit_depth = 8;
};
```

If you're building from an older commit, update to the latest code or manually add these defaults.

### Mouse Scroll Wheel Not Working

**Symptom:**
- Mouse movement works but scroll wheel has no effect
- Scrolling in remote applications doesn't respond

**Root Cause:**
macOS input code used `kCGScrollEventUnitLine` which has insufficient precision on modern macOS versions.

**Solution:**
This is already fixed in `src/platform/macos/input.cpp` to use `kCGScrollEventUnitPixel`:

```cpp
CGEventRef event = CGEventCreateScrollWheelEvent(
    macos_input->source,
    kCGScrollEventUnitPixel,  // Changed from kCGScrollEventUnitLine
    2,
    scrollY,
    scrollX
);
```

### Mouse Input Delay on Stream Start

**Symptom:**
- Mouse doesn't respond for ~250ms when first entering stream
- Initial clicks/movements are ignored

**Root Cause:**
macOS has a default 250ms event suppression interval to prevent event loops.

**Solution:**
This is already fixed in `src/platform/macos/input.cpp`:

```cpp
macos_input->source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
CGEventSourceSetLocalEventsSuppressionInterval(macos_input->source, 0.0);
```

### VideoToolbox Encoder Best Practices

**Tips for optimal VideoToolbox performance:**

1. **Use NV12 pixel format** for 8-bit content (most common)
2. **Use P010 pixel format** for 10-bit HDR content
3. **Ensure proper dimension alignment**: Width and height should be even numbers for YUV420
4. **Check hardware support**: Not all Macs support all encoder features
5. **Monitor encoder logs**: Look for "Encoder [videotoolbox] passed" in startup logs

**Common VideoToolbox errors:**

- `kVTParameterErr`: Usually indicates invalid encoder parameters
- `kVTAllocationFailedErr`: System resources exhausted, try lowering resolution/bitrate
- `kVTPixelTransferNotSupportedErr`: Pixel format conversion not supported by hardware

### Remote Microphone Issues

#### No audio signal

**Symptoms:** Input level meter shows no activity

**Solutions:**
1. Verify BlackHole is installed:
   ```bash
   ls /Library/Audio/Plug-Ins/HAL/ | grep BlackHole
   ```

2. Check system input device:
   - System Settings → Sound → Input
   - Should be "BlackHole 2ch"

3. Check Sunshine logs:
   ```bash
   tail -f ~/.config/sunshine/sunshine.log | grep -i "microphone\|blackhole"
   ```

4. Restart Sunshine

#### Audio feedback/noise

**Symptoms:** Hearing echo or static

**Solutions:**
1. Verify system output is NOT BlackHole:
   - System Settings → Sound → Output
   - Should be "Mac mini Speaker" or headphones

2. Check for audio loops:
   - Only BlackHole should be input
   - Output should be physical device

#### High latency

**Symptoms:** Noticeable delay in voice

**Solutions:**
1. Check network quality
2. Reduce streaming resolution/bitrate
3. Use wired connection instead of WiFi

#### Application not detecting microphone

**Symptoms:** App shows "No microphone"

**Solutions:**
1. Grant microphone permission:
   - System Settings → Privacy & Security → Microphone
   - Enable for the application

2. Restart the application

3. Verify BlackHole is system input

### Dynamic session lookup failed
If you get this error:

> Dynamic session lookup supported but failed: launchd did not provide a socket path, verify that
> org.freedesktop.dbus-session.plist is loaded!

Try this.
```bash
launchctl load -w /Library/LaunchAgents/org.freedesktop.dbus-session.plist
```

## Windows

### No gamepad detected
Verify that you've installed [Nefarius Virtual Gamepad](https://github.com/nefarius/ViGEmBus/releases/latest).

### Permission denied
Since Sunshine runs as a service on Windows, it may not have the same level of access that your regular user account
has. You may get permission denied errors when attempting to launch a game or application from a non system drive.

You will need to modify the security permissions on your disk. Ensure that user/principal SYSTEM has full
permissions on the disk.

<div class="section_buttons">

| Previous                                    |                    Next |
|:--------------------------------------------|------------------------:|
| [Performance Tuning](performance_tuning.md) | [Building](building.md) |

</div>
