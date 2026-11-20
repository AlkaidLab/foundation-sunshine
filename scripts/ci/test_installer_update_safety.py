"""The update path of the installer must not be able to break a working installation.

The failure this guards: the Control Panel's updater ran the installer silently while a program of
the install folder still ran. Copying sunshine-gui.exe failed, Setup (silent, so "Abort" is the
default answer) rolled back, and the rollback removed assets\\web, which Setup had just created
after an [InstallDelete] entry removed the old folder. The installation stayed without a Web UI.

Inno Setup does not undo an [InstallDelete] entry and does not bring back a file it overwrote
(installer-inno-installdelete-20261119; the Setup log of the failed update shows the rollback), so the installer has to
(1) refuse to start copying while the folder is busy, and (2) move the Web UI folder aside instead
of deleting it and put it back when the installation does not finish. The behaviour is run on a
Windows runner by scripts/ci/test-installer-silent.ps1; this file pins the script's structure on
every platform.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ISS = (ROOT / "cmake/packaging/sunshine.iss.in").read_text(encoding="utf-8")
QUIESCE = (ROOT / "src_assets/windows/misc/migration/update-quiesce.ps1").read_text(encoding="utf-8")
SMOKE = (ROOT / "scripts/ci/test-installer-silent.ps1").read_text(encoding="utf-8")


def section(name: str) -> str:
    """The body of an Inno section ([InstallDelete], [Files], ...) up to the next section."""
    match = re.search(r"^\[" + re.escape(name) + r"\]\n(.*?)(?=^\[)", ISS, re.S | re.M)
    assert match, f"section [{name}] not found"
    return match.group(1)


def procedure(name: str) -> str:
    """A [Code] procedure or function, from its header to the closing `end;` at column 0."""
    match = re.search(r"^(?:procedure|function) " + re.escape(name) + r"\b.*?^end;", ISS, re.S | re.M)
    assert match, f"{name} not found"
    return match.group(0)


class WebFolderIsNeverDeletedUpFront(unittest.TestCase):
    def test_install_delete_does_not_remove_the_web_folder(self):
        # [InstallDelete] runs as the first step and is not rolled back.
        entries = [line for line in section("InstallDelete").splitlines() if line.startswith("Type:")]
        for entry in entries:
            self.assertNotIn("assets\\web", entry, entry)

    def test_a_folder_is_finished_only_when_the_installer_says_so(self):
        # two pages are copied first: a copy that ended after them must not count as finished
        self.assertIn("'\\.install-complete'", procedure("WebUiIsComplete"))
        self.assertIn("MarkWebUiComplete();", procedure("CurStepChanged"))
        self.assertRegex(section("UninstallDelete"), r"assets\\web\\\.install-complete")

    def test_a_run_that_prepared_nothing_settles_nothing(self):
        deinit = procedure("DeinitializeSetup")
        self.assertIn("if UpdateAppDir <> '' then", deinit)

    def test_the_old_folder_is_moved_aside_before_the_copy_and_kept_until_the_end(self):
        # in the Preparing to Install stage, where a refusal still costs nothing; a folder that cannot be moved stops the update
        prepare = procedure("PrepareToInstall")
        self.assertIn("if not BackUpWebUi() then", prepare)
        self.assertLess(prepare.index("RunUpdateQuiesce(False, Report)"), prepare.index("if not BackUpWebUi() then"))
        self.assertNotIn("BackUpWebUi", procedure("CurStepChanged"))
        self.assertIn("RenameFile(WebUiDir(), WebUiBackupDir())", procedure("BackUpWebUi"))

    def test_a_setup_that_did_not_finish_puts_the_old_folder_back(self):
        # DeinitializeSetup runs after the rollback ("Deinitializing Setup." follows the rollback in the Setup log).
        self.assertIn("FinishWebUiBackup();", procedure("DeinitializeSetup"))
        finish = procedure("FinishWebUiBackup")
        self.assertIn("PostInstallReached and (WebUiIsComplete(WebUiDir()) or WebUiMarkerFailed)", finish)
        self.assertIn("RenameFile(WebUiBackupDir(), WebUiDir())", finish)
        self.assertIn("PostInstallReached := True;", procedure("CurStepChanged"))

    def test_an_interrupted_run_is_recovered_by_the_next_one(self):
        self.assertIn("if not RecoverWebUi() then", procedure("PrepareToInstall"))
        self.assertIn("RenameFile(WebUiBackupDir(), WebUiDir())", procedure("RecoverWebUi"))


class TheFolderIsMadeSafeBeforeAnythingIsCopied(unittest.TestCase):
    def test_the_script_is_embedded_and_run_from_prepare_to_install(self):
        self.assertRegex(
            section("Files"),
            r'Source: "\{#MySourceDir\}\\scripts\\update-quiesce\.ps1"; DestDir: "\{tmp\}"; Flags: dontcopy',
        )
        prepare = procedure("PrepareToInstall")
        self.assertIn("RunUpdateQuiesce(False, Report)", prepare)
        # a refusal changes nothing: Setup stops in the Preparing to Install stage (exit code 7)
        self.assertIn("Result := FmtMessage(CustomMessage('MsgUpdateBlocked'), [Report]);", prepare)

    def test_it_is_checked_again_right_before_the_copy(self):
        step = procedure("CurStepChanged")
        self.assertIn("RunUpdateQuiesce(True, Report)", step)
        self.assertLess(step.index("RunUpdateQuiesce(True, Report)"), step.index("JournalStart();"))

    def test_programs_are_no_longer_ended_by_name_alone(self):
        prepare = procedure("PrepareToInstall")
        # the name-based kill is only the fallback when the script could not run at all
        self.assertRegex(prepare, r"if QuiesceCode < 0 then\s+begin\s+// Without the script")

    def test_programs_are_moved_aside_so_nothing_can_start_them_and_put_back_on_failure(self):
        self.assertIn("-Park", procedure("RunUpdateQuiesce"))
        self.assertIn("$Park", QUIESCE)
        self.assertIn(".parked", QUIESCE)
        self.assertIn("SettleParkedImages(PostInstallReached);", procedure("DeinitializeSetup"))
        self.assertIn("RecoverParkedImages();", procedure("PrepareToInstall"))
        # a refused update puts them back before the service is started from them
        prepare = procedure("PrepareToInstall")
        self.assertLess(prepare.index("SettleParkedImages(False);"), prepare.index("net.exe'), 'start SunshineService'"))
        for relative in ("sunshine.exe", "tools\\sunshinesvc.exe", "assets\\gui\\sunshine-gui.exe"):
            self.assertIn(relative, QUIESCE)
            self.assertIn(relative, procedure("SettleParkedImages"))

    def test_a_service_the_updater_stopped_is_started_again_after_a_refusal(self):
        # the Control Panel's updater stops the service before the installer starts; the installer ends that
        # updater, so the refusal path has to start the service itself
        self.assertIn("updater: ended", QUIESCE)
        self.assertIn("--updater-helper", QUIESCE)
        self.assertIn("Pos('updater: ended', Lines[I]) = 1", procedure("RunUpdateQuiesce"))

    def test_both_languages_have_the_refusal_text(self):
        for language in ("english", "chinesesimplified"):
            self.assertTrue(re.search(rf"^{language}\.MsgUpdateBlocked=", ISS, re.M), language)

    def test_the_script_stops_the_service_first_and_ends_processes_by_path(self):
        self.assertLess(QUIESCE.index("System32\\sc.exe') stop"), QUIESCE.index("Stop-Process"))
        self.assertIn("ExecutablePath", QUIESCE)
        self.assertIn("StartsWith($rootFull", QUIESCE)

    def test_it_waits_for_a_quiet_period_because_a_tray_can_be_started_again(self):
        self.assertIn("$QuietMilliseconds", QUIESCE)
        self.assertRegex(QUIESCE, r"\$QuietMilliseconds = \d{4}")

    def test_it_reports_a_held_file_with_a_no_sharing_open(self):
        self.assertIn("[IO.FileShare]::None", QUIESCE)
        for code in ("exit 10", "exit 20", "exit 30", "exit 0"):
            self.assertIn(code, QUIESCE)

    def test_the_installers_own_copies_are_never_ended(self):
        self.assertIn("updates\\", QUIESCE)
        self.assertIn("Get-OwnAncestors", QUIESCE)

    def test_user_data_is_not_scanned(self):
        self.assertRegex(QUIESCE, r"\$userData = @\('config'")


class TheBehaviourIsExercisedOnARealRunner(unittest.TestCase):
    def test_the_smoke_script_runs_every_failure_the_installer_has_to_survive(self):
        for phase in (
            "update-running-program",
            "update-held-file",
            "update-respawning-program",
            "update-failing-copy",
            "update-after-interrupted-run",
            "update-after-half-copy",
        ):
            self.assertIn(phase, SMOKE)
        self.assertIn("Test-UpdateSafety", SMOKE)


if __name__ == "__main__":
    unittest.main()
