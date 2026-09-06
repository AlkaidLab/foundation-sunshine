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

The same-OS loopback result above is superseded for the separate-OS hardware case by the VM test below. The full Moonlight UI/video-session flow remains unverified.

## Hyper-V Windows 10 result: 2026-09-06

The host exported the physical phone using usbipd-win, the production Qt tunnel connected through an SSH port forward to the production Sunshine tunnel service in a Windows 10 VM, and usbip-win2 0.9.7.8 imported it at virtual hub port 1. The VM recognized VID:PID `18d1:4ee7` and ADB enumerated the phone, without the VirtualBox binding seen in same-OS loopback.

ADB reported `unauthorized`, so the hardware check was changed to standard, read-only USB control transfers that require no phone interaction. The native `usb_control_probe.cpp` uses the installed Android WinUSB interface GUID to read the device descriptor and serial string, checks the expected VID/PID/serial, and executes 20 `GET_STATUS` requests via `WinUsb_ControlTransfer`.

**Passed:** two cycles of TLS forwarding, real device import, matching descriptor/serial and 20 status reads, followed by release. The second cycle reused hub port 1 successfully. The initial test and the parameterized reproduction script each passed both cycles. Each release left `usbip port` empty; afterward the phone returned to authorized local ADB. usbip-win2 also logged "device is not connected" on the explicit detach after tunnel closure; the port-empty and reimport checks verified the resulting cleanup.

This verifies real-device USB control transfers and reconnect through both production transport classes. It does not establish ADB shell authorization, sustained bulk/isochronous throughput, other device classes, or the Moonlight video-session/UI flow.

### Reproduce without phone authorization

Build the WinUSB probe from a Visual Studio x64 Native Tools prompt:

```bat
cl /EHsc /std:c++17 /MT /Febuild\usb_control_probe.exe /Fobuild\usb_control_probe.obj tests\tools\usb_control_probe.cpp setupapi.lib winusb.lib
```

Prepare a separate Windows importer with SSH access, usbip-win2, and `reverse_tunnel_probe.exe` plus its runtime DLLs and `usb_control_probe.exe` in `C:/usb-e2e`. Use an exporter with an already-shared Android device using the standard Android WinUSB interface. Stop ADB on the importer; the script restarts ADB on the exporter to release/recover its handle. Configure local Qt/OpenSSL DLL and plugin paths as for the transport suite. Then run:

```powershell
python tests/tools/run_usb_control_vm_e2e.py --ssh-target <user@importer> --identity-file <ssh-key> --client-probe <client-probe.exe> --busid <busid> --vid <hex-vid> --pid <hex-pid> --serial <serial> --output <new-evidence-directory>
```

The script refuses pre-existing imports and forwards a local SSH port to the importer's loopback TLS listener. Both the local output directory and the unique remote `control-*` directory contain temporary test credentials; keep them private and out of version control. The control probe is an Android WinUSB smoke test, not a generic device-class test.
