# Remote USB local validation

## Reproduce

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

For a real, already-shared ADB device and installed usbip-win2 driver:

```powershell
python tests/tools/run_usb_tunnel_e2e.py --server-probe build/tests/reverse_tunnel_probe.exe --client-probe <client-probe.exe> --usbip <usbip.exe> --busid <busid> --adb-serial <serial> --output <new-evidence-directory>
```

This test temporarily exports/imports the selected device and restarts the local ADB server. A complete pass requires an ADB shell command through the imported device, detach, and a second successful cycle. Inspect `usbip port` and `adb devices -l` after failure to verify cleanup. Evidence directories contain temporary private keys and tokens and must not be committed.

## Local result: 2026-09-06

- Sunshine, Moonlight Qt, and both standalone test drivers built successfully on Windows.
- CTest: 3/3 targets passed, including 34 controller/bridge unit cases and the host transport process tests.
- Both production transport classes: valid connection, wrong certificate pin, and wrong token all passed their expected outcomes.
- Source/build scan of `src`, `tests`, `cmake`, and `.gitmodules` found no obsolete protocol dependency or obsolete receive-mode command option before this explanatory document was added. The touchscreen POC still uses the separate loopback bridge.
- Real-device loopback: TLS forwarding and usbip-win2 attach succeeded at virtual hub port 1. The imported node bound to `VirtualBox USB Driver`, and ADB did not find the imported phone within 30 seconds. This is a failed hardware E2E, not a full pass.
- After the failed test, `usbip port` was empty and the physical phone reappeared as an authorized device in local ADB.

The local results establish the tested transport/controller behavior. Real device usability on separate computers and the full Moonlight UI/video-session flow remain unverified.
