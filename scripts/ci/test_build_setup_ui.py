"""Rules of scripts/build-setup-ui.ps1 (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "build-setup-ui.ps1"


class NativeExitCodes(unittest.TestCase):
    def test_every_python_step_checks_its_exit_code(self):
        # The script must also run on Windows PowerShell 5.1, which ignores
        # $PSNativeCommandUseErrorActionPreference: a refused prune or strip
        # would otherwise let cargo embed already-rewritten pages.
        lines = SCRIPT.read_text(encoding="utf-8").splitlines()
        python_steps = [i for i, line in enumerate(lines) if line.strip().startswith("python ")]
        self.assertTrue(python_steps)
        for index in python_steps:
            following = next(
                line for line in lines[index + 1:] if line.strip() and not line.strip().startswith("#")
            )
            self.assertIn("$LASTEXITCODE -ne 0", following, lines[index])


if __name__ == "__main__":
    unittest.main()
