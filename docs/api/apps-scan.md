# Application library scan API

`POST /api/apps/scan` discovers games that are already installed on the host
where the configuration server runs. It is a read-only call: the endpoint
returns candidates and never changes the application list. A client adds the
selected entries through the existing application save API.

The endpoint is registered by `src/confighttp/apps_scan.cpp`; discovery lives in
`src/app_scan/scanner.cpp` so the same host implementation serves every client.

## Authentication

The route uses the same policy as the other configuration writes:

* `authenticate()` applies the browser origin, referer, `Sec-Fetch-Site` and
  host checks (`src/confighttp/auth.cpp`), then the remote-access/password
  policy.
* The body must be `Content-Type: application/json`.
* The call is not restricted to loopback: an authorized remote client scans the
  host that serves the Web UI, which is the intended behaviour.
* One scan runs at a time. A second concurrent request receives `409 Conflict`.

## Request

```json
{
  "sources": ["steam", "epic", "gog"],
  "directory": "D:\\Games"
}
```

* `sources` is optional. When omitted, every library source is scanned.
  Values are `steam`, `epic`, `gog`, `xbox`, `ea`, `ubisoft`, `battlenet`,
  `riot`, `hoyoplay`, `wegame` and `sparxie`; duplicates are ignored and any
  other value is rejected with `400`.
* `directory` is optional. When present and non-empty, the bounded directory
  walk also runs. It can be used together with `sources`.
* At least one source or a directory must be selected.

## Response

```json
{
  "games": [
    {
      "platform": "steam",
      "name": "Portal 2",
      "id": "620",
      "install_dir": "D:\\SteamLibrary\\steamapps\\common\\Portal 2",
      "launch": "steam://rungameid/620",
      "exe": "D:\\SteamLibrary\\steamapps\\common\\Portal 2\\portal2.exe",
      "cover_hint": "https://cdn.akamai.steamstatic.com/steam/apps/620/header.jpg"
    }
  ],
  "warnings": []
}
```

`exe` and `cover_hint` are omitted when the scanner has no value. `source`
repeats the preferred `platform`. `sources` lists every library that had this
install. `region`, `profile` and `launches` are present when the scanner has
them. `warnings` are stable codes (`steam.not_found`, `sparxie.absent`,
`directory.depth`, and the same shape for the other libraries). A warning never
turns a successful request into an error. `sparxie.absent` means the touch
component is not installed; it is not a failure.

## Discovery rules

The rules follow the existing desktop scanner, so both clients report the same
games for the same host.

### Steam

1. Windows: the `InstallPath` value of
   `HKLM\SOFTWARE\WOW6432Node\Valve\Steam`, then
   `C:\Program Files (x86)\Steam`.
2. Linux: `~/.steam/steam`, `~/.local/share/Steam`, then the Flatpak data path
   `~/.var/app/com.valvesoftware.Steam/.local/share/Steam`.
3. macOS: `~/Library/Application Support/Steam`.
4. `steamapps/libraryfolders.vdf` (falling back to
   `config/libraryfolders.vdf`) contributes every existing `"path"` entry.
5. Every `appmanifest_*.acf` in each library's `steamapps` directory becomes a
   candidate. Manifests are skipped when the app id is below 10, the app type is
   `tool`, `demo`, `music`, `dlc`, `config` or `media`, or the name matches the
   scanner's tool, runtime, soundtrack or demo keyword list.
6. `launch` is `steam://rungameid/<id>`. The main executable is the largest
   root-level `.exe` that is not a setup, update, crash-report or helper tool;
   it is reported as `exe` without changing the launch command.
7. `cover_hint` is the Steam CDN header image for the app id.

### Epic Games

Windows only. Every `*.item` file under
`%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests` is parsed. `DisplayName`,
`AppName`, `InstallLocation` and `LaunchExecutable` are required; a manifest
without one of them is ignored. `launch` is
`com.epicgames.launcher://apps/<AppName>?action=launch&silent=true`.

### GOG

Windows only. The subkeys of `HKLM\SOFTWARE\WOW6432Node\GOG.com\Games` are
read. `gameName` and `path` are required. `exe` comes from the key's `exe` value,
or from the largest non-tool root executable when that value is empty. With an
executable the launch command is the quoted executable path; otherwise it is
`goggalaxy://openGameView/<id>`.

### Directory walk

The directory scope uses the same game heuristic as the desktop scanner.
Paths are normalised to backslash separators before matching, so the same rules
work for native Windows paths and for Wine-style prefixes on other hosts.
Only entries that the heuristic classifies as games are returned, with
`platform` set to `directory` and `id` set to the source file path.

