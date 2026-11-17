# 游戏适配表 / Game adapter table

One game-app model, one table per title. This file is the reference for what
the host knows about every launcher-managed game: how an install is detected,
which regions exist, how each build starts, which settings our preset can
write and where the official art comes from. The model itself (identity,
de-duplication, launch modes, settings modes) is [docs/sparxie-api.md](sparxie-api.md).

Facts come from the official launchers' own records and public answers, and
from the reference launchers' observed behaviour. Code was read for behaviour
only; nothing was copied, and per the project's rule for these non-core
features no source-index rows are added — each fact names its source here.
Everything was read on 2026-11-11.

## Summary

| Title | id | Regions | Executable | Detection | Direct launch | Preset settings | Official art |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 原神 Genshin Impact | genshin | cn, bili, global | YuanShen.exe / GenshinImpact.exe | HoYoPlay registry, config.ini, Epic | exe (store build via its store) | all fields | HoYoPlay API |
| 崩坏：星穹铁道 Honkai: Star Rail | starrail | cn, bili, global | StarRail.exe | HoYoPlay registry, config.ini, Epic | exe (store build via its store) | all but hdr | HoYoPlay API |
| 绝区零 Zenless Zone Zero | zzz | cn, bili, global | ZenlessZoneZero.exe | HoYoPlay registry, config.ini, Steam | Steam link or exe | all fields | HoYoPlay API |
| 崩坏3 Honkai Impact 3rd | hi3 | cn, global | BH3.exe | HoYoPlay registry, config.ini game_biz, Steam | Steam link or exe | resolution, display, fps | HoYoPlay API |
| 鸣潮 Wuthering Waves | wuwa | cn, bili, global | Client\Binaries\Win64\Client-Win64-Shipping.exe | launcherDownloadConfig.json, SDK tree, Steam, Epic | Steam/Epic link or exe | none (unsourced) | Kuro launcher CDN (cn, bili, global), Steam store second (global) |
| 战双帕弥什 Punishing: Gray Raven | pgr | cn, global | PGR.exe | executable only today | Steam link or exe | none (unsourced) | Kuro launcher CDN (cn, global), Steam store second (global) |
| 明日方舟 Arknights (PC) | arknights | cn, global (+bili exists, see risks) | Arknights.exe | launcher uninstall record, games folder, game-launcher-config.json (Yostar), folder name | official launcher or exe (parent check) | none (unsourced) | launcher API (cn, bili), placeholder (global) |
| 明日方舟：终末地 Arknights: Endfield | endfield | cn, bili, global | Endfield.exe | launcher uninstall record, games folder, sdkdata channel config, Steam, Epic | Steam/Epic link or official launcher | none (unsourced) | launcher API (cn, bili) then Steam store, Steam store + briefing page (global) |
| 异环 NTE: Neverness to Everness | nte | cn, bili, global | NTEGame.exe / NTEGlobalGame.exe (HTGame.exe engine build) | NTELauncher Config.ini branch, NTEGlobal folder, Steam | Steam link, launcher or exe | none (unsourced) | NTE launcher file list (cn, bili), Steam store (global) |

Sparxie touch (触控) stays available for Genshin Impact, Honkai: Star Rail and
Zenless Zone Zero only — Sparxie's own title list — and only while the
component is installed.

## Per-title facts and sources

### 崩坏3 Honkai Impact 3rd (hi3)

- The PC launcher became HoYoPlay on 2024-10-17 (official notice:
  honkaiimpact3.hoyoverse.com news 126443). The launcher API answered today
  with biz bh3_cn on the CN launcher (jGHBHlcOq1) and bh3_global on the
  global one (VYTpXlbWo8), so the same registry layout as the other HoYo
  games finds it: Software\miHoYo\HYP\1_1\bh3_cn and
  Software\Cognosphere\HYP\1_0\bh3_global, value GameInstallPath.
- Executable BH3.exe, data folder BH3_Data, launcher game folder "Games"
  (Collapse stable metadata config_Hi3CN.json / config_Hi3Global.json,
  cdn.collapselauncher.com/cl-cdn/metadata/v3/stable/, read 2026-11-11).
- Both clients write channel 1, sub_channel 1 (same metadata files), so the
  channel pair never narrows this game's region; config.ini game_biz
  (bh3_cn / bh3_global) does.
