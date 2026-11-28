# Generated application covers

This contract is implemented by the Windows host first. Linux and macOS keep their current application-art selection until their host renderers are added.

## Configuration

The global `generated_cover_style` setting is stored in the host configuration and defaults to `tonal`. Its values are `tonal`, `monogram`, `icon`, `editorial`, `glass`, and `geometric`.

Each application may carry `cover-style` in `apps.json`. The value is `auto` or one of the six style names. `auto` follows `generated_cover_style`. Invalid values are rejected by `POST /api/apps`.

The Xbox default entry is identified by its command: an entry whose `cmd` contains `start xbox:` (the command of the shipped entry, whatever its name). Its `box.png` remains Xbox art, also when its `image-path` is empty. An entry with any other command is never the Xbox entry.

## Upgrading from a version without generated covers

An application that shows the placeholder picture (`assets/box.png`) before the update shows a generated cover after it, with no action by the person. The rule is applied when the host reads `apps.json`; the file is not rewritten, so no backup is needed, a downgrade finds the file as it was, and the application ids (made of the name and the picture file) do not change.

An application shows the placeholder when it is not the Xbox entry and:

- its `image-path` is missing, empty or blank;
- its `image-path` is `box.png`, `./assets/box.png` or `.\assets\box.png` (any case, with or without quotes), the names the default Xbox entry and older versions wrote;
- its `image-path` is the absolute path of the host's own `assets/box.png`;
- its `image-path` names a file that is gone, or is not an image the host can use (the poster lookup falls back to `assets/box.png`).

A picture of the person's is never replaced, also when it is called `box.png` in another folder: it is the host's file, not the name, that makes the placeholder.

## Art version

`FsArtVersion` of the application list (and the `ETag` of hero art and the application list tag, which carry it) includes the generated cover of an application that has no picture of its own: the cache keys of both orientations, which hold the generator version, the style, the name and the program icon. The version of an application that keeps its poster, hero, logo, icon or Xbox art is the one it had before. A client that keeps art by version therefore fetches the cover once when the application switches from the placeholder, and again when the style, the name or the cover design changes. The `ETag` of a poster is that of the served file, so a conditional request for the old placeholder gets the cover too.

## Application list

`GET /api/apps` retains the existing application object and adds the following fields.

```json
{
  "cover-source": "poster | generated | xbox",
  "cover-effective-style": "tonal | monogram | icon | editorial | glass | geometric",
  "cover-portrait-url": "/appasset?appid=<id>&type=poster",
  "cover-landscape-url": "/appasset?appid=<id>&type=hero",
  "cover": {
    "source": "poster | generated | xbox",
    "style": "tonal | monogram | icon | editorial | glass | geometric",
    "portrait": "<same portrait URL>",
    "landscape": "<same landscape URL>"
  }
}
```

An existing poster remains the portrait URL. An existing hero is the landscape URL when available. A generated application has both URLs. Generated portrait covers are 600×900. Generated landscape covers are 1920×1080, which is the host's existing landscape poster ratio.

## Preview

`GET /api/apps/cover-preview?appid=<id>&style=<style>&orientation=portrait|landscape` renders one requested style for the picker. It uses the normal application authentication and origin checks. The response is the image bytes with `Content-Type`, a cache validator, and a short private cache lifetime.

## Asset delivery

`/appasset?appid=<id>&type=poster` serves the portrait selection. `type=hero` serves the landscape selection. Generated files are also stored under `appdata()/covers/gen_<hash>.<ext>` and are available through `/boxart/gen_<hash>.<ext>`. JPEG is encoded at quality 85. PNG is selected when its encoded bytes are smaller.

The cache hash includes `generator_version`, style, orientation, application name, executable icon path, and executable modification time. Old generated files are pruned by age. Rendering failures use the existing `box.png` fallback through the current poster path.

## Implementation authority

The renderer uses Direct2D software WIC rendering and DirectWrite text layouts. Font selection uses system fonts and DirectWrite fallback, including color emoji. The implementation relies on `installer-ms-direct2d-portal-20261107`, `installer-ms-direct2d-write-20261107`, `installer-ms-directwrite-start-20261107`, `ms-wic-factory-20261110`, `fs-ms-dwrite-createtextlayout-20261123`, `fs-ms-shellitemimagefactory-getimage-20261123`, and `fs-ms-extracticonexw-20261123` in `docs/authority/source-index.tsv`.
