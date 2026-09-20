# Windows GUI package pairing

Windows builds on `master`, including manual builds, require a GUI artifact
matching the committed `src_assets/common/sunshine-control-panel` gitlink.
The workflow discovers a successful Panel `build.yml` run with a non-expired
`sunshine-gui-windows-x64` artifact for that commit. If none exists, packaging
stops with the required commit in the error message. Build the matching Panel
commit before retrying; there is no fallback to an older GUI release.

Development branches retain the default released GUI. To test the paired GUI
on a development branch, supply `gui_run_id` when dispatching `main.yml`.
The override must identify a successful Panel build at the exact gitlink.
The helper keeps the GUI executable and native plugin together and writes
`paired-build.json` with the repository, run and commit used.

For local staging, run `scripts/fetch-paired-gui.ps1 -Destination <fresh-path>`
from the Sunshine checkout. Omit `RunId` to discover a matching build, or pass
`-RunId <id>` to validate a specific build. Configure with `FETCH_GUI=OFF` and
`GUI_DIR=<fresh-path>` to use the verified bundle. GitHub artifact access requires
an authenticated `gh` session or an appropriate `GH_TOKEN`.