- The global client is on Steam, app 1671200 "Honkai Impact 3rd", free,
  released 2021-10-21 (Valve appdetails answer; the same id is in Collapse's
  config_Hi3Global.json as SteamGameID).
- Settings registry root Software\miHoYo\崩坏3 (CN) and Software\miHoYo\Honkai
  Impact 3rd (global) (Collapse metadata ConfigRegistryLocation). The preset
  writer uses the screen values GENERAL_DATA_V2_ScreenSettingData_h1916288658
  ({width, height, isfullScreen}, plain JSON bytes) and the plain Screenmanager
  dwords, plus the frame rates TargetFrameRateForInLevel / TargetFrameRateForOthers
  in GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519 (Collapse source
  commit d478585, ScreenSettingData.cs and PersonalGraphicsSettingV2.cs).
  vsync, render scale, preset and hdr have no sourced writer for this title
  and answer unavailable.
- The SEA, TW, KR and JP PC clients (Collapse keeps configs for all six
  regions) are not separate regions here: an executable outside a known
  record stays an open install and the person chooses.

### 鸣潮 Wuthering Waves (wuwa)

- Executable Client\Binaries\Win64\Client-Win64-Shipping.exe; the game root
  is the folder that holds Client\ (WutheringWavesTool by leck995, commit
  d08a16a, GameServerSwitchService.java).
- The official launcher's own record is launcherDownloadConfig.json in the
  game root; its appId names the channel: 10003 mainland (cn), 10004
  bilibili, 50004 global (same source). Without it, the SDK folder under
  Client\Binaries\Win64\ThirdParty names the region: KrPcSdk_Global is the
  global build, KrPcSdk_Mainland the national one, and KrPcSdk_Mainland\
  KRSDKRes\Bilibili marks the bilibili channel.
