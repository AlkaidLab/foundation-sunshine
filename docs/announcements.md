# Announcements

Announcements let the maintainers show a short notice in the Sunshine control panel and the
Web UI without shipping a new build. The notice comes from one JSON file published beside the
release metadata. The code lives in `src/announcements/` (import `foundation-ui/announcements`)
and has no framework dependency; the panel and the Web UI share it.

## Where the file lives

| Host | URL |
| --- | --- |
| Primary | `https://www.alkaidlab.com/release-metadata/foundation-sunshine-announcements.json` |
| Mirror | `https://www.alkaidlab.cn/release-metadata/foundation-sunshine-announcements.json` |

Publish the same file on both hosts, next to `foundation-sunshine.json`. Both answer with
`Access-Control-Allow-Origin: *`, which the Web UI needs. A client requests both hosts at once and
uses the first usable answer; a host that is slow, offline or answers anything unusable is ignored.

Until the file is published the hosts answer 404. That is the normal state: a missing file, a 404,
a body that is not JSON, a document that does not validate and an unreachable network all mean
"no announcements" and show nothing, never an error.

## File format

```json
{
  "schema": 1,
  "items": [
    {
      "id": "2026-11-driver-update",
      "severity": "warn",
      "title": { "en": "...", "zh": "..." },
      "body": { "en": "...", "zh": "..." },
      "url": "https://example.org/page",
      "starts": "2026-11-01T00:00:00Z",
      "ends": "2026-12-01T00:00:00Z",
      "minVersion": "2026.900.0",
      "maxVersion": "2026.1100.0",
      "pinned": true
    }
  ]
}
```

A complete example is in `announcements.sample.json`.

| Field | Required | Rules |
| --- | --- | --- |
| `schema` | yes | The number `1`. Any other value makes the whole file be ignored. |
| `items` | yes | Array. At most 50 items are kept: the first 50 valid ones, scanning at most the first 500 entries. |
| `id` | yes | 1 to 64 characters from `A-Z a-z 0-9 . _ -`. Unique; a repeated id is dropped. The read state is stored by id, so never reuse an id for a different notice. |
| `severity` | yes | `info`, `warn` or `critical`. |
| `title` | yes | Object from locale to text, at least one valid entry. Up to 120 characters per entry. |
| `body` | no | Object from locale to text. Up to 1000 characters per entry. Newlines are allowed. |
| `url` | no | `https:` only, at most 500 characters, no user name or password. |
| `starts` | no | ISO-8601. Not shown before this moment. |
| `ends` | no | ISO-8601. Not shown from this moment on. Must be after `starts`. |
| `minVersion` | no | Shown only on versions greater than or equal to this. |
| `maxVersion` | no | Shown only on versions less than or equal to this. |
| `pinned` | no | Boolean. Sorts first within its severity. |

Unknown fields are ignored, so newer files stay readable by older clients.

### Validation

Every item is checked on its own. An item that breaks a rule is dropped and the others are kept.
The rules that drop a whole item: a bad or duplicate `id`, an unknown `severity`, no valid
`title`, a `url` that is not a plain `https:` URL within 500 characters (`javascript:`, `http:`,
`data:` and the like are all rejected), a date that does not parse, `ends` not after `starts`, an
unparsable version, or a field of the wrong type (for example `pinned: "yes"`). A single locale
entry that is too long or not a string is dropped and the item keeps its other entries.

Text is plain text. It is never rendered as HTML or Markdown. Control characters, line and
paragraph separators, byte order marks and the bidirectional override controls are removed;
in titles newlines are removed as well, in bodies they are kept. Whitespace at both ends is
trimmed and nothing inside the text is collapsed. Lengths are counted in characters
(code points), not UTF-16 units.

A file larger than 256 KB is ignored.

### Dates

`starts` and `ends` accept `YYYY-MM-DD` (midnight UTC) or `YYYY-MM-DDThh:mm[:ss[.fff]]` followed by
`Z` or an offset such as `+08:00`. A date-time without an offset is rejected because it would
mean a different moment in every time zone. An impossible date such as `2026-02-31` is rejected.
An item is active from `starts` (inclusive) until `ends` (exclusive).

### Versions

Versions are dotted numbers compared segment by segment, missing segments counting as 0. A leading
`v` and everything after the numeric part are ignored: `v2026.925.152547.suffix` is
`2026.925.152547` and `1.2.3-beta.1` is `1.2.3`. Both bounds are inclusive. If the running version
is unknown, an item that has a version bound is not shown.

### Locales

`title` and `body` are keyed by locale (`zh`, `en`, `zh_TW`, `zh-CN` and so on; `_` and `-` are the
same and case is ignored). The text for the user's locale is chosen in this order: the exact
locale; the same locale with trailing parts removed (`zh_TW` then `zh`); any other key of the same
language; `en`; the first key in the file. Title and body are resolved separately.

## Behaviour in the client

Order: `critical`, then `warn`, then `info`; inside a severity pinned items first, then the newest
`starts` first (items without `starts` last), then by `id`. The item to surface prominently is the
first one the user has not dismissed.

Dismissing stores the id in `localStorage` under `foundation-ui.announcements.dismissed` as a JSON
array of ids (oldest first, at most 200; the oldest are forgotten first). A missing or corrupted
value counts as "nothing dismissed".

Critical items cannot be dismissed. A critical item is dismissible only once its `ends` has passed
and by then it is no longer active, so while it is shown it stays; without `ends` it never goes
away until the file changes. Use critical sparingly and always set `ends`.

The client never sends cookies, custom headers, a referrer or a query string, and uses no
identifier or analytics. The request is a plain `GET` with `credentials: 'omit'`,
`referrerPolicy: 'no-referrer'` and `cache: 'no-store'` (the client keeps its own cache).

## Publishing

1. Edit the file and validate it by running it through `parseAnnouncements` (a mistake only drops
   the affected item, so check that every item you wrote survives).
2. Upload the identical file to `release-metadata/` on `alkaidlab.com` and `alkaidlab.cn`.
3. Clients cache the file for 30 minutes (in memory and in `localStorage` under
   `foundation-ui.announcements.cache`), and ask the server without using the browser HTTP cache.
   A change therefore reaches running clients within about 30 minutes plus whatever a CDN
   in front of the hosts holds (the release metadata uses `max-age=600`); newly started
   clients see it as soon as their cache expires.
4. A client that cannot reach either host keeps showing its last good copy for up to 24 hours,
   then nothing. A 404 from both hosts (file removed) clears it at the next refresh.
5. To retire an item, set `ends` to the current time (or any past time) and leave it in the file for
   a while, or delete it. Setting `ends` ahead of time is best: it also covers clients that
   still hold a cached copy, because the window is checked on every display, not only on fetch.
6. Never reuse an `id`. Someone who dismissed `maintenance` will not see a later, different
   notice with the same id. Put the date or a counter in ids.
