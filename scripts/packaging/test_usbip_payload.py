"""Packaging regression tests; no driver installation or machine state changes."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('extract_usbip', HERE / 'extract-usbip.py')
extract_usbip = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract_usbip)
MANIFEST = json.loads((HERE / 'usbip-payload.json').read_text())


class UsbipPayloadTests(unittest.TestCase):
    def test_complete_signed_package_is_preserved(self):
        self.assertEqual({p for p in MANIFEST['files'] if p.startswith('driver/')}, {
            f'driver/usbip2_{driver}.{extension}'
            for driver in ['ude', 'filter'] for extension in ['cat', 'inf', 'sys']})

    def test_only_required_tools_and_runtime_are_shipped(self):
        self.assertEqual(len(MANIFEST['files']), 14)
        self.assertEqual({k for k, v in MANIFEST['files'].items() if v['source'].startswith('runtime/')}, {
            'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'})
        self.assertFalse(any('redist' in p or p.endswith('.pdb') or 'wusbip' in p for p in MANIFEST['files']))

    def test_missing_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(FileNotFoundError):
                extract_usbip.verify_payload(Path(temp), MANIFEST)

    def test_modified_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'usbip.exe').write_bytes(b'changed')
            with self.assertRaises(ValueError):
                extract_usbip.verify_payload(root, {'files': {'usbip.exe': MANIFEST['files']['usbip.exe']}})

    def test_unexpected_installer_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'vc_redist.x64.exe').write_bytes(b'not allowed')
            with self.assertRaises(ValueError):
                extract_usbip.verify_payload(root, {'files': {}})

    def test_bad_installer_is_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            installer = root / 'upstream.exe'
            installer.write_bytes(b'incorrect distribution')
            with self.assertRaises(ValueError):
                extract_usbip.extract(installer, root / 'payload', root)
            self.assertFalse((root / 'payload').exists())


if __name__ == '__main__':
    unittest.main()