- The global build is on Steam: app 3513350 "Wuthering Waves", free,
  2025-04-28 (Valve appdetails), and on Epic: the store launch link uses
  AppName e885327ce4414509bff4c10757f88334
  (com.epicgames.launcher://apps/…?action=launch&silent=true), read from the
  Collapse WuWa plugin (commit 828ca2b, Exports.GameLaunch.Epic.cs); the
  Epic manifest itself is matched by the executable inside InstallLocation.
- The official launcher starts the game through a "Wuthering Waves.exe"
  wrapper that exits right after spawning the game executable (same plugin,
  Exports.GameLaunch.cs); launching the game executable directly is what the
  community launchers do. Whether the anti-cheat (AntiCheatExpert under
  Client\Binaries\Win64) treats a direct start differently on a real machine
  is unverified; a store install starts through its store link anyway.
- Settings: the community tools patch frame rate in memory; that is a
  modification the game does not expose, so our preset writes nothing for
  this title (unsourced writer). 自动 keeps the game's own settings.

### 战双帕弥什 Punishing: Gray Raven (pgr)

- The global PC client's executable is PGR.exe (the official FAQ,
  pgr.kurogame.net/news/398, read 2026-11-11 via the store's search answer).
- The global build is on Steam: app 4125930 "Punishing: Gray Raven", KURO
  GAMES, free, 2026-02-02 (Valve appdetails). The CN PC client has shipped
  since 2023-06 through its own launcher (official news on pns.kurogames.com
  and kurobbs.com); no registry value or launcher file that names the CN
  install was sourced, so a CN install is found by its executable (or the
  folder search) and its region stays open until the person picks one.
- Settings: no sourced writer; unwired. Art: the Kuro launcher CDN (cn, global),
  the store page second for global (see the art section).

### 明日方舟 Arknights PC (arknights)

- The CN PC client launched 2026-02-10 through the 鹰角启动器
  (ak.hypergryph.com/news/0717 and /news/7388); executable Arknights.exe,
  default layout Games\Arknights\… (community detection table in
  HuaXiaoLing-Launcher, commit 330732a, electron/main.js).
- The TW server opened 2026-09-14 through GRYPHLINK (official TW channels);
  the Yostar EN/JP/KR clients are separate installs in Arknights_EN /
  Arknights_JP / Arknights_KR folders, each with a
  game-launcher-config.json whose tag names the region (HuaXiaoLing
  yostar-packs.js; the Yostar launcher APIs are
  api-launcher-en/jp/kr.yo-star.com). Those installs are region global here;
  a TW install is not separated and stays open.
- 官服 and B服 share one executable; the bilibili build differs by an SDK
  file set the community switcher swaps (lTinchl/ArknightsLauncher, commit
  424e075, load/ArkOfficial vs load/ArkBilibili). No file that marks a
  bilibili install was sourced, so a CN install without a Yostar record is
  an open cn/global choice and the person can pick bili when they know.
- The game checks its direct parent process: a Hypergryph-signed executable
  must be the parent, and starting Arknights.exe by hand answers 10100
  「请通过官方启动器打开游戏」 (HuaXiaoLing endfield-launch.js note, which
  also records that Arknights.exe itself is a signed member of the same
  platform). Our direct launch therefore starts the official launcher
  (Launcher.exe under a Hypergryph Launcher install root) when it is
  present, and falls back to the executable with that stated risk.
- Settings: unsourced; unwired.
- Detection beyond the executable: the three official launchers write machine
  uninstall records whose key names identify the channel —
  33a0a6296a20400d503c59ac0fd6341e 鹰角启动器 (cn),
  d9f71f86325c5b9733268efda40413c8 GRYPHLINK (global) and
  d957df99efbdf610162fd47c00ceaba7 the bilibili edition of Endfield; each
  key's InstallLocation names the launcher folder, read from both HKLM
  registry views and HKCU (MaaEnd agent go-service, commit bd1cc26c,
  launchgamepc.go lines 54-58 and regedit_windows.go lines 721-745, read
  2026-11-11). The launcher keeps every game one level under its games
  folder: games\Endfield Game\Endfield.exe (cn launcher, MaaEnd issue
  5729 log), games\Arknights Endfield\Endfield.exe (GRYPHLINK, MaaEnd
  issue 6510 log), games\Arknights\Arknights.exe. The library scan reads
  these records and then the games folders; each install's own channel
  record (Endfield sdkdata, the Yostar tag) still decides the region, and a
  store install of the same folder keeps its store row.

### 明日方舟：终末地 Arknights: Endfield (endfield)

- Released 2026-01-22 on PC (endfield.hypergryph.com/news/7231), also on
  Epic (store FAQ page) and on Steam since 2026-10-14: app 4732690
  "Arknights: Endfield", Hypergryph, free (Valve appdetails).
- Executable Endfield.exe with config.ini in the install root; the default
  launcher layout is Hypergryph\Arknights Endfield\ or Games\Arknights
  Endfield\ (HuaXiaoLing main.js; endfield-nolauncher README names
  D:\Hypergryph\Games\Endfield\Endfield.exe).
- A real install, read-only on a Windows test VM on 2026-11-11: the 鹰角启动器
  (cn) sits in C:\Games\hypergryph\Hypergryph Launcher (a custom folder, not
  a default one), its uninstall record is
  HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\
  33a0a6296a20400d503c59ac0fd6341e with InstallLocation naming that folder,
  and the game is games\Arknights Endfield\Endfield.exe with sdkdata holding
  sdk_hgsdk_config.bin (the cn channel). The folder search alone never found
  it; the uninstall record does.
- The sdkdata folder's channel config names the channel: sdk_hgsdk_config*
  is the CN build, sdk_glsdk_config* the global one, and PCGameSDK.dll /
  BLPlatform64*.dll mark the bilibili channel (HuaXiaoLing main.js
  endfieldChannelOf, reverse-engineered from the official launcher; the
  order matters and is kept).
- The game checks its parent process through OneChannelLC.dll: the parent
  image must be the launcher's Games.exe (signed), and a direct start
  answers 10100 (endfield-direct by Kelcoin, commit 2e45fb7;
  endfield-nolauncher, commit 664647e, verifies the launcher's Authenticode
  signature before acting as parent). Spoofing a signed parent is a launch
  check this host deliberately does not bypass: direct launch starts the
  official Launcher.exe when it is found (its install roots: the
  hypergryphlauncher* URL protocol, the 鹰角启动器 / Hypergryph Launcher
  uninstall entries, and the Program Files / LocalAppData default folders
  with versioned sub-folders — endfield-nolauncher src/launch.cpp
  InstalledRoots), and a store build starts through its store link.
- Official art: the clean key art and the wordmark come from the Steam
  library assets of the title's app, the launcher's public content API is the
  second choice; the icon is the program's own (see the art section). The
  version briefing page's `brand` asset is a watermark, not the wordmark, and
  its favicon is the Hypergryph company icon, not the game's.
- Settings: unsourced; unwired.

### 异环 NTE: Neverness to Everness (nte)

- PC released 2026-04-29 (nte.perfectworld.com; the Co-Ex test ran 2026-02),
  also on Steam since 2026-07-08: app 4508340 "NTE: Neverness to Everness",
  free (Valve appdetails), and on Google Play PC (play.google.com/pc-store).
- Game executables NTEGame.exe (CN) and NTEGlobalGame.exe (global) in the
  install root, engine build Client\WindowsNoEditor\HT\Binaries\Win64\
  HTGame.exe five levels down; the launchers are NTELauncher.exe and
  NTEGlobalLauncher.exe (NTE-Maid by leck995, commit 44a0158,
  GameServerService.java; HuaXiaoLing main.js paths).
- The launcher's channel config NTELauncher\Config\Config.ini
  [UPDATE_CONFIG] Branch names the channel: publish_ob is the official CN
  channel, publish_ob_B the bilibili one (serialNumber 2, Partner 123 in the
  same file); the global launcher keeps its own NTEGlobal folder with the
  same file (HuaXiaoLing neverness-channels.js, reverse-engineered 2026-10).
- Registry records (HKCU\Software\Perfect World\NTE,
  HKLM\SOFTWARE\PerfectWorld\异环 — HuaXiaoLing main.js) exist as detection
  hints in the community table; their value names were not sourced, so this
  host detects by the launcher config files instead.
- Settings: the engine Saved folders are Saved (CN) and Saved_Global
  (global) (NTE-Maid); no settings writer is sourced. Art: the national launcher's file list and
  the global store page (see the art section).

### HoYoverse titles that are not PC yet

The launcher API also lists hyg_cn 星布谷地 (Petit Planet) and abc_cn
崩坏：因缘精灵, both with display_status RESERVATION_ENABLED and reservation
links (planet.mihoyo.com, yyjl.mihoyo.com) — no PC client exists to detect.
They stay out of the catalog until that changes; the reservation answers were
read from the same launcher API on 2026-11-11.

### Other Hypergryph PC titles

泡姆泡姆 PopUCom (Steam app 2543180, paid co-op game) and 明日方舟：泰拉之影
(Steam app 4176500, TriBeluga Studio, 2026-05-14, free) are current PC titles
of the group (Valve appdetails, read 2026-11-11). Neither is launcher-managed
in the reviewed list and neither's executable was sourced, so both stay
documented here without an adapter row.

## Official art of the non-HoYoverse titles

Every title gets cover (portrait), hero (16:9, `hero.jpg`) and icon through
the one publish path the HoYoverse titles use: the cover is composed from the
key art and the official logo (or is the store's finished capsule), the hero
is the 16:9 window of the key art with the official logo large and centred
above the clients' title scrim, the banner and the icon are written next to
them, one content-hash cache per install id, and index.json short-circuits an
unchanged version (`artRev` and `iconRev` in it name the revision of the
sources and of the icon choice that made the files: a poster made by an older
revision is made again). A touch application's badged variants (`hero-badge.jpg`
and the badged cover) are made from the same pictures. A source that has a
bilibili or national twin serves it too (the bilibili build of a title uses its
national picture, as for the HoYoverse posters).

Each title has an ordered list of sources (src/sparxie/art_sources.cpp); the
refresh takes the first whose picture downloads and decodes.

| Title and region | Art sources, best first | No source |
| --- | --- | --- |
| Endfield cn, bili | Steam clean variant (library hero + Chinese library logo + Chinese capsule), 鹰角启动器 content API, Steam store | placeholder |
| Endfield global | Steam store page art + library logo, capsule as the cover | placeholder |
| Arknights cn, bili | 鹰角启动器 content API (the picture has the title painted in; no logo over it) | placeholder |
| Arknights global (Yostar) | none sourced | placeholder |
| Wuthering Waves cn, bili, global | Kuro launcher CDN first frame + Steam library logo (the launcher's slogan if Steam does not answer), Steam store (global) | placeholder |
| Punishing: Gray Raven cn, global | Kuro launcher CDN first frame + Steam library logo, Steam store (global) | placeholder |
| NTE cn, bili | NTE launcher file list + the logo of the game's site | placeholder |
| NTE global | Steam store page art + library logo, capsule as the cover | placeholder |

**The logo rule.** A title's hero and cover carry its official logo, like the
HoYoverse ones: the Steam library logo (a transparent PNG per language; cn and
bilibili take the Simplified Chinese one, then English, global takes English).
A launcher picture that already has the title painted into it (Arknights cn,
Endfield cn when Steam does not answer) is used as it is, without a second
logo; Endfield cn and bilibili therefore prefer Steam's clean library hero. The
Kuro launcher's `slogan` is the current version banner, not the game's name, so
it is only the last resort. The Endfield version-briefing `brand` asset (the
ENDFIELD INDUSTRIES watermark) is never used.

**One logo style on every poster** (src/sparxie/art.cpp, the same routine
`lay_logo` for the landscape hero and the portrait cover; HoYoverse and the
other titles alike). The logo is the visible mark of the best logo the host has
(the large `top_left_logo` of the HoYoverse launcher, the transparent bilibili
one for a national or bilibili Zenless Zone Zero, the Steam library logo, the
site's wordmark): an opaque plate is cut out (`logo_without_plate`), a backdrop
too close in tone to the logo is dimmed (lifted for a dark logo) softly behind
it, a soft shadow follows its shape. Placement: the hero's mark is at most 36
percent of the width and 34 percent of the height, centred, its middle at 42
percent of the height (above the clients' title scrim); the portrait cover's
mark is at most 66 percent of the width and 17 percent of the height, centred,
its middle at 76 percent of the height (the lower third, above the bottom title
area). The size follows the visible mark, not its canvas, so a logo with a wide
transparent margin is as large as one without. A cover that is the store's
finished capsule, or a placeholder card, is used as it is. Composed covers
carry `coverKind` and `coverRev` in index.json and are made again from the
cached pictures (no fetch) when the composition changes.

**Arknights** (national and bilibili): the title has no store page or public
logo file, but the national site draws its wordmark (明日方舟 / ARKNIGHTS) from an
inline SVG definition in its home page (`<path id="svg_def-title_arknights">`,
viewBox 0 0 254 119). The host reads that path from https://ak.hypergryph.com/,
rasterises it (src/sparxie/svg_path.cpp: M L H V C S Q A Z, even-odd fill) to a
white-on-transparent PNG and keeps it like any logo; its hash is part of the art
version. The launcher picture has the painted title where the landscape hero is
cut, so the wordmark is laid on the portrait cover only (`logoCoverOnly`).

**Steam's app record.** The library logo, the clean library hero, the finished
language capsule and the program icon (`clienticon`) are not in the store
browse answer; they are in the app record the Steam client reads (PICS). It is
served as JSON by the community mirror https://api.steamcmd.net/v1/info/<appid>
(`data.<appid>.common.clienticon` and `library_assets_full`). Asset paths are
`<40 hex>/<file>` on shared.akamai.steamstatic.com/store_item_assets/steam/apps/
<appid>/ and the icon is
shared.akamai.steamstatic.com/community_assets/images/apps/<appid>/<clienticon>.ico.
The host accepts only paths of that shape. The mirror is third-party: when it
does not answer, the launcher sources and the monogram icon take over, and a
good icon made earlier stays.

### The program icon

The icon is the game's own, not a crop of a poster. Per title and region, best
first (src/sparxie/icon_file.cpp, art_sources.cpp, host_refresh.cpp):

1. The program icon of the install on this machine: the largest image of the
   executable's RT_GROUP_ICON resource, read from the PE file itself (headers
   and the resource section only, so a multi-hundred-MB executable costs a few
   KB of IO; a PNG-compressed 256 px entry when the program has one, else the
   largest bitmap; every offset is bounds-checked). The detected executable is
   tried first, then the title's other known executables in the same folder.
2. Steam's `clienticon` .ico of the title's app (any region: the national
   builds have no store page, the program is the same): its 256 px PNG entry.
3. The publisher's own icon: for Arknights (not on Steam) the Yostar site's
   PNG icon. The Hypergryph company favicon that the national sites carry is
   not the game's icon and is not used.
4. The program icon of another region of the same title that is installed on
   this machine.
5. A monogram: the title's brand colour and initial (256 x 256), tried again
   on every refresh until a real icon answers.

A picture under 64 px on its shorter side (Steam's 32 px community icon) is
not an icon. Icons are stored as 256 x 256 PNGs (aspect kept, transparent
padding, transparent where the source is) in posters/<install>/icon.png;
index.json records `iconSource` (exe, steam-clienticon, official-site,
exe-sibling, monogram) and `iconRev`.

- **鹰角启动器 content API** (Endfield cn/bili, Arknights cn/bili):
  POST https://launcher.hypergryph.com/api/proxy/web/batch_proxy with one
  proxy_req of kind get_main_bg_image (appcode, language zh-cn, channel 1,
  sub_channel 1, platform Windows, source launcher). Appcodes:
  6LL0KJuqHBVz33WK Endfield, GzD1CpaWgmSq1wew Arknights. The answer's
  main_bg_image.url is the launcher's current background (a webp for Endfield,
  a png for Arknights) with a content-hash name, which is the cache version.
  The launcher has no public icon or wordmark call. The launcher picture has
  the game's title painted in, so no logo is laid over it. (The version-briefing
  page's favicon is the Hypergryph company icon, and its `brand` asset the faint
  ENDFIELD INDUSTRIES watermark: neither is used.)
- **Kuro launcher CDN** (Wuthering Waves, Punishing: Gray Raven). Public
  static JSON the Kuro PC launcher reads, no login:
  `<host>/launcher/launcher/<id>_<key>/<app>/index.json` (gzip) names
  `functionCode.background`; then
  `<host>/launcher/<id>_<key>/<app>/background/<code>/<language>.json` holds
  `firstFrameImage` (the launcher background, a 3840x2280 webp) and `slogan`
  (the transparent wordmark png, per language). The cache version is the two
  file names. Hosts: prod-cn-alicdn-gamestarter.kurogame.com (national) and
  prod-alicdn-gamestarter.kurogame.com (global). The launcher ids and keys are
  the path segments baked into the official launcher, published by community
  tools (WaveTools, WwTool docs/API/Launcher.md, Haiyu, pgr-assets):
  wuwa cn 10003_Y8xXrXk65DqFHEDgApn3cpK5lfczpFx5 / G152 / zh-Hans,
  wuwa global 50004_obOHXFrFanqsaIEOmuKroCcbZkQRBC7c / G153 / en,
  pgr cn 10011_qYQv6TyyyhCKD3ox3gssyolNPwMoCPZt / G148 / zh-Hans,
  pgr global 50015_LWdk9D2Ep9mpJmqBZZkcPBU2YNraEWBQ / G143 / en. They are not
  documented by Kuro: if Kuro rotates one, that source answers nothing and the
  next one (the store build) or the placeholder takes over. The bilibili build
  of Wuthering Waves shares the national launcher art.
- **Steam store** (Endfield 4732690, Wuthering Waves 3513350, Punishing: Gray
  Raven 4125930, NTE 4508340; all verified as the titles' own store pages):
  GET https://api.steampowered.com/IStoreBrowseService/GetItems/v1 with
  `include_assets` answers `raw_page_background` (1438x810 key art),
  `library_capsule_2x` (the finished 600x900 portrait cover, used as the
  cover as it is) and `community_icon` (a 32x32 jpg, too small to be an icon,
  so it is not used). The assets are served from
  shared.akamai.steamstatic.com/store_item_assets/steam/apps/<id>/<hash>/ and
  the content hashes are the cache version. The library logo laid over the
  hero comes from the app record (above).
- **NTE launcher file list** (NTE cn, bili): Version.ini
  (yhcdn1.wmupd.com/hd/publish_ob/launcher/Version.ini) names FileListURL
  (AllFiles.xml); the xml names the base URL, the build and the launcher's
  picture /ResFilesM/1289/bgimgs/bg_0.png (1920x1080 key art, served as
  `<path>.zip`, extracted by the host's own zip reader with its name, size and
  CRC checks); the entry's Checksum is the cache version. The wordmark is the
  `logo.png` of https://yh.wanmei.com/ (its favicon.ico is 32 px, too small for
  an icon; the icon is Steam's, as for the global build). Path 1289
  is the launcher's resource id; if it changes the source answers nothing.
- **Arknights global** (the Yostar builds EN/JP/KR): the official site's
  og:image URLs answer 404, Steam has no listing, the Yostar launcher APIs
  were not found public: no sourced art, the placeholder.
- **Placeholder**: the title's brand colour with its Latin name in a 5x7 block
  face, as the portrait card (600x900), the 16:9 hero (1920x1080, the name
  inside the band clear of the clients' title scrim) and a 256x256 monogram icon, through
  the same publish path; never the generic box art. A placeholder never
  replaces a real picture published earlier (sources down while offline), and
  is replaced as soon as a source answers. A title with a source that did not
  answer is listed in the refresh answer's `skipped`; `sources` names the
  source each install was published from.
- **Decoding**: pictures are decoded by the host's image decoder (WIC on
  Windows). The Hypergryph Endfield and both Kuro pictures are WebP, which
  needs the Windows WebP codec (inbox on Windows 11; the Store's WebP Image
  Extension on Windows 10). A machine without it fails to decode, and the
  refresh moves on to the next source or the placeholder instead of
  publishing a broken file.
- **Bounds**: every request is HTTPS only, has resolve/connect/send/receive
  timeouts, a 200-only answer, gzip decoded, and at most 32 MiB; a picture over
  24 MiB, a zip that is not one sound file, a failed decode and an answer of
  the wrong shape all count as "this source did not answer". No credentials
  are sent or stored.
- Terms: every image remains the property of its publisher (Hypergryph /
  Gryphline, Kuro Games, Perfect World, and Valve's store assets under the
  Steam terms); the host fetches them from the publishers' own public
  endpoints for the person's own library view, caches them by content hash
  and never redistributes or rewrites them.

## Cross-title decisions

- Region tokens stay {cn, bili, global} for every game; a title lists only
  the regions it has. Games the launcher records do not identify keep their
  candidate list open and the person chooses (the one game-app model's
  open-region flow).
- Detection order per install: executable name, HoYoPlay registry, Steam or
  Epic record, vendor launcher record, PCGameSDK.dll (HoYo bili), config.ini
  (game_biz, channel, cps). Store records win the source when both the
  launcher registry and a store name the same folder, because the registry
  only tracks where the launcher last saw the game.
- Nested executables (Wuthering Waves, NTE's engine build) identify by the
  executable and walk up to the install root, so de-duplication by
  (title, region, normalised installDir) works the same way for them.
- Anti-cheat and terms: our preset only writes values the game's own options
  expose, at the game's own storage, while the game is not running. The
  community frame-rate unlockers (memory patches) and parent-process spoofs
  are deliberately not adopted. Sparxie touch is the one injection-based
  path, behind its own risk notice.
- The general-purpose launchers (Playnite, commit e2e6efc) reach these games
  through library plugins that read the same store records this host reads
  (Steam libraryfolders.vdf and appmanifest files, Epic .item manifests);
  no HoYoPlay-first-class library plugin exists in their default set.

## The Sparxie mark

The poster badge of a touch application (see "Poster badge" in
[sparxie-api.md](sparxie-api.md)) draws the Sparxie mark, `sparxie-256.png` from
the Sparxie repository (github.com/ShadowLemoon/Sparxie, assets/), unmodified
and only to identify the component the application starts through. The host
embeds it as `src/sparxie/badge_mark.inc` (the PNG bytes). Sparxie's licence is a
proprietary draft: the mark is used as decided on 2026-11-11, for
identification only, and is not a licence to redistribute Sparxie itself. If
the Sparxie author asks for it to go, it is that one include and `with_badge`
in `art.cpp`; the posters then come out plain.

## Unverified

- Only Endfield cn (the 鹰角启动器 install on a Windows test VM) was a real
  install; the other titles' detection paths are verified against fake
  registries, fake launcher files and fake store trees only.
- The Hypergryph launcher lookup now reads the three uninstall records and
  the default folders; the hypergryphlauncher* URL-protocol discovery named
  in endfield-nolauncher remains unimplemented, so a launcher that uninstalled
  its record and sits outside the default folders is still not found.
- Whether a direct start of Client-Win64-Shipping.exe (Wuthering Waves,
  non-store build) passes its anti-cheat on a real machine is unverified.
- The Epic AppName for Endfield and the exact Steam installdir layouts were
  not observed on real installs; Steam installdirs are read from the
  appmanifest, Epic matches by executable inside InstallLocation.
