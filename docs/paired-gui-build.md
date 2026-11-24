# Optional Windows GUI package pairing

Normal Windows builds, including pushes and manual builds on `master`, download
the latest available Control Panel release bundle through `FetchGUI.cmake`.
They do not require a GUI artifact matching the Panel submodule commit.

For an explicit paired development test, supply `gui_run_id` when dispatching
`main.yml`. Only this opt-in path uses `scripts/fetch-paired-gui.ps1`: the run
must be a successful Panel `build.yml` run at the committed
`src_assets/common/sunshine-control-panel` gitlink and contain a non-expired
`sunshine-gui-windows-x64` artifact. It keeps the GUI executable and native
plugin together and writes `paired-build.json` with the repository, run and
commit used. This overrides the release download for that run only.

For local paired staging, run `scripts/fetch-paired-gui.ps1 -Destination <fresh-path>`
from the Sunshine checkout. Omit `RunId` to discover a matching build, or pass
`-RunId <id>` to validate a specific build. Configure with `FETCH_GUI=OFF` and
`GUI_DIR=<fresh-path>` to use the verified bundle. GitHub artifact access requires
an authenticated `gh` session or an appropriate `GH_TOKEN`.
