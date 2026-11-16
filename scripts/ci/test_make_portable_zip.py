import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


SCRIPT = Path(__file__).with_name("make-portable-zip.py")


class PortableZipTest(unittest.TestCase):
    def test_stores_precompressed_entries_and_deflates_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "staging"
            (root / "assets").mkdir(parents=True)
            (root / "scripts" / "usbip").mkdir(parents=True)
            (root / "assets" / "logo.png").write_bytes(b"image" * 1000)
            (root / "scripts" / "usbip" / "usbip-win2.msi").write_bytes(b"installer" * 1000)
            (root / "readme.txt").write_bytes(b"portable package\n" * 1000)
            output = Path(directory) / "portable.zip"

            subprocess.run(
                [sys.executable, str(SCRIPT), "--source", str(root), "--output", str(output), "--workers", "2"],
                check=True,
            )

            with zipfile.ZipFile(output) as archive:
                entries = {entry.filename: entry for entry in archive.infolist()}
                self.assertEqual(entries["Sunshine/assets/logo.png"].compress_type, zipfile.ZIP_STORED)
                self.assertEqual(entries["Sunshine/scripts/usbip/usbip-win2.msi"].compress_type, zipfile.ZIP_STORED)
                self.assertEqual(entries["Sunshine/readme.txt"].compress_type, zipfile.ZIP_DEFLATED)
                self.assertEqual(archive.read("Sunshine/readme.txt"), b"portable package\n" * 1000)


if __name__ == "__main__":
    unittest.main()