`.exe`, `.lnk`, `.bat`, `.cmd` and `.url` files are inspected. A `.lnk` is
resolved through `IShellLinkW` on Windows and ignored when it does not resolve;
its target participates in the game heuristic. Scanned code is never executed.

## Security and resource bounds

* The walk reads the requested root only and never writes.
* UNC paths (`\\server\share`, `//server/share`) and device namespaces
  (`\\.\`, `\\?\`, `\Device\`, `\??\`, and the Linux `/dev`, `/proc` and
  `/sys` trees) are rejected with `400` before any I/O.
* Symbolic links are not followed. A directory that resolves outside the
  canonical scan root is skipped, so a link cannot widen the read scope.
* The walk stops at a depth of 24 and after 5 seconds; reaching either bound is
  reported as a warning. A single request therefore cannot hold the server
  thread or the disk indefinitely.
* Only one scan runs at a time.
* Logs contain the source count, result counts and elapsed time. When the
  requested directory is logged, the user's home prefix is replaced with `~`;
  no user name and no per-game path is logged.

## Errors

| Status | Meaning |
| --- | --- |
| `400` | Malformed JSON, unknown source, no source selected, or a rejected directory path |
| `401` / `403` | The normal Web UI authentication and origin policy |
| `409` | Another scan is already running |

## Sources

The platform facts above rely on the records in
`docs/authority/source-index.tsv`:

* `ms-win-reggetvalue`, `ms-win-alternate-registry-view` and
  `ms-win-registry-redirector` for registry reads and the 32-bit view.
* `ms-win-naming-files` for the UNC and device namespace rules.
* `ms-win-shell-links` and `ms-win-ishelllink-getpath` for `.lnk` resolution.
* `freedesktop-xdg-basedir` and `flatpak-sandbox-permissions` for the Linux data
  paths.
* `apple-filesystem-overview` for the macOS `Library/Application Support`
  placement.
* `cpp-fs-weakly-canonical` and `cpp-fs-symlink-status` for the containment and
  link checks.
* `nlohmann-json-parse` for the Epic manifest reader.

The Steam `libraryfolders.vdf` and `appmanifest` layout, the Epic `.item`
fields and the Steam, Epic and GOG launch URIs are not covered by a published
vendor schema. Their behaviour is taken from the existing desktop scanner, which
is the reference for this port. No vendor text is copied into the repository.

## Additional libraries

These sources are Windows-only. On another host each one reports
`<source>.unsupported`.

* Xbox reads `.GamingRoot` on each drive, then `MicrosoftGame.config`
  (`ShellVisuals`, `Executable`, `Identity`). Launch is
  `shell:AppsFolder\<PackageFamilyName>!<AppId>` when the package family is
  known from the signed-in user's packages or from a package full name, and the
  game executable otherwise. A local shell logo is used when the file exists.
* EA app reads `user.downloadinplacedir` under
  `%LOCALAPPDATA%\Electronic Arts\EA Desktop` and the EA Games / Origin Games
  registry keys, then `__Installer\installerdata.xml`. Launch is
  `link2ea://launchgame/<contentID>`, the handler shipped with the EA app.
* Ubisoft Connect reads
  `HKLM\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\<id>\InstallDir`
  and launches `uplay://launch/<id>/0`. A cached image under the user's
  Ubisoft assets folder is used when one is there.
* Battle.net reads embedded product codes and install paths from
  `%ProgramData%\Battle.net\Agent\.product.db` (and `Battle.net.config` when
  it carries `InstallPath`). Launch is `battlenet://<code>`.
* Riot Client reads `%ProgramData%\Riot Games\RiotClientInstalls.json` and
  `%ProgramData%\Riot Games\Metadata\<product>.<patchline>`. Launch is
  `RiotClientServices.exe --launch-product=<id> --launch-patchline=<line>`.
* HoYoPlay reads the same per-user `HYP` `GameInstallPath` values the touch
  component uses, and launches the game executable directly. The region is a
  tag. Official art is the cover already cached for that install; the scan
  does not call the network.
* WeGame reads `HKLM\SOFTWARE\WOW6432Node\Tencent\WeGame\InstallPath` and
  `rail_apps.json`. Launch is `wegame://launch_game/<id>`. Whether the client
  must already be running is not verified here; the protocol handler is the
  launch command.
* Sparxie is reported only as `sparxie.absent` when its executable is not
  installed. When it is installed, each profile in its `config.json` is one
  result with launch choices `touch` and `pc`. Adding a profile calls
  `POST /api/sparxie/import` or `POST /api/sparxie/apps`, the same routes as
  creating a Sparxie application.

Results that share an install directory or an executable are one entry. The
preferred launch is the store command (Steam before the other stores, and a
store before a direct executable).

Per-user registry and known folders are read for the signed-in person. The
service token's own `HKCU` and environment are not treated as that person's
library.
