# Build identity: `GET /api/version`, `sunshine --version --json`, VERSIONINFO

Every build records what exactly it is (`cmake/prep/build_info.cmake`). The same record is available three ways:

- `sunshine.exe --version --json` prints one JSON object and exits (no config is read, no server starts).
- `GET /api/version` (authenticated, like the rest of the API) answers the same object.
- The Windows VERSIONINFO of `sunshine.exe` carries the fields as custom String keys of `StringFileInfo\040904E4`,
  so the control panel reads them with `GetFileVersionInfoW` / `VerQueryValueW` without starting the host:
  `Commit`, `Branch`, `Dirty`, `Channel`, `BuildTime`, `Variant`, `Package`, `Builder`, `CiRun`.
  A local build also sets `VS_FF_PRIVATEBUILD` with the `PrivateBuild` string the flag requires.

```json
{
  "json_version": 1,
  "version": "2026.1104.121813",
  "commit": "96a1e2b6...40 hex",
  "commit_short": "96a1e2b6c0ff",
  "branch": "master",
  "dirty": false,
  "channel": "beta",
  "build_time_utc": "2026-11-04T04:18:13Z",
  "variant": "foundation-sunshine",
  "package": "installer",
  "package_source": "layout",
  "builder": "ci",
  "ci_run": "https://github.com/AlkaidLab/foundation-sunshine/actions/runs/123"
}
```

| Field | Values | Source |
|---|---|---|
| `commit` | full 40-hex commit, empty when unknown | `SUNSHINE_BUILD_COMMIT`, else `git rev-parse HEAD`, else (a `git archive` tree without `.git`) `cmake/prep/build_identity_archive.txt`, else `$COMMIT` |
| `dirty` | `true` when the tree had uncommitted changes | `git diff --quiet HEAD`; an archive of a commit is that commit, so `false` |
| `channel` | `stable`, `beta`, `dev`, `local` | `SUNSHINE_BUILD_CHANNEL`; CI: published master build `beta`, PR and manual runs `dev`; local builds `local` |
| `build_time_utc` | ISO 8601, UTC, `Z` | configure time; honours `SOURCE_DATE_EPOCH` |
| `variant` | product line | `SUNSHINE_BUILD_VARIANT`, default `foundation-sunshine` (Product Zero builds set `product-zero`) |
| `package` | `installer`, `portable` | `SUNSHINE_BUILD_PACKAGE`; the CI build makes both packages from one binary, so it records `auto` and the host decides from its folder: an Inno Setup uninstaller (`unins###.exe`) next to `sunshine.exe` means `installer` (`package_source: layout`) |
| `builder` | `ci`, `local` | `SUNSHINE_BUILD_BUILDER`, else `ci` when `$CI` / `$GITHUB_ACTIONS` is set |
| `ci_run` | run URL | `SUNSHINE_BUILD_CI_RUN`, else the GitHub run URL |

The display version (`version`, the FileVersion and ProductVersion strings) is unchanged.

## Source trees without `.git`

Windows test builds and other tar-based builds get their source from `git archive`. `.gitattributes` marks
`cmake/prep/build_identity_archive.txt` `export-subst`, so `git archive` writes the commit (`%H`) and its refs (`%D`) into it;
`build_info.cmake` reads it when git cannot answer and takes the first branch of the refs. In a checkout the file keeps
its `$Format` placeholders and is ignored. A tar made some other way (a copy of a working tree) has neither: pass
`SUNSHINE_BUILD_COMMIT` / `SUNSHINE_BUILD_BRANCH` / `SUNSHINE_BUILD_DIRTY` to its configure, otherwise the commit stays empty
and the panel shows no build details. Test: `tests/cmake/test_build_info_archive.cmake` (ctest `build_info_archive`).

## Comparing versions

Everything that compares two Foundation Sunshine versions (the installer's maintenance page, the panel's updater, the Web UI's update card)
uses the first three numeric parts only, `YYYY.MDD.HMMSS`, unpadded (CI tag `v2026.1103.21122`) or zero-padded (local build
`2026.1112.025657`) alike. What follows is never compared: `.杂鱼` (every CI release), `.<short commit>` (a build off another branch),
any other text. A manual CI build, `manual-YYYYMMDD-HHMMSS[-dev]`, compares by that embedded date; a string with no date version is
unknown, and unknown is never newer, older or the same. A version string never says "dirty": only `dirty` of this endpoint does
(the doctor's fallback for a host without build info reports no dirty state).

The rules are in `src/version_compare.h` (host and installer), `update/metadata.rs` (panel) and `sunshine_version.js` (Web UI), and all three run
`tests/fixtures/version_vectors.json` (`version_compare_unit_tests`, the panel's `version_compare` tests, `tests/versionCompare.test.js`).
The panel keeps an identical copy of the table in `src-tauri/tests/fixtures/`.

## Binary FILEVERSION

The binary FILEVERSION / PRODUCTVERSION of VERSIONINFO is four 16-bit fields. The version string
`YYYY.MMDD.HHMMSS` cannot go there as is (`HHMMSS` reaches 235959 > 65535), so `cmake/prep/rc_version.cmake`
encodes it as `YYYY, MMDD, HHMM, SS`: `2026.1104.235959` → `2026,1104,2359,59`. A suffix (`.commit`, `.杂鱼`) does not
change it, a later build time always gives a larger binary version, another numeric `a.b.c[.d]` whose parts fit is used
as is, and a version that is not numeric (a manual CI tag) gives `0,0,0,0`.

Before this the fields came from `project(Sunshine VERSION 0.0.0)`, so every build was `0.0.0.0` (`build_version.cmake`
only changes the string). `PROJECT_VERSION_MAJOR` and friends are left alone (they also set the `SOVERSION`).

Consumers checked: nothing compares the binary fields. The Inno Setup script copies every file with `ignoreversion`,
does not set `VersionInfoVersion` and finds a previous install through the registry; the panel (version, updater,
component compatibility) reads the FileVersion string; the update channel metadata and the doctor compare version
strings.

Test: `tests/cmake/test_rc_version.cmake`, ctest `rc_version_encoding` (`cmake -P` on any platform). The channel is the channel the build was made
for; a release promoted later keeps its binary, so "stable" is only recorded when the build environment says so.

References: VERSIONINFO resource (https://learn.microsoft.com/windows/win32/menurc/versioninfo-resource),
String structure of StringFileInfo (https://learn.microsoft.com/windows/win32/menurc/string-str),
VerQueryValueW (https://learn.microsoft.com/windows/win32/api/winver/nf-winver-verqueryvaluew),
CMake string(TIMESTAMP) and SOURCE_DATE_EPOCH (https://cmake.org/cmake/help/latest/command/string.html#timestamp).
