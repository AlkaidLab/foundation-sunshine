# Remote USB local validation

## Transport regression suite

Build Sunshine and the standalone regression targets from a configured Windows build:

```powershell
cmake --build build --target sunshine reverse_tunnel_probe remote_usb_host_controller_unit_tests loopback_usbip_bridge_unit_tests -j 6
ctest --test-dir build -R '^(remote_usb_host_controller_unit_tests|loopback_usbip_bridge_unit_tests|reverse_tunnel_transport_tests)$' --output-on-failure
```

Build Moonlight Qt's `tests/usb_forwarding_tunnel/usb_forwarding_tunnel.pro` with its Qt toolchain. Run both production transport classes together, with the Qt and OpenSSL runtime DLLs on PATH and Qt's plugin directory configured as needed:

```powershell
python tests/tools/test_reverse_tunnel.py build/tests/reverse_tunnel_probe.exe --client-probe <path-to-usb_forwarding_tunnel_probe.exe>
```

The test generates temporary certificates and tokens. It exercises certificate and token rejection, forwarding before import completion, bytes following the JSON handshake, detach/reconnect, late attach failure, and shutdown during a pending import. The helper simulates USB/IP import; these tests do not establish hardware compatibility or video-session integration.

## Real-device E2E (separate-OS importer)

These scripts require a real, already-shared Android device using the standard Android WinUSB interface, a separate Windows importer VM with SSH access, and the installed usbip-win2 driver on the importer. They temporarily export/import the device and restart the local ADB server. Evidence directories contain temporary private keys, tokens, and credentials; keep them private and out of version control.

Build the WinUSB control probe (standard, read-only USB control transfers that require no phone interaction — used because ADB on the importer reports `unauthorized` for an imported phone) from a Visual Studio x64 Native Tools prompt:

```bat
cl /EHsc /std:c++17 /MT /Febuild\usb_control_probe.exe /Fobuild\usb_control_probe.obj tests\tools\usb_control_probe.cpp setupapi.lib winusb.lib
```

Prepare the importer with `reverse_tunnel_probe.exe`, its runtime DLLs, and `usb_control_probe.exe` in `C:/usb-e2e`. Stop ADB on the importer (the script restarts ADB on the exporter to release/recover its handle). Configure local Qt/OpenSSL DLL and plugin paths as for the transport suite. Then run:

```powershell
python tests/tools/run_usb_control_vm_e2e.py --ssh-target <user@importer> --identity-file <ssh-key> --client-probe <client-probe.exe> --busid <busid> --vid <hex-vid> --pid <hex-pid> --serial <serial> --output <new-evidence-directory>
```

The script refuses pre-existing imports and forwards a local SSH port to the importer's loopback TLS listener. A complete pass is two cycles of TLS forwarding, real device import, matching descriptor/serial, 20 `GET_STATUS` reads, release, and re-import. Inspect `usbip port` and `adb devices -l` after failure to verify cleanup. The control probe is an Android WinUSB smoke test, not a generic device-class test.

## Full video-session flow

Sunshine now uses `usb_forwarding_enabled` (default off) and `usb_forwarding_port`
(default 0 = main port + 7, normally 47996), configurable under Web settings → Input.
An explicit 1024–65535 overrides the automatic port; an existing explicit 47996
remains fixed until changed to 0. Clients must use the advertised port, not derive it.
An isolated runtime check covered main port 58989 -> USB 58996, omitted/zero settings,
explicit 58997, invalid -1/1023/65536 falling back to automatic, and occupied-port unavailability.
Save and restart after
enabling. The host generates an in-memory token and exposes version 1 capability
JSON at paired-mTLS-only `GET /api/v1/usb-forwarding`; disabled/unavailable replies
omit credentials. `SUNSHINE_USB_TUNNEL_*` no longer provisions the production host.

On Android 9+ ARM64, pair normally, start a stream, open USB forwarding, enable it
for this host, select an OTG device, and grant USB permission. The client retrieves
credentials automatically; no build-time secrets or separate exporter app are
needed. Verify Windows enumeration, stop sharing, and verify the imported nodes
disappear; repeat in another stream to check reconnect.

The following desktop-client fixture predates runtime provisioning. It requires
a Qt client that consumes the capability endpoint (or the standalone probe above
with explicit test credentials); environment-only clients cannot provision this
production host. Start the compatible full Moonlight client with:

```powershell
Moonlight.exe stream <host-address> Desktop --resolution 1024x768 --fps 30 --bitrate 3000 --display-mode windowed --video-codec H.264 --no-hdr --no-yuv444
```

Use `Ctrl+Alt+Shift+O` to open the streaming menu and select the USB device. Run `usb_control_probe.exe <vid> <pid> <serial>` on the host, then quit streaming normally and verify `usbip port` is empty. The device must re-import in a second session to prove the reconnect path.

Deployment prerequisites:

- Use the usbip-win2 executable and accompanying DLLs matching the installed driver version; a mismatched `resources.dll` or an older helper against a newer driver fails import.
- The imported device's driver binding can vary with the host's USB stack (for example a VirtualBox filter driver binding the imported node); run the E2E on a host without conflicting USB filter drivers.
