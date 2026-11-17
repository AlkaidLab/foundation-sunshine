#!/usr/bin/env python3
"""Static contract check for the legacy DualSense upgrade cleanup."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
INSTALLER = ROOT / "cmake/packaging/sunshine.iss.in"
SCRIPT = ROOT / "src_assets/windows/misc/migration/cleanup-legacy-ds5.ps1"


def main() -> None:
    installer = INSTALLER.read_text(encoding="utf-8")
    script = SCRIPT.read_text(encoding="utf-8")

    for fragment in (
        'Type: filesandordirs; Name: "{app}\\tools\\sunshine-ds5-component"',
        'Type: files; Name: "{app}\\tools\\ds5-sidecar-package.json"',
        "ExtractTemporaryFile('cleanup-legacy-ds5.ps1')",
        "function PrepareToInstall",
    ):
        assert fragment in installer, fragment
    for fragment in (
        "Sunshine.Ds5Sidecar.exe",
        "Get-CimInstance Win32_Service",
        "Get-ScheduledTask",
        "Get-NetFirewallApplicationFilter",
        "Test-OwnedPath",
    ):
        assert fragment in script, fragment
    assert "uninstall-usbip" not in script.lower()
    assert "pnputil" not in script.lower()
    print("legacy DS5 installer cleanup contract: PASS")


if __name__ == "__main__":
    main()
