# Sparxie host API

The host exposes an optional Sparxie component over the existing configuration
server. Sparxie stays a separate program. This API detects it, reads and writes
its `config.json`, detects a local game install and its region, stores a normal
Sunshine application with a `sparxie` block, reads and writes that game's
display and quality settings, and caches official launcher art.

Authentication is the same policy as `POST /api/apps/scan`
(`docs/api/apps-scan.md`): `authenticate()` applies the browser origin,
referer, `Sec-Fetch-Site` and host checks, then the remote-access and password
policy. Request bodies are `Content-Type: application/json`. The calls are not
restricted to loopback.

A JSON error has HTTP status 4xx and this body:

```json
{ "ok": false, "error": "human readable", "error_code": "sparxie_running" }
```

`error_code` is a stable token. `error` is text for a person.

| `error_code` | HTTP | When |
|---|---|---|
| `absent` | 409 | The call needs the Sparxie program and it is not installed. Detection, art and the warning do not use this. |
| `sparxie_running` | 409 | Sparxie is open, so the host will not write `config.json`. |
| `schema_unsupported` | 409 | `schemaVersion` is not 1. The file is left untouched. |
| `config_unreadable` | 409 | The file exists but is not valid schema 1 JSON. The host does not rewrite it. |
| `config_conflict` | 409 | The file changed while the host was preparing a write. |
| `write_failed` | 500 | The temp file could not be committed, or the read-back check failed and the backup was restored. |
| `apps_write_failed` | 500 | The application list (apps.json) could not be saved. A Sparxie profile that the same request had added is taken out of Sparxie's config again. |
| `internal_error` | 500 | The host raised an exception while handling the request; `error` carries its text. |
| `unrecognised` | 404 | The path has none of the known game executables. |
| `ambiguous` | 409 | The folder contains more than one known game executable. Pass the executable path. |
| `invalid_region` | 400 | `region` is not one of the remaining candidates. |
| `invalid_input` | 400 | A field is missing or out of range. |
| `game_running` | 409 | The game process is running, so its settings are not written. |
| `not_found` | 404 | No such profile, install or application. |
| `picker_unavailable` | 409 | This session cannot open a file dialog. Send the path to `identify` instead. |
| `art_unavailable` | 404 | No cached image and the network fetch failed. |

`GET /api/sparxie/status` does not use these codes for a missing component. It
returns 200 with `"installed": false`.

## Identifiers

Games in this API are `genshin`, `starrail`, `zzz`, `hi3`, `wuwa`,
`pgr`, `arknights`, `endfield` and `nte`; every title's detection,
regions, launch and settings are tabled in
[docs/sparxie-games.md](sparxie-games.md). Regions are `cn`, `bili` and
`global`, and a game lists only the regions it has. Sparxie's own file uses
different game tokens (`genshin`, `starRail`, `zenlessZoneZero`). The host
translates at the file boundary. Region is not a Sparxie field. It lives only
in the Sunshine application's `sparxie` block.

An install object:

```json
{
  "id": "genshin-cn",
  "game": "genshin",
  "region": "cn",
  "regionName": "国服",
  "candidates": [],
  "markers": ["exe", "registry"],
  "source": "hoyoplay",
  "dir": "D:\\Genshin",
  "exe": "YuanShen.exe",
  "version": "5.8.0",
  "biz": "hk4e_cn",
  "profile": "",
  "art": { "version": "", "updated": "" }
}
```

`region` is empty while more than one candidate remains. `candidates` lists
only the regions still possible. `regionName` (国服, B 服, 国际服) is the
region's display name and is present once the region is decided. `markers`
is a subset of `exe`, `registry`, `sdk`, `ini`, `dir`. `sources` says
everywhere the host found the install: `hoyoplay` (the HoYoPlay
registry), `bili` (the Bilibili launcher's registry record), `steam`,
`epic`, `official-launcher` (another vendor's launcher record: the
Wuthering Waves, NTE, Endfield and Yostar files named in
[docs/sparxie-games.md](sparxie-games.md)); empty when it found none (the page marks a path the person chose as
`folder` or `manual`, and a Sparxie profile as `sparxie`). The same
directory seen by two sources is one install with both. `source` is the one
that answered the identification (a registry or store record; `manual` when
nothing else did). `profile` is the id of a Sparxie profile whose executable
is this install, or `""` when there is none.
`id` is `{game}-{region}` when the region is known, otherwise
`{game}-open-{8 hex chars of the directory}`.

A Sunshine application is unchanged apart from a `sparxie` object:

```json
{
  "name": "原神 · 国服",
  "cmd": "\"C:\\Program Files\\Sunshine\\components\\sparxie\\Sparxie.exe\" --profile genshin-cn-1",
  "elevated": true,
  "working-dir": "D:\\Genshin",
  "image-path": "C:\\ProgramData\\Sunshine\\covers\\sparxie-genshin-cn-<version>.png",
  "sparxie": {
    "game": "genshin",
    "region": "cn",
    "install": "genshin-cn",
    "profile": "genshin-cn-1",
    "mode": "touch",
    "cover": "official",
    "artVersion": "",
    "artUpdated": "",
    "restore": "restore",
    "settings": {
      "vsync": true,
      "hdr": true
    },
    "options": {
      "fpsUnlock": true,
      "fps": 120,
      "bgLimit": true,
      "bgFps": 10,
      "priority": "normal",
      "followPreset": false,
      "preset30": 60,
      "preset60": 1000,
      "scaleOverride": false,
      "scale": 400
    },
    "launchTemplate": "\"{sparxie-launch}\" --sparxie \"{sparxie}\" --profile {profile}",
    "launchTemplateProvisional": true,
    "exe": "D:\\Genshin\\YuanShen.exe",
    "dir": "D:\\Genshin",
    "candidates": [],
    "markers": ["exe", "registry"]
  }
}
```

`mode` is `touch` or `pc` (`direct` is accepted and stored as `pc`: 直接启动, always available; `touch` launches through Sparxie and is only for the titles whose catalog row says `touch`, and only while the component is installed). `pc` launches the game executable directly
(keyboard and gamepad) and does not write a Sparxie profile. `touch` launches
with the template below and `elevated: true`.

The `pc` command starts the build the way its source wants: a Steam install
starts `steam://rungameid/<app id>`, an Epic install starts
`com.epicgames.launcher://apps/<AppName>?action=launch&silent=true` (the
install object carries the manifest AppName as `epicApp`), and a launcher
build of Arknights or Endfield starts the official Hypergryph launcher
(`Launcher.exe` in a `Hypergryph Launcher` install root) when it is installed,
because those games check their parent process and a hand start answers
10100. Every other build starts its own executable.

Many applications may share one game and one region. Each touch application
owns its own profile id.

`options` is the per-application Sparxie profile. `fps` and `bgFps` are
integers from 10 to 1000. `scale` is an integer from 100 to 500.
`priority` is `normal`, `belowNormal`, `aboveNormal` or `high`.
Genshin-only fields (`followPreset`, `preset30`, `preset60`,
`scaleOverride`, `scale`) are ignored for the other games. Zenless Zone Zero
has no `hoyo` object in Sparxie's file. The host's created profiles default
`fpsUnlock` to `true` with `fps` 120, the way Sparxie's own editor starts
a profile. `options.input` (`touch` or `kbm`, default `touch`) is stored
with the application, and a Sparxie profile of Genshin or Star Rail carries it
as `hoyo.touchUiEnabled` (`false` for `kbm`): Sparxie's `HoyoSettings.touch_ui_enabled`,
a session that installs the frame-rate unlock and the other runtime settings but not the touch interface
(see "Input, touch interface and `touchUiSwitch`" below). Zenless Zone Zero has no `hoyo` object, so it stores nothing.
A profile without the field (an older Sparxie, or one written by hand) is a touch profile; importing it gives `options.input: "touch"`.

`settings.display` is `fullscreen` or `windowed`. These three games do
not document a borderless mode. Sending `borderless` is `invalid_input`.
`settings.fps` is the game's own frame-rate tier (Genshin 30, 45 or 60, Star Rail
30, 60 or 120, Zenless Zone Zero 30, 60 or 0 for unlimited), which is separate
from Sparxie's `options.fps` unlock.

