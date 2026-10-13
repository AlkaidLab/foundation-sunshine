# Foundation HIDMaestro Core patch

This is a downstream managed-code patch for foundation-sunshine #1138. The
upstream v1.7.3 API and assembly/file versions are preserved so already-installed
sidecars can load it without a separate component update. The main Sunshine
package carries this Core and installs it before any sidecar creates a device.
The sidecar uses normal .NET dependency loading. It permits read-only capability
probes against the stock SDK during staging, but requires the patch marker before
any device operation. Sunshine verifies and replaces Core before launching it.

Foundation uses a Joystick UMDF2 persona and USB/IP composite personas. Neither
needs a global GameInput mapping or a shared Joystick OEM override. This build
therefore disables **all** default VID/PID-wide GameInput and OEM writes, including
the unused legacy Game Pad path. It retains per-controller registry configuration.
Launch recovery and full cleanup preserve shared OEM/slot settings. Device
parameter and interface cleanup require SDK ownership, and an unknown parent is
not accepted as proof of ownership of a phantom HID child.

`registry-isolation.patch` is applied exactly to the source commit and archive
hash in `upstream.json`. `scripts/build-hidmaestro-core-patch.ps1` extracts every
embedded resource from the separately hash-verified upstream release and
rebuilds only C# code. No WDK or native-driver rebuild is needed. Build validation
compares the entire public API and every embedded resource byte for byte, then
tests both Joystick and Game Pad registry writers in a process-local redirected
registry hive. Driver deployment, PnP cleanup and live device creation are not
run by these tests.

The DLL is hash-bound to the Sunshine build. Do not sign or rewrite it after
generating its trust header. Preserve the upstream MIT license and third-party
notices when distributing it. The original source is available at
https://github.com/hifihedgehog/HIDMaestro/tree/00b7303f8533c3fe10687a765c84e929b34a5e9c.

Existing, unowned GameInput mappings are deliberately preserved. Older SDKs did
not record their original values or ownership, so deleting by VID/PID or guessing
from a template would risk destroying another controller's configuration.
Previously deleted OEM/axis settings cannot be reconstructed without a backup.

When upstream ships an equivalent fix, verify the shared-setting paths and ABI
before removing this patch. A version bump alone does not establish equivalence.
