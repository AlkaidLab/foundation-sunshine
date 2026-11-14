# Default applications (Windows host)

The bundled `apps.json` (and `apps.zh.json` of the Chinese installer) ships four entries: Desktop, Big Screen, Steam Big Picture and Xbox Game.

## Covers: portrait and landscape

An application has a portrait cover, `image-path` (stock Moonlight shows it), and optionally a landscape cover, `hero` (the `hero` of the client
interface, served to clients as `/appasset?type=hero` and listed in `FsArt`). Either may be set, both, or none. A relative path is looked up in the
host's `assets` folder, then in the `covers` folder of the application data (where the Web UI puts an upload), then as an absolute path.
The editor (Applications -> an application -> Image Settings) has a slot for each: 竖版封面 / Portrait cover (2:3) and 横版封面 / Landscape cover (16:9).

The default entries ship both: `steam.png` + `steam-hero.png`, `box.png` + `box-hero.png`, `desktop` (the wallpaper) + `desktop-hero.png`, `bigscreen.png` + `bigscreen-hero.png`.
Sources and terms of the art: `assets/brand/default-apps/README.md`.

## Names follow the language

Each default entry carries `default-id` (`desktop`, `big-screen`, `steam`, `xbox`). While the name of a marked entry is still one of the names of that entry
(`src/default_app_names.h`, 19 languages), it is shown in the language of the host (`locale` in the configuration): when the language is saved with a new value
(the Web UI language choice, the guide, or the control panel) the names in apps.json are rewritten and the application list is refreshed. The same holds for the
two commands of the Desktop entry (Touch Keyboard, Desktop Pet; by their `id`). A name the person wrote is never changed again.

An apps.json of an older install is marked once at start-up: an app gets `default-id` (and its landscape cover, once) when what it opens is a default entry AND its name is one of that
entry's names. The id of an application is made from the English name of a marked entry, so the application ids do not change with the language.

Translations other than English and Chinese were written for this table and should be reviewed by native speakers.
