# Spatial audio experiments

Current cross-PC entry point: [Atmos / MAT handoff](../docs/ATMOS-MAT-HANDOFF.md).

The top-level `.txt` reports and older READMEs are historical stage evidence,
not current production-readiness claims. Source-only capture/bridge experiments
are preserved here for further work; no recording, proprietary encoder, injected
binary, private key, APK or local toolchain cache is distributed with this branch.

Current acceptance uses `mat-android-session/` and the paired Android repository
cloned at `moonlight-vplus/` (not embedded in this repository). The 3.56 s fixture
must be copied privately. `tools/mat-handoff/make-bundle.ps1 -Output <NEW_DIR>`
creates an allowlisted local transfer ZIP; it does not upload anything.

Some historical reference-only probes require separately acquired upstream
projects under `mat-spike/reference/`; those checkouts are intentionally not
vendored. Some native scripts require recorded object-input directories omitted
from Git. They are not clean-machine one-command tests. Follow the current
handoff's dependency/path/ABI checks before choosing a test.