`settings` holds only the items the person changed (sparse overrides). Every other item
follows the game: the editor shows the game's own current value with a 跟随游戏 mark and
the host never writes it. Changing an item makes it the application's own value (已更改),
and each item (or all of them, 全部跟随游戏) can go back to following the game. An
application that changed nothing has `"settings": {}`; there is no settings mode (the old
`settingsMode` of `auto` / `preset` is gone, and a stored `settingsMode: "auto"` is read as
holding nothing).

`restore` is `restore` or `keep`. `restore` puts back, when the session ends, the values this
launch wrote. Each value goes back where the game has not changed it since: a registry
value that is a JSON document the game rewrites (Genshin keeps most items in one) is compared
item by item, so an item the game changed while it ran keeps the game's value and every item
this launch wrote and the game left alone goes back. `keep` leaves everything as written.

## Described settings, input and frame-rate ownership (interface the Web UI draws from)

A Sparxie title always starts through Sparxie while the component is installed (`mode: "touch"` is the stored value of that path; `"sparxie"` is accepted as a name for it, `"pc"`/`"direct"` is the game started directly, used without the component). There is no launch choice in the UI.
Input (touch, or keyboard, mouse and gamepad) is a setting of the Sparxie session, `options.input`: `"touch"` (default) or `"kbm"`. For Genshin and Star Rail it reaches Sparxie as `hoyo.touchUiEnabled` / `HoyoSettings.touch_ui_enabled` (see the next section); the host also keeps it in the application and uses it to decide which touch-only items apply. Neither input hides the other Sparxie settings (frame-rate unlock, the tiers, the Genshin preset mapping, background frame rate, priority).

### Input, touch interface and `touchUiSwitch`

Sparxie's runtime (the `Genshin_StarRail_fps_unlocker` code) installs the game's mobile interface only on request. A Sparxie build with the switch
(`HoyoSettings.touch_ui_enabled`, proto3 `optional bool`, field 11; absent means true) starts a session without that interface when it is `false`, and the
frame-rate unlock works the same. `GET /api/sparxie/status` reports whether the installed build has it:

```json
{ "installed": true, "capabilities": ["touch-ui-switch"], "touchUiSwitch": true }
```

`capabilities` is read from `capabilities.json` in the component's `app-<version>` folder (`{ "schema": 1, "capabilities": ["touch-ui-switch"] }`, written by the Sparxie package);
a build without that file (preview.9 and older) has none and `touchUiSwitch` is `false`. The launcher checks the Broker as well: it sends `touch_ui_enabled` in every `StartSession`,
and when the profile says `false` while the Broker's `Ping` lists no `touch-ui-switch` it refuses to start (exit code 11, logged), because an older Broker would ignore the field and start the touch interface.

* `touchUiSwitch: true`: a `kbm` application keeps `mode: "touch"` (the launcher, Sparxie's settings) and its profile says `touchUiEnabled: false`.
* `touchUiSwitch: false` with Sparxie installed: a `kbm` application is created (and saved) with `mode: "pc"`, the game started directly; Sparxie's settings do not apply to it, and the page says so.
  Saving it again after a newer Sparxie is installed makes it a Sparxie application. Without the component at all the mode is whatever the caller sent.
* An application made before the switch existed (its profile has no `touchUiEnabled`) is brought in line at its next launch: `prepare_launch` writes `touchUiEnabled: false` into the profile when the application says `kbm`, the installed build has the switch and Sparxie is not open (a refused write leaves the profile as it is).
* A profile with both `touchUiEnabled: false` and `fpsUnlockEnabled: false` has nothing for Sparxie to install; Sparxie's Broker refuses it (`touchUiEnabled` and `fpsUnlockEnabled` cannot both be false).

`GET /api/sparxie/games` rows in `catalog` MAY carry two lists of described items. The page draws exactly what is listed; an item the host does not list is not shown. A host that sends neither gets the items of Sparxie's profile that the title has (no frame-rate tiers).

```json
{ "game": "genshin",
  "settings": [ { "key": "fps", "kind": "choice", "section": "display", "options": [30, 45, 60] },
                { "key": "renderScale", "kind": "slider", "section": "quality", "min": 0.6, "max": 1.4, "step": 0.1, "decimals": 1 },
                { "key": "shadow", "kind": "choice", "section": "quality", "label": "阴影质量", "labelEn": "Shadow quality",
                  "options": [ { "value": 1, "label": "低", "labelEn": "Low" } ] } ],
  "sparxie": { "fields": [
      { "key": "input", "kind": "choice", "section": "input", "options": ["touch", "kbm"], "default": "touch" },
      { "key": "fpsUnlock", "kind": "toggle", "section": "fps", "default": true },
      { "key": "fps", "kind": "choice", "section": "fps", "options": [60, 90, 120, 144, 165, 240, "display"], "default": 120, "enabledBy": "fpsUnlock" },
      { "key": "scale", "kind": "slider", "section": "ui", "min": 100, "max": 500, "step": 5, "enabledBy": "scaleOverride", "inputs": ["touch"] } ] } }
```

Item fields: `key`, `kind` (`choice` | `toggle` | `slider`), `section`, `label`/`labelEn` (for a key the page has no words for), `options` (values, or `{ value, label, labelEn }`), `min`/`max`/`step`/`decimals`/`scale: "log"`/`marks`/`unit` for sliders, `default`, `enabledBy` (the item is off while that toggle is off), `inputs` (the inputs the item is for; an item for `["touch"]` is hidden, not greyed, for `kbm`; an item without `inputs` is for every input).
- `settings` are the items of the game's own settings menu (the host reads and writes them; an application's stored `settings` holds only the items the person changed).
- `sparxie.fields` are the items of Sparxie's own profile that apply to the title. Zenless Zone Zero has none of the `hoyo` items in Sparxie (its Desktop sends them for Genshin and Star Rail only), so its list holds `input` only.
- The frame-rate tiers (`fps`, `preset30`, `preset60` options) are the host's list; there is no free number. A tier `"display"` follows the display's refresh rate; the host resolves it to a number when it writes Sparxie's `targetFps`.
- While `options.fpsUnlock` is true the game's own frame rate belongs to Sparxie: the page marks the item "frame rate is set by Sparxie" and leaves `fps` out of `settings`; the host must not write the game's frame rate for such an application.

