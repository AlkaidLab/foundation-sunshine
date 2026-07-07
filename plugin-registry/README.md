# Sunshine Plugin Registry

This directory is a starter registry for a GitHub-operated Sunshine plugin marketplace.

The registry is intentionally static:

- plugin authors publish release artifacts in their own GitHub repositories;
- this registry stores reviewed marketplace metadata;
- GitHub Actions validates submissions and generates `index.json`;
- GitHub Pages can serve the generated index for the Sunshine Web UI.

## Layout

```text
plugin-registry/
  plugins/                 # one JSON listing per plugin id
  schemas/listing.schema.json
  blocklist.json           # emergency deny list for plugins or versions
  index.json               # generated marketplace index
```

## Listing Flow

1. Publish a plugin release from the plugin repository.
2. Add `plugins/<plugin-id>.json`.
3. Run `node scripts/plugin-registry/build-index.js --check`.
4. Open a pull request and wait for registry review.

The generated index only lists entries with `status` set to `listed`, `deprecated`, or `blocked`. Draft entries can stay in review without appearing in the marketplace.

## Status Values

- `listed`: visible and installable when platform-compatible.
- `deprecated`: visible with a migration warning.
- `blocked`: visible only as a safety warning; clients must refuse install.
- `unlisted`: retained for history but hidden from marketplace browsing.
- `draft`: review-only metadata; hidden from marketplace browsing.
