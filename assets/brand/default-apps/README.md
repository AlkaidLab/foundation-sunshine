# Default app artwork: provenance and terms

Where the files live: `steam.png`, `steam-hero.png`, `box.png`, `box-hero.png` in `src_assets/common/assets/`; `desktop-hero.png` and
`bigscreen-hero.png` in `src_assets/windows/assets/`. Every one is installed into the host's `assets` folder; the `*-hero.png` ones are the
`hero` of the default entries of apps.json (served to clients as `/appasset?type=hero`), the others are their `image-path`.
Regenerating: `python3 -I build_svgs.py && sh render.sh` in this folder writes the SVGs to `svg/` and the PNGs next to the script; copy them to the folders above.
The downloaded official files (`sources/`: Valve's EPS and PDF, saved pages) are not committed (size); their URLs and sha256 are below and in
`docs/authority/source-index.tsv`.

Generated 2026-11-10. All six new files are flat vector designs exported from the SVG sources in `svg/`
(soft gradient fields, the brand marks, outlined text). No AI-generated imagery, no stock photos, no
watermarks, no sparkle. The previous AI-generated `steam.png` and `box.png` (external-provider sparkle watermark,
no Xbox mark) are replaced.

Regenerate: `python3 -I build_svgs.py && sh render.sh` (rsvg-convert + Pillow optimize; re-rendering produced
byte-identical PNGs). `build_svgs.py` reads `marks/steam-logo-inverse.d` (path extracted unaltered from Valve's EPS),
`marks/xbox-sphere-FALLBACK.d` (see below), the existing `assets/brand/bigscreen-cover.svg`, and the font
`/usr/share/fonts/TTF/OpenSans-CondensedBold.ttf` (Open Sans, Apache-2.0), whose glyphs are converted to outlines
so the SVG renders identically everywhere.

## Trademarks

Steam and the Steam logo are trademarks of Valve Corporation. Xbox and the Xbox sphere logo are trademarks of
Microsoft Corporation. The marks appear only to identify the launcher shortcut that opens the installed app.
Foundation Sunshine is not affiliated with, sponsored by or endorsed by Valve or Microsoft.

## Outputs

| file | size | what | source svg | sha256 | bytes |
|---|---|---|---|---|---|
| `steam.png` | 600x900 | Steam Big Picture poster: blue-teal field (#1b2838 -> #2a475e -> #3b6a8c, #66c0f4 glow), official inverse steam(R) logo centred | `svg/steam.svg` | caed5185fc917963b1acc168d3a66863ad90bf5dedd3c80e67f833b7cda03126 | 107388 |
| `steam-hero.png` | 1920x1080 | same, 16:9 | `svg/steam-hero.svg` | fc935c5314f46744b6ad28557b0d1be9172147de1896d1a0d08abb9dacf408db | 310304 |
| `box.png` | 600x900 | Xbox Game poster: green field (#0b3d0b -> #107C10, lighter glow), white Xbox sphere centred | `svg/box.svg` | 890a5989a7586fe8a3e4858218cb6af6f070bb0b073541736266bcb83944a1df | 69952 |
| `box-hero.png` | 1920x1080 | same, 16:9 | `svg/box-hero.svg` | b0f2bb1d8e8422d65c6d74fb9b4fe988eb4c8426c28b4acb54393da623086a86 | 205290 |
| `desktop-hero.png` | 1920x1080 | Desktop: same dark grey gradient (#212121 -> #757575) and monitor line icon as `desktop.png`, icon left, outlined uppercase DESKTOP right | `svg/desktop-hero.svg` | 67edfdbe756574345b8ec56ceb582975bb4a9181919c75acc33cd041290c67db | 48997 |
| `bigscreen-hero.png` | 1920x1080 | Big Screen landscape: TV with the mascot on the left, game controller on the right; same shapes, palette and glow as `bigscreen-cover.svg`, re-arranged, nothing cropped | `svg/bigscreen-hero.svg` | 182ed84a5171a35a8409578cb0fbf5e6ecf7b0ad6329273f22101609bf683d0f | 239609 |
| `contact.png` | 2400x1080 | review sheet only, not an app asset | - | 42586e5e4a7ddab35736dcbb3540d6344d9a9ba8d61c84bb84636f2bcafc036e | - |

The Big Screen mascot (the 薇薇 / Foundation Sunshine logo, a raster `<image>` embedded in the original
`bigscreen-cover.svg`) is reused as-is from that file; it is existing project artwork, not produced here.
Everything else in `bigscreen-hero.svg` is the original vector shapes.

## Steam logo (official)

- Source: Valve Steamworks "Steam Brand Guidelines and Logos" page https://partner.steamgames.com/doc/marketing/branding
  (retrieved 2026-11-10; saved as `sources/partner_steamgames_marketing_branding.html`,
  sha256 18d0720edebe5db76ed6ae9d1b0887d5cd92afab442145e2ba247c97b988244c).
- Vector file: https://shared.akamai.steamstatic.com/community_assets/images/steamworks_docs/english/steam_brandAssets.eps
  -> `sources/steam_brandAssets.eps`, sha256 cb7aa790e3ed34755978eef763b4ea41670fd8075de14807485334a52d47cba0
  (6559670 bytes). Treated as data: Ghostscript (`-dSAFER -dEPSCrop`) converted it to PDF, `pdftocairo -svg` to SVG
  (`work/steam_all.svg`), and the single compound path of the inverse (white-on-dark) "steam(R)" logo, bbox
  133.39,451.71 - 388.38,529.30, was copied unchanged to `marks/steam-logo-inverse.d`. It is only translated and
  uniformly scaled (no distortion, no recolouring: white = the inverse logo, RGB 255,255,255, as specified).
- Guidelines: https://shared.akamai.steamstatic.com/community_assets/images/steamworks_docs/english/steam_brandGuidelines.pdf
  -> `sources/steam_brandGuidelines.pdf`, sha256 fa7f467f5dbb2b31b6f9891e7ec5e233d878cac231a6ca8d97db9a822e8ce02d.
- Rules applied: inverse logo on a darker background; logo stands alone (no label, no other graphic next to or on it,
  so the old "STEAM" text label is gone: the official logo already contains the wordmark and (R)); height far above the
  50 px on-screen minimum (poster logo height 121 px, hero 249 px); clear space (crank diameter, roughly 11% of the logo (estimate)
  width) is empty gradient.
- Terms found (quotes): "Nothing in these guidelines conveys any rights to use any Steam trademarks or logos. To make such
  use, you must receive a separate license from Valve." / "Steam branding may not be used in any manner that might imply
  that any non-Steam materials are sponsored, endorsed, licensed by, or affiliated with Steam. Use only Valve-approved
  artwork for branding." / "The logos provided must stand alone and may not be combined with any object, including but not
  limited to other logos, words, graphics, photos, slogans, numbers, design features, or symbols." / (web page) "The Steam
  trademarks may not be displayed as primary or prominent features on any non-Valve materials." / Valve "reserves the right
  to approve any communication using the Steam family of brands before its distribution"; contact
  steambranding@valvesoftware.com.
- Launcher-tile note: the Steam logo programme is written for games, hardware and partners. A shortcut tile that opens the
  installed Steam client is not covered by any published licence, and a large centred Steam mark can read as a
  "primary or prominent feature". The brief asked for a big centred mark; for a release, ask steambranding@valvesoftware.com
  to approve this tile, or shrink the mark and let the app name carry the tile.

## Xbox mark (FALLBACK, not official)

- The official Xbox logo files are only distributed through the Microsoft Game Dev Portal
  (https://developer.microsoft.com/en-us/games/resources/xbox-brand-assets/), which redirected to a Microsoft sign-in
  (Partner Center account required; confirmed 2026-11-10 with curl, and the Microsoft Learn page
  https://learn.microsoft.com/en-us/gaming/game-publishing/resources/managed-support/how-to-access-branding-and-marketing-assets
  says the same). Tried and found without logo files: microsoft.com/legal/intellectualproperty/trademarks,
  xbox.com/legal/fansiteguidelines, developer.microsoft.com/en-us/games/products/, news.xbox.com press kit (403),
  xbox.com/en-US/brand and xbox.com/legal/brand (404).
- The only public official Xbox graphic found, https://developer.microsoft.com/content/dam/microsoftgamedev/new/XboxSphere_1.png
  (`sources/XboxSphere_1.png`, 375x373 RGBA raster, sha256 8c455c73b5895ce9e94fa89d5d3bd3c92f05722407f50ca71884397ad0d10afc),
  is an old glossy green 3D raster, not the flat mono mark and not a vector, so it was not used and not traced.
- FALLBACK used in `box.png` and `box-hero.png`: the Xbox sphere path from simple-icons 16.37.0,
  https://cdn.jsdelivr.net/npm/simple-icons@latest/icons/xbox.svg (`sources/simple-icons-xbox_latest.svg`,
  sha256 c5ec61839e921542b3581d9aab7d72b822200975c4b508ea4f9bc02ce4c6f025, licence CC0-1.0 per `sources/simple-icons_LICENSE.md`
  and `sources/simple-icons_package.json`). It is a third-party redraw of the Microsoft mark. It is used unaltered in white
  (mono) on the Xbox green field, with no label, nothing overlapping it. Replace `marks/xbox-sphere-FALLBACK.d` with the official
  mono sphere once Partner Center access exists, then rerun `build_svgs.py` and `render.sh`.
- Terms found: Microsoft Trademark and Brand Guidelines, https://www.microsoft.com/en-us/legal/intellectualproperty/trademarks
  (`sources/microsoft_trademarks.html`, sha256 c70696866f8b644cb413d6bd2333d3ee256264cd0a119cba9aa21e9b33e0a69a): "Many uses,
  including our logos, app and product icons, and other designs, will require a license first"; "Don't alter, animate, distort,
  or misappropriate Microsoft's Brand Assets"; truthful compatibility statements in text are allowed. Xbox Fansite Guidelines,
  https://www.xbox.com/legal/fansiteguidelines (`sources/xbox_fansiteguidelines.html`, sha256
  4ccf07379f6351f70934ca9e72321d8b268bf8b79e44cbc0c08acdbcaaa44889), the only public Xbox logo rules: "You may not alter the XBOX Logos in
  any manner, including size, proportions, colors, elements, etc.", "Don't place anything behind, in front of, or otherwise overlap
  graphics on top of the XBOX Logos.", "Do not create a logo that imitates or is similar to any of the XBOX Logos." That page is for
  non-commercial fan sites; it does not license a launcher tile. A Microsoft logo licence is needed for a formal grant; the 2019 Xbox
  guidelines mirror names xboxbrand@microsoft.com for asset and review requests (third-party mirror, not verified).
- No Xbox wordmark or "XBOX" text label was added (it would be a redraw or an imitation of the lockup); the sphere stands alone.

## Checks run (see `chk.py`, output in the task report)

Exact pixel sizes (Pillow), maximum deviation of each 160x160 corner from a bilinear fit of the field (all channels),
white-mark bounding boxes, 4x enlarged corner crops in `check/corners-*.png` viewed by eye, full images viewed by eye.