This host sends the lists. `settings`: every title has `resolution`,
`display` and the game's own frame-rate tiers (`30, 45, 60` for Genshin,
`30, 60, 120` for Star Rail, `30, 60, 0` for Zenless Zone Zero); Genshin
and Star Rail add `vsync`, `preset` and `renderScale`; Genshin adds
`hdr`. Star Rail's and Zenless Zone Zero's preset lists include `custom`
(the game's own mode in which the quality sub-items take effect). Star Rail has
no HDR item and no HDR field in `unavailable`.
Zenless Zone Zero lists the file-backed items (`fps`, `vsync`,
`preset`, `renderScale`, `hdr`); they read as `unavailable`, and are never
written, only when the game's own file does not decode with the shipped key.
No title lists an
exclusive or borderless window mode: none of the three games stores one
readably (Genshin's registry value is fullscreen-or-window; Star Rail and
Zenless Zone Zero store 1 fullscreen or 3 window), and no quality sub-item
shadow, reflection or the like is listed because the value domains of those
entries are not recorded anywhere this host may read; the keys of the sub-items
it does know (the Genshin `saveItems` entry types, the Star Rail model
fields) stay out of the list until their domains are verified. `sparxie.fields`:
every title has `input`; Genshin and Star Rail add `fpsUnlock`, `fps`
(60, 90, 120, 144, 165, 240, `display`), `bgLimit`, `bgFps` and
`priority`; Genshin alone adds `followPreset`, `preset30`, `preset60`,
`scaleOverride` and `scale`, the last two for `inputs: ["touch"]`.

`fps: "display"` resolves at launch: `prepare_launch` reads the refresh
rate of the display this host streams (the configured output when it is
active, else the primary, else any active device) and rewrites that profile's
`targetFps` with the number before the game starts. A write while Sparxie is
open, or an unreadable rate, leaves the last resolved number in place.

## Component presence

`GET /api/sparxie/status`

```json
{
  "installed": true,
  "running": false,
  "version": "",
  "protocol": "",
  "runtime": { "hoyo": "", "zzz": "" },
  "capabilities": [],
  "touchUiSwitch": false,
  "dataDir": "C:\\Users\\me\\AppData\\Local\\Sparxie",
  "dataDirProvisional": true,
  "launchTemplate": "\"{sparxie-launch}\" --sparxie \"{sparxie}\" --profile {profile}",
  "launchTemplateProvisional": true,
  "store": { "state": "empty", "profiles": 0 },
  "warned": false,
  "statement": "…",
  "statement_en": "…"
}
```

`installed` is true when `Sparxie.exe` or `Sparxie.Launcher.exe` is found.
The search order is `SPARXIE_HOME`, then `components/sparxie` under the
host data directory (the writable install location), then `components/sparxie`
beside the host executable. `version` comes from `component.json` or
`version.ini` beside that executable. There is no `version --json` command
in the build this host was written against, so `protocol` and `runtime`
stay empty until that command exists.

The same response carries `launchNotice`: `null`, or the note the touch launcher left after a launch that needed its safety net (`config\\sparxie\\launch-notice.json`: `version`, `id`, `time` (UTC), `kind` (`direct`: Sparxie could not start the session and the game was started directly; `retried`: the first session failed and the automatic retry worked; `unlock_forced`: the installed Hoyo runtime cannot run touch only, so the game started with the unlock at 60 FPS), `profile`, `game`, `message` (Chinese), `messageEn`, `reason`, `log`). A clean launch removes the note, so it always describes the last launch; a page shows `message` once per `id`.

The same response also carries `noticeAck` (true only after
`POST /api/sparxie/warning`), `installHint` / `installHintEn` (the one line
that tells the person to bring the package and choose it here), `componentDir`,
`dependencies` and `manifest`. `dependencies` is a list of
`{ id, result, detail, required }` with ids `os`, `disk`, `webview2`,
`vcruntime`, `admin` and `quarantine`. `result` is `ok`, `error` or `skip`.
`manifest.online` is false: this host does not download the package.

`running` is true when `Sparxie.exe`, `Sparxie.Desktop.exe` or
`Sparxie.Launcher.exe` is running. Those are the processes that keep
`config.json` in memory. A broker left behind by a game session does not by
itself set `running`.

`store.state` is `unused` (component not installed and no file),
`empty` (no profiles), `data` (one or more profiles) or `unreadable`.
`store.profiles` is the count.

`warned` is false until `POST /api/sparxie/warning` records that the
statement was shown. `statement` and `statement_en` are always present,
including after acknowledgement and when the component is absent. The text
states that Sparxie injects code into the game process and writes that
process's memory, that this can conflict with the game's terms, and that the
account risk belongs to the person using it.

Not installed:

```json
{
  "installed": false,
  "running": false,
  "version": "",
  "store": { "state": "unused", "profiles": 0 },
  "warned": true,
  "statement": "…",
  "statement_en": "…"
}
```


## Install from a file

There is no download. The person obtains the Sparxie package and chooses the
ZIP here. `GET /api/sparxie/manifest` returns the pinned known versions
(version, file name, byte size, SHA-256) and no download address.

`POST /api/sparxie/install` with `{ "path", "confirm_unverified", "acknowledged" }`
starts the install and returns `{ job, done: false, stage }`. Poll
`GET /api/sparxie/install/<job>` until `done` is true. Send `"wait": true`
to run the install in the request and receive the final body directly.
Stages are `dependencies`, `verify`, `extract`, `probe` and `done`.

The ZIP is hashed and compared with the pinned SHA-256 values. A match is
`verified: true` and the version is the pinned label. Any other hash returns
`error_code` `unverified` and does not write files until the same call is
repeated with `confirm_unverified: true`. A file whose size matches a pinned
package but whose hash does not returns `hash_mismatch` and the same
`confirm` value, `未经验证的版本`. Repeating that call with
`confirm_unverified: true` installs it with `verified: false`, `version` and
`mark` set to `未经验证的版本`. Entry names may use backslashes; they are normalised to
`/`. A name that is absolute, has a drive letter, or contains a `.` or `..`
segment is `zip_slip` and nothing outside the install directory is written.
Versions are extracted side by side as `app-<version>`. `config.json` already
in the directory is left in place.

The first install requires the injection statement. Until `warning.json`
records it, the install returns `notice` unless `acknowledged` is true, in
which case the acknowledgement is stored. Other `error_code` values:
`os_too_old` (Windows 10 build 19041 x64 is the minimum), `webview2_missing`,
`vcruntime_missing`, `no_admin` (the data directory is not writable and the
process cannot elevate; elevation is not required when the directory is
writable), `disk_full`, `quarantined` (a file written by the install is
missing afterwards; this host does not add an exclusion), `sparxie_running`,
`bad_zip`, `not_found`, `busy`.

`POST /api/sparxie/uninstall` removes the component directory. If
`config.json` is in that directory and `delete_config` is omitted, the call
returns `confirm_config` and deletes nothing. `delete_config: false` removes
the program files and keeps `config.json`. `delete_config: true` removes it.

## Detection

`GET /api/sparxie/games`

```json
{ "installed": true, "stored": [], "detected": [] }
```

`detected` is every install the launchers and stores record: the HoYoPlay
and Bilibili registry values whose `GameInstallPath` exists on disk, the
Steam library that holds Zenless Zone Zero, and the Epic Games Launcher
manifests that hold Genshin Impact or Honkai: Star Rail. `stored` is one
install per Sparxie profile, identified from that profile's executable.
Both arrays use the install object. This call works when Sparxie is absent:
`stored` is then empty and `detected` still lists the registry and store
installs.

`POST /api/sparxie/identify` with `{ "path": "D:\\Genshin" }` or the
executable path.

```json
{ "ok": true, "install": { }, "has_game": true }
```

The path is a folder or an executable. Identification order:

1. Executable file name. `YuanShen.exe` is Genshin (`cn` or `bili`).
   `GenshinImpact.exe` is Genshin `global`. `StarRail.exe` is Star Rail.
   `ZenlessZoneZero.exe` and `ZenlessZoneZeroBeta.exe` are Zenless Zone
   Zero. No known executable is `unrecognised`. Two different known games in
   one folder is `ambiguous`.
2. HoYoPlay `GameInstallPath` equal to that directory. National keys are
   `HKCU\Software\miHoYo\HYP\1_1\{biz}`. International keys are
   `HKCU\Software\Cognosphere\HYP\1_0\{biz}`. `biz` is `hk4e_cn`,
   `hk4e_global`, `hkrpg_cn`, `hkrpg_global`, `nap_cn`, `nap_global`;
   the Bilibili launcher's records are `hk4e_bilibili`, `hkrpg_bilibili`
   and `nap_bilibili` under the national publisher. Other launcher versions
   are found by enumerating the same two hives. A hit whose game matches the
   executable selects that region and sets `source` to `hoyoplay` or
   `bili`.
3. `{Data}\Plugins\PCGameSDK.dll` beside the executable
   (`YuanShen_Data`, `GenshinImpact_Data`, `StarRail_Data`,
   `ZenlessZoneZero_Data`). Present, and `bili` still possible, selects
   `bili`.
4. A Steam or Epic record that names the folder. Steam is read from the
   `HKCU\Software\Valve\Steam` value `SteamPath`: `steamapps\libraryfolders.vdf`
   lists the libraries and the game's `appmanifest_<app id>.acf` in a
   library names the install folder (4162040 Zenless Zone Zero, 1671200
   Honkai Impact 3rd, 3513350 Wuthering Waves, 4125930 Punishing: Gray
   Raven, 4732690 Endfield, 4508340 NTE — Valve's own store answers); the
   store build is the international client. Epic is read from the launcher's manifest files
   under `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\`: each `.item`
   names an `InstallLocation`, which is searched for a known game
   executable. A match sets `source` to `steam` or `epic` and leaves `global`
   as the only candidate. When the launcher registry points at the same
   folder the store wins the `source`: the registry only keeps a store
   install up to date.
5. `config.ini` `[General]`: `game_biz` first (the launcher writes the
   region's biz there, for example `hk4e_bilibili`), then `channel` and
   `sub_channel` (national 1/1, bilibili 14/0, international 1/0, the values
   the reference launchers write), then `cps` (`hyp_hoyoverse` leaves only
   the international region, `hyp_mihoyo` only the national and bilibili
   ones). A value selects or narrows the region only while that region is
   still a candidate. The raw numbers are returned as `channel` and
   `sub_channel` on the install.
6. Anything still ambiguous returns `region: ""` and the remaining
   `candidates`. The client asks the person to choose. Nothing left to try,
   after step 1 failed, is `unrecognised`.

The Bilibili registry key spelling and the Epic manifest layouts are not
documented by their publishers; both follow what the reference launchers and
other installers read, and stay unverified until a real install of each is
checked. The Google Play Games build of Genshin Impact runs inside its own
container and is deliberately not detected: it has no game executable this
host could start.

`POST /api/sparxie/pick` is kept for older clients and always answers 409
`picker_unavailable`. The host is a service on Windows: a dialog it opened would
land in the service's own profile, not in the person's session, so the host
never opens one. The control panel opens its own dialog (a process of the
person's session) and sends the chosen path to `identify` or `scan`; a browser
uses `browse`.

`identify` takes optional `game` and `region`. A path whose game or executable
name does not fit them is 409 `wrong_game` with `allowed`, the names that fit
(Genshin Impact's international client is `GenshinImpact.exe`; the national and
bilibili clients are `YuanShen.exe`). Sparxie's own folder is 409 `is_sparxie`
for `identify` and `scan`.

`POST /api/sparxie/scan` with `{ "path", "game"?, "region"?, "depth"? }`
searches a chosen folder level by level, breadth first, starting at the folder
itself (level 0). It stops at the first level that holds a known game
executable and lists every install of that level.

```json
{ "ok": true, "installs": [ { "…": "install object", "depth": 1 } ], "depth": 4, "searched": 12, "timed_out": false, "truncated": false }
```

`depth` is at most 4, the search stops after 8 seconds or 20000 folders
(`timed_out`, `truncated`), and Sparxie's own folder, `$` folders, `Windows`,
`.git` and `node_modules` are not entered. Nothing found is 404 `unrecognised`
with the same counters. Reads happen as the signed-in user; nobody signed in is
409 `no_user_session`.

`POST /api/sparxie/browse` with `{ "path"?, "files"?: "exe" }` lists drives
(empty path) or one folder as the signed-in user, read-only:
`{ ok, path, parent, entries: [{ name, path, kind: "drive"|"dir"|"file", known? }], truncated }`.
Folders come first, then `.exe` files with the known game executables leading;
at most 3000 entries. A network path (`\\server\share`) is 400 `invalid_input`.

`POST /api/sparxie/open` starts the Sparxie program in the signed-in user's
session with the host's launch-as-user path (never as the service):
`{ ok, already_running }`. Not installed is 409 `absent`.

`GET /api/sparxie/games` also carries `catalog` (per game: its executables and,
per region, the executables that region ships; plus `name` and `nameEn`, `aliases` (what people type: 星铁, HSR, ZZZ) and
`touch`, whether Sparxie's touch launch supports the title). The pages list the
titles in this answer, so a new title is a new row here and nothing in the pages) and `art`, the version (content
hash) of the official poster each game and region has now:
`{ "genshin-cn": { "version": "…" } }`. A poster that changes is a new address.
The host fetches the posters of all nine games and regions, installed or not,
one cache per install id.

## Profiles and applications

`GET /api/sparxie/profiles`

```json
{
  "installed": true,
  "profiles": [
    {
      "id": "genshin-cn-1",
      "name": "原神",
      "taken": false,
      "game": "genshin",
      "install": { }
    }
  ]
}
```

`taken` is true when some Sunshine application already references that
profile id. A profile whose executable is not a known game is omitted from
`install` and the row is still returned with `install: null`; the Web UI
client drops rows without an install.

`POST /api/sparxie/apps`

```json
{
  "install": "genshin-cn",
  "path": "D:\\Genshin\\YuanShen.exe",
  "region": "cn",
  "mode": "touch",
  "name": "原神 · 国服"
}
```

Optional: `settings` (only the items the person changed; none = every item follows the game),
`restore` (`restore` or `keep`), and a cover of the person's own: `cover`
`"custom"` with `image-path` and/or `hero` (a local file or an address). A
custom cover is kept as given and a refreshed official poster never replaces
it.

`badge` (a boolean) is the Sparxie mark on the posters, see
[Poster badge](#poster-badge). It is read for a touch application only.

`display` is the application's display choice, see [Display](#display):
`{ "mode": "follow" }` (the default) or `{ "mode": "fixed", "resolution": "1920×1080",
"window": "fullscreen" | "borderless" | "windowed", "resolutionIndex": "3" }`, every value a string.

`install` may be the install id or the install object returned by
`identify`. `region` is required when identification left more than one
candidate, and it must be one of those candidates.

Response:

```json
{
  "ok": true,
  "app": { },
  "saved": true,
  "index": 3,
  "wrote": true,
  "write_back": {
    "state": "written",
    "profile": "genshin-cn-1",
    "backup": "config.backup-20261110T120000Z.json",
    "dataDir": "C:\\Users\\me\\AppData\\Local\\Sparxie",
    "dataDirProvisional": true,
    "launchTemplateProvisional": true
  }
}
```

The host appends the application to the Sunshine application list and returns
it. `index` is its position. `wrote` is true only when `config.json` was
modified.

`write_back.state` is one of:

| state | Meaning |
|---|---|
| `written` | A profile was appended and the read-back check passed. |
| `created` | Sparxie had no file. The host created schema 1 at the resolved data directory. |
| `skipped` | `mode` is `pc`, so no profile was written. The command is the game executable. |
| `refused_running` | Not written because Sparxie is running. HTTP 409 `sparxie_running`. |
| `refused_schema` | Not written because `schemaVersion` is not 1. HTTP 409. |
| `refused_unreadable` | Not written because the existing file failed validation. HTTP 409. |

Write-back follows Sparxie's own add-game rules:

* `schemaVersion` must be 1, otherwise the host refuses and does not touch the file.
* The host copies the current file to `config.backup-<UTC>.json` and keeps the newest five backups.
* The profile is appended to the parsed JSON document, so fields this host does not know survive. `selectedProfileId` is set to the new id only when it was empty.
* The result is validated (unique non-empty id, non-empty name, known game, executable file name on the whitelist, no `hoyo` object for Zenless Zone Zero, frame-rate 10–1000, touch scale 100–500, known priority).
* The host writes a temporary file in the same directory, flushes it, and replaces `config.json`. On Windows the replace is `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. A sharing violation is retried three times, 50 ms apart.
* If the file's modification time or SHA-256 changes while the new document is built, the host re-reads and applies the same addition once. A second change is `config_conflict`.
* The host reads the file back and checks the new profile. If that check fails, it restores the backup and returns `write_failed`.
* The host does not write while Sparxie is running.

When Sparxie is not installed, the host still creates the file so a later
install reads it. The directory is provisional, in this order: `SPARXIE_DATA_DIR`
when set, otherwise the writable directory that contains `Sparxie.exe` (the
portable root, not an `app-<version>` subdirectory), otherwise
`%LOCALAPPDATA%\Sparxie`. `dataDirProvisional` stays true until upstream
documents a data-directory command.

The touch command is the host-owned launcher template `"{sparxie-launch}" --sparxie "{sparxie}" --profile {profile}`.
`{sparxie-launch}` is installed in the host `tools` directory; it loads the profile and speaks the Broker contract. `{sparxie}` is replaced by the component executable path. The helper follows source record `sparxie-broker-contract-031efe2`.

The launcher (`tools/sparxie-launch`, published as `sparxie-launch.exe`) is a Windows-subsystem program: no console and no window of its own.
It starts `Sparxie.Broker.exe` at once with `--pipe-name=<random> --parent-pid=<its pid>` (directly when it is already elevated, which is the
normal case because the host starts Sparxie applications elevated; through `runas`, so with a UAC prompt, only when it is not), pings the
Broker, opens the event stream, sends `StartSession` with the profile and every `hoyo` setting, and then stays alive until the final event.
It exits 0 when the session ends with `Exited`, 7 for `Failed` or a crashed SessionHost, and another non-zero code for each earlier failure
(2 command line, 3 config or profile, 4 Broker, 5 session refused, 6 pipe error, 8 event stream lost, 9 the game never reached Running, 10 unexpected, 11 the profile needs a Sparxie feature the installed build lacks).
Every run writes `config\logs\sparxie-launch-<yyyymmdd-hhmmss>.log` beside the host (the newest 20 are kept; `%LOCALAPPDATA%\Sunshine\logs` when
that folder cannot be written; `SPARXIE_LAUNCH_LOG_DIR` overrides). The Broker, SessionHost and game are put in a job object that kills them when
the launcher ends, so ending the Sunshine application, even by killing the launcher, ends the game.

A launch is not allowed to end in failure while the game can still be started. A Hoyo profile whose frame-rate unlock is off ("touch only") is sent with the unlock on at 60 FPS, the
game's own cap, when the installed `HoyoTouchCore.dll` is a build known to fail touch-only sessions (preview.9: the session ends `Failed`, Hoyo error 6). If a session fails before the
game runs, the launcher retries once with a new Broker and no job object of its own; if that fails too, it starts the game directly (elevated, Sunshine's working directory) and follows it,
so its exit code is the game's. Each of these leaves the note described under `launchNotice`.
Upstream removed the console entry point (commit `8a69c36`); this template remains provisional
(`launchTemplateProvisional: true`) until the publisher confirms the launch
arguments. A host file `sparxie-host.json` beside the host's Sparxie data may
set `launchTemplate`. Placeholders are `{sparxie}`, `{profile}`,
`{exe}`, `{dir}`, `{id}` and `{name}`.

`PUT /api/sparxie/apps/{index}` changes a game application in place. Any of
`name`, `mode` (`touch`, `pc` or `direct`), `settings` (the whole set of changed items; `{}` returns every item to following the game; a request without it changes none), `display`,
`restore`, `cover` (`official` or `custom`, with `image-path` and `hero`; an
empty string clears one, `official` puts the newest official cover back),
`badge` (see [Poster badge](#poster-badge)) and
`options` (Sparxie's own options of the profile, written to its config for a
touch application). Choosing `touch` for an application that has no Sparxie
profile writes one, as `POST apps` does. A running Sparxie is 409
`sparxie_running`; an application that is not a game application is 404
`not_found`. The answer is `{ ok, app, index, wrote, write_back }`.

### Types in `apps.json`

A `sparxie` block is typed JSON: booleans are booleans, frame rates, scales and render scales are numbers. The generic apps writer
(`POST /api/apps`, `DELETE /api/apps/{index}`, `POST /api/apps/batch-delete`) goes through a property tree that writes every scalar of a nested
object as a string (`"fpsUnlock": "false"`, `"fps": "120"`) and an empty array or object as `""`. Since a game application is saved by
`PUT /api/sparxie/apps/{index}`, which is typed, the generic writer is only a danger for a block it passes through unchanged (an edit of
another application, a reorder, a delete). The host therefore:

* types the block again after every generic write of the file (`heal_apps_file`), and
* types it when it reads the file (`GET /api/apps` answers typed text, and every Sparxie route reads typed values), so a file that was
  already written with strings heals on the next read and is rewritten typed by the next save.

The keys that are converted are the boolean and numeric ones of `options` (`fpsUnlock`, `bgLimit`, `followPreset`, `scaleOverride`,
`isDefault`; `fps`, `bgFps`, `preset30`, `preset60`, `scale`) and of `settings` (`vsync`, `hdr`; `fps`, `renderScale`), the block's
`launchTemplateProvisional`, and the `""` that stands for an empty `options`, `settings`, `candidates` or `markers`. Text that only looks
like a number (`artVersion`) is never converted, and `options.fps: "display"` stays the word.

`POST /api/sparxie/import` with `{ "ids": ["genshin-cn-1"] }`.

```json
{ "ok": true, "apps": [], "saved": true }
```

Each id becomes one application that references the existing profile. The host
does not rewrite `config.json`. Region comes from identifying the profile
executable. An ambiguous region is left empty and `candidates` is set on the
block.

`PUT /api/sparxie/profiles/{id}` replaces that profile's display name,
executable and options. The body is `{ "name", "exe", "options" }`. The same
write-back rules apply, including the running and schema refusals. Unknown
JSON fields on the profile and on the document are preserved. Zenless Zone Zero
never gains a `hoyo` object.

## Game settings

`GET /api/sparxie/game-settings?install={id}` (for an install the host does not
store yet, add `&exe={path}&region={region}`)

```json
{
  "ok": true,
  "running": false,
  "values": {
    "resolution": "1920×1080",
    "display": "fullscreen",
    "fps": 60,
    "vsync": false,
    "renderScale": 1.0,
    "preset": "high",
    "hdr": false
  },
  "unavailable": []
}
```

`running` is true when that install's game executable is running.
`unavailable` lists fields the host could read no store for (for example
Zenless Zone Zero quality values when the game's own file does not decode).
`screen` ({ width, height }) is the display the host sees, when it can read it;
`stream` (the same shape, plus `refresh`) is there only while a stream is running: it is the
display the next launch covers.

## Display

Every game application has a display choice in its `sparxie` block (`display`), whatever its
launch path. An application that never chose follows the stream.

- `follow` (跟随串流): at every launch the game runs fullscreen at the size of the display that is
  streamed. The host resolves the size when the application starts (`prepare_launch`): the display it
  streams (already configured for the session) wins, the session's own width and height
  (`SUNSHINE_CLIENT_WIDTH`, `SUNSHINE_CLIENT_HEIGHT`) are the fallback, and a size that is not known
  leaves the game's stored size alone. The window mode is the full-screen window (Unity's
  `FullScreenWindow`, Unreal's windowed fullscreen); an exclusive mode is never chosen.
- `fixed` (固定): the person's size and window mode.

| Title | What the host writes |
| --- | --- |
| Genshin Impact, Honkai Impact 3rd | registry: `Screenmanager Resolution Width/Height`, `Screenmanager Is Fullscreen mode`; Honkai Impact 3rd also the `ScreenSettingData` JSON |
| Honkai: Star Rail | registry: `Screenmanager Resolution Width/Height`, `Screenmanager Fullscreen mode` (1 full-screen window, 3 window), `GraphicsSettings_PCResolution` JSON |
| Zenless Zone Zero | registry: `Screenmanager Fullscreen mode`, `Screenmanager Resolution Width/Height Default`; the size is the position 0 (the screen's own) of the game's resolution list in `GENERAL_DATA.bin` |
| Wuthering Waves, NTE | `GameUserSettings.ini`: `ResolutionSizeX/Y`, `FullscreenMode` |
| Arknights: Endfield | no settings are written: the size is the Sparxie session's (`endfieldAutoResolution` for 跟随串流, `endfieldWidth/Height` for 固定), mirrored in `options.resolution` |

The same size and window go to the game on its command line (Unity's own arguments, as Collapse and
Starward pass them), so the registry and the command line agree: `-screen-width W -screen-height H` and
`-screen-fullscreen 1` (full screen), `-screen-fullscreen 0` (window) or `-screen-fullscreen 0 -popupwindow`
(a frame-less window; the registry says windowed). A game that starts directly gets them appended to its command
(store addresses and official-launcher commands take none, and a command that already has a size is left alone);
a touch application hands them to Sparxie through `sparxie-launch.exe --screen-width W --screen-height H
--window-mode fullscreen|windowed|borderless`, which sends them in the Broker's `ProfileSnapshot.screen` (capability
`screen-args`; an older Sparxie ignores them and the registry write is all there is). Following the stream without
a known size sends nothing. The writes go through the same backup and restore as the game settings.

`POST /api/sparxie/game-settings`

```json
{ "install": "genshin-cn", "app": 3, "values": { "fps": 60, "display": "windowed" } }
```

`install` or `app` (the application index) selects the target. `values`
may contain any subset of the settings object, and only that subset is
written: a field that is not there keeps the game's own value.

An application writes only the items it holds (its changed items, see above) at launch,
after backing the original values up, and with `restore` puts them back when the session ends
(the game quit, or the stream ended and the host ended the application; a game that is still running
is waited for up to two minutes, and anything left over is put back at the next launch or host start). `POST /api/sparxie/game-settings` is the one write that
is not a launch: when the game is running the
host returns 409 `game_running` and writes nothing. Otherwise it writes only
the fields it manages, keeps every other registry and JSON field, and stores a
backup of the previous bytes under the host data directory
`game-settings-backup/`. When `app` is set, the same values are saved into
that application's `sparxie.settings`.

At launch, before the application command starts, the host applies that
application's `sparxie.settings` (its changed items) if the game is not running. If the game is
running, launch continues and the settings are left as they are. A previous
backup that was not put back (the host went down mid-session) is put back first, by the same
rule as at the session end: each value where the game has not changed it since.

Where the values live, all under `HKEY_CURRENT_USER`:

| Game and region | Key |
|---|---|
| Genshin national and bilibili | `Software\miHoYo\原神` |
| Genshin international | `Software\miHoYo\Genshin Impact` |
| Star Rail national and bilibili | `Software\miHoYo\崩坏：星穹铁道` |
| Star Rail international | `Software\Cognosphere\Star Rail` |
| Zenless Zone Zero national and bilibili | `Software\miHoYo\绝区零` |
| Zenless Zone Zero international | `Software\miHoYo\ZenlessZoneZero` |
| Honkai Impact 3rd national | `Software\miHoYo\崩坏3` |
| Honkai Impact 3rd international | `Software\miHoYo\Honkai Impact 3rd` |

National and bilibili share one key, so two applications on those regions
overwrite the same values. Each application writes its own values at launch.

Each title manages only the fields its adapter has a sourced writer for; the
catalog row says which (`settings` in the games answer). Honkai Impact
3rd manages resolution, window mode and the frame rates
(`GENERAL_DATA_V2_ScreenSettingData_h1916288658` with the plain
Screenmanager DWORDs, and `TargetFrameRateForInLevel` /
`TargetFrameRateForOthers` in
`GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519`, 30 or 60). Wuthering
Waves, Punishing: Gray Raven, Arknights, Endfield and NTE have no sourced
writer yet: reading answers every field `unavailable` and writing answers
400 `settings_unsupported` — their settings stay the game's own, which is
what an application that follows the game does anyway.

Genshin resolution and fullscreen are the DWORD values
`Screenmanager Resolution Width_h182942802`,
`Screenmanager Resolution Height_h2627697771` and
`Screenmanager Is Fullscreen mode_h3981298716` (1 fullscreen, 0 window).
Frame rate, render scale, vertical sync, preset and HDR are fields inside the
UTF-8 JSON blob `GENERAL_DATA_h2389025596` (a trailing NUL is preserved).
The host edits `globalPerfData.saveItems` entries and `enableHDR`, and the
DWORD `WINDOWS_HDR_ON_h3132281285`. It does not create a new
`GENERAL_DATA` document when the game has not written one, because the
version strings in that document must match the installed game. Those fields
then come back in `unavailable`.

Star Rail resolution is the JSON blob
`GraphicsSettings_PCResolution_h431323223` (`width`, `height`,
`isFullScreen`) plus the DWORD width, height and
`Screenmanager Fullscreen mode_h3630240806` (1 fullscreen, 3 window).
Quality and frame rate are fields of
`GraphicsSettings_Model_h2986158309` (`FPS`, `EnableVSync`,
`RenderScale`). The preset is the DWORD
`GraphicsSettings_GraphicsQuality_h523255858`. Star Rail has no HDR field;
`hdr` is ignored, and because the game has none it is not listed in
`unavailable` either.

Zenless Zone Zero resolution and fullscreen use the same width and height DWORD
names, the default width and height DWORD names, and fullscreen mode 1 or 3.
Frame rate, vertical sync, render scale and HDR live in
`{game}\ZenlessZoneZero_Data\Persistent\LocalStorage\GENERAL_DATA.bin`.
That file is an obfuscated JSON document. The host implements its container
and transform, and ships the key that decodes it as
`assets/sparxie/zzz-magic.bin` (29 bytes, decoded once from the MIT-licensed
Collapse Launcher metadata repository; one key covers the national, bilibili
and international presets at template version 3, and the transform and key
carry that repository's MIT notice, see NOTICE). A file named
`zzz-magic.bin` in the host's Sparxie data directory replaces the shipped
key.

Inside the document the frame rate is `SystemSettingDataMap` entry `110`
(`0` 30, `1` 60, `2` unlimited), vertical sync entry `8` (0 or 1), render
scale entry `9` (`0` 0.8, `1` 1.0, `2` 1.2), the quality preset entry `3`
(`0` high, `1` medium, `2` low, `3` custom) and HDR the top-level
`HDRSettingRecordState` (0 or 1). An entry the person never changed in-game
is absent and reads as the game's own default (60, vsync on, 1.0, medium).

A write edits only those numbers and keeps every other byte of the document
(key order, padding and formats). The new bytes go to a temporary file that
is read back and verified before it replaces the game's file, the previous
bytes are kept by the `game-settings-backup` flow, and the game must not be
running. A file the key does not decode (a newer game version) is never
written: the five items report read-only through `unavailable`.

## Art

Official images are fetched from the HoYoPlay `hyp-connect` `getGames` and
`getAllGameBasicInfo` endpoints.

| Region | Host | `launcher_id` |
|---|---|---|
| national | `https://hyp-api.mihoyo.com` | `jGHBHlcOq1` |
| international | `https://sg-hyp-api.hoyoverse.com` | `VYTpXlbWo8` |
| bilibili Genshin | `https://hyp-api.mihoyo.com` | `umfgRO5gh5` |
| bilibili Star Rail | `https://hyp-api.mihoyo.com` | `6P5gHMNyK3` |
| bilibili Zenless Zone Zero | `https://hyp-api.mihoyo.com` | `xV0f4r1GT0` |

The query is `/hyp/hyp-connect/api/{getGames|getAllGameBasicInfo}?launcher_id=…&language=zh-cn`
(national and bilibili) or `language=en-us` (international). The host reads
`data.games[]` where `biz` matches, and uses `display.icon.url`,
`display.logo.url` and `display.background.url`. Bilibili
`display.background` is often null; the background then comes from
`data.game_info_list[].backgrounds[0].background.url`.

The version is the content token in that URL (the last path segment without
its extension), not the calendar day. An unchanged token does not download
again.

Cache directory: `{host sparxie data}/posters/{biz}/`. The host stores the
original images and a composed 600×900 PNG. The previous version is kept for
offline use. Nothing in this cache is part of the repository or the installer.

The cover crop keeps the full image height and a width of two thirds of that
height (a 2:3 window). The window slides horizontally. The chosen place is the
one with the largest column-gradient energy. A scrim darkens the bottom of the
window and the official logo is drawn on that scrim. The banner slot is the
top portion of the same background. The icon slot is the official icon.

`GET /api/sparxie/art/{icon|card|banner}/{installId}?v={version}` returns the
image bytes (`image/png` for `card` and `banner`, the cached icon bytes
for `icon`). `v` is the version token so a new poster is a new URL.
`cover: custom` on an application is never replaced. `cover: official`
updates that application's `image-path` to the new composed file in the
host covers directory, which is what a Moonlight client reads.

Refresh runs at host start, again after six hours plus up to 30 minutes of
jitter, and when the application list is opened if the last check was more
than 15 minutes ago. The list request is not delayed for the fetch. Offline,
or a failed fetch, serves the last cache. `art_unavailable` is only the case
where there has never been a cache.

`POST /api/sparxie/art/refresh` with `{ "install": "genshin-cn" }` forces
one check. An empty body checks every known install.

`PUT /api/sparxie/apps/{index}/cover` with `{ "cover": "official" }` or
`"custom"` sets the flag. `custom` stops the sync from changing
`image-path`. `official` points `image-path` at the current composed
cover again.

### Poster badge

A touch application (`mode: touch`) shows the Sparxie mark on its posters, so
that it is told apart on a phone grid. The clients only show the image they
are given, so the mark is drawn into the image by the host. An application
that starts the game directly (`mode: pc`) never has it.

- `sparxie.badge` (boolean) is the setting. Absent, it is `true` for an
  official cover and `false` for a cover of the person's own
  (`cover: custom`). `POST apps` and `PUT apps/{index}` accept it; import from
  Sparxie sets it on. The editor shows it as 「海报显示 Sparxie 标志」 under
  the cover choice, for touch applications only.
- Official posters. Every refresh writes both variants: the plain ones
  (`covers/sparxie-<install>-<version>.png`, `posters/<install>/hero.jpg`) and
  the badged ones (`covers/sparxie-<install>-<version>-badge<rev>.png`,
  `posters/<install>/hero-badge.jpg`, `<rev>` is the drawing revision). An
  application's `image-path` and `hero` point at the variant its setting
  asks for; the files have their own names, so `FsArtVersion` changes when
  the setting is switched and the clients fetch the poster again. A file
  that would come out byte for byte the same is not rewritten. An install
  whose posters were made before the badge gets the badged cover from the
  plain one at host start and the badged hero at the next refresh (from the
  cached pictures; until then the plain hero stays).
- A cover of the person's own is untouched unless `badge` is `true`. Then the
  application points at a badged copy of the local PNG or JPEG file,
  `posters/custom/<hash>-<size>-<time>-r<rev>-badge.<ext>`, and the block
  keeps the original as `customImage` / `customHero` (the editor shows and
  saves the original; a path that is the host's own copy is never badged a
  second time). The person's file is never modified. An address (`http`,
  `https`), a missing file and a format other than PNG or JPEG (the host's
  own decoder reads those two) keep their picture without the badge. A
  changed source file makes a new copy and the old one goes.
- Drawing: a rounded square of smoked glass in the top-right corner, whose
  side is the smaller of 26 percent of the width and 25 percent of the height
  (a 16:9 hero: 14 percent of the width; a 2:3 cover: 26 percent), with the
  mark inside. It stays clear of the centred game logo of the hero and of the
  title scrim the clients draw at the bottom. About 25 px on a 110 px wide
  portrait tile and on a 180 px wide landscape tile.
- The mark is the Sparxie project's own (`sparxie-256.png` from its repository),
  embedded unmodified to identify the component; see
  [sparxie-games.md](sparxie-games.md#the-sparxie-mark).

## The input of an application, the badge and Endfield (2026-11-12)

* **Effective input.** An application's input is `sparxie.options.input` when it is stored, else what its launch was: `pc` (a direct launch of the first builds) is `kbm`, `touch` is `touch`. A save stores it. With a Sparxie that reports `touch-ui-switch`, a Genshin or Star Rail application with the `kbm` input starts through the launcher with the PC interface (`touchUiEnabled: false`); at host start such an application that was made as a direct launch (`mode: pc`, no profile) is moved to the launcher route, keeping its options. Zenless Zone Zero has no PC-interface session: its `kbm` application starts the game directly, with or without the switch. An open Sparxie postpones the move to the next start.
* **Badge.** The poster carries the Sparxie mark only for an application with the `touch` input that starts through Sparxie; `kbm` (also through Sparxie's PC session) and Endfield's `auto` have the plain posters. The person's own `badge` on/off wins for the touch input. The rule is applied at host start and at every save.
* **Migrated covers.** The installer's `migrate-images.ps1` used to copy every poster that is not a bare file name to `covers/app_<hash>_<time>.png`, the official covers of game applications included. It now skips the host's posters of a game application, and at host start an `app_*` file whose bytes equal one of the install's official covers (`covers/sparxie-<install>-*.png`) is the official cover again; a custom cover and a picture of the person's own stay.
* **Catalog.** Every row has `settingsManaged`: true when the host reads and writes some of the title's own settings. Endfield has none (`settings` is empty, `settingsManaged` false): the page says once that its in-game settings are not changed, it does not list items as unavailable. Endfield's Sparxie items are `input` (`auto` by the client's device, `touch`, `kbm`), `resolution` (`auto` follows the stream, or `WxH`), the target frame rate (tiers up to 240 and `display`), and the launch options (background limit, priority). The profile's `game` for Endfield is `endfield`.
* **Endfield session fields.** `sparxie-launch` sends `endfield_input_mode` (12), `endfield_auto_resolution` (13), `endfield_width` (14), `endfield_height` (15) and `endfield_refresh_rate` (16) of the Broker protocol: the stored choice, resolved with the variables Sunshine gives the application (`SUNSHINE_CLIENT_TOUCH`, which the host sets from the client's touch intent, and `SUNSHINE_CLIENT_WIDTH`, `_HEIGHT`, `_FPS`). Without the touch variable, `auto` stays `auto`. A Broker that lists no `endfield` capability gets no Endfield session: the launcher starts the game directly and says so.

## Warning

`POST /api/sparxie/warning` with `{ "acknowledged": true }` records that
the statement was shown. The status object continues to include `statement`
and `statement_en`. There is no call that clears the text.

## Failures and leftovers

* `POST /api/sparxie/apps` and `PUT /api/sparxie/apps/<i>` change two files: Sparxie's `config.json` (a profile) and `apps.json`. A request that fails after the profile was written takes that profile out again (backup first), so a failed save leaves no profile behind.
* Every failed Sparxie route is logged at warning level: `Sparxie API <METHOD> <path> failed: <error_code>: <error>`.
* At host start the host removes profiles it made earlier that no application uses: ids of the shape `<game>-<region>-<n>` that no `sparxie.profile` and no command line (`--profile <id>`) names. Profiles with any other id are never touched. Nothing is removed while Sparxie is open or when the application list cannot be read; `config.json` is backed up before it is changed.
