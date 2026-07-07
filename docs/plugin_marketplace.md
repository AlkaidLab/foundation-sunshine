# Sunshine Plugin Marketplace

This document describes the GitHub-operated marketplace model for Sunshine Plugin Platform v1.

The marketplace is designed as an open-source registry rather than a custom backend. GitHub remains the system of record for review, release, provenance, and community discussion.

## Roles

- Plugin repositories publish source code and GitHub Releases.
- The registry repository stores reviewed marketplace metadata.
- GitHub Actions validates registry pull requests and generates `index.json`.
- GitHub Pages serves `index.json`.
- Sunshine reads the index and displays marketplace entries in the Web UI.

## Registry Repository

The registry can live in a dedicated public repository such as:

```text
AlkaidLab/sunshine-plugin-registry
```

The scaffold in `plugin-registry/` can be copied into that repository:

```text
plugin-registry/
  plugins/<plugin-id>.json
  schemas/listing.schema.json
  blocklist.json
  index.json
scripts/plugin-registry/build-index.js
.github/workflows/plugin-registry.yml
.github/ISSUE_TEMPLATE/plugin_listing.yml
```

## Listing Lifecycle

New listings start as pull requests against `plugins/<plugin-id>.json`.

Allowed listing states:

- `draft`: metadata is being reviewed and is not visible in the marketplace.
- `listed`: visible and installable when platform-compatible.
- `deprecated`: visible, but Sunshine should warn users and show migration guidance.
- `unlisted`: hidden from marketplace browsing, but retained for historical continuity.
- `blocked`: visible as a safety warning; clients must refuse installation.

Delisting should prefer state changes over file deletion. Keeping old metadata makes audits, support, and installed-plugin warnings possible.

## Release Requirements

Each visible listing must point to release assets from GitHub:

- a release URL;
- one or more platform assets;
- SHA-256 for each asset;
- declared capabilities;
- publisher and support URLs;
- license metadata.

Future phases should require GitHub artifact attestations for official and verified-publisher plugins.

## Trust Tiers

Marketplace UI should present trust as explicit metadata:

- `official`: maintained by the Sunshine project or AlkaidLab.
- `verified-publisher`: publisher identity and release repository reviewed by maintainers.
- `community`: reviewed for registry metadata, but not endorsed as official.
- `experimental`: visible only when users opt in to experimental listings.

Trust does not replace capability prompts. A trusted plugin still needs to disclose sensitive behavior.

## Safety Operations

Emergency removals use `blocklist.json`.

Use `blocked_plugins` when every version of a plugin is unsafe. Use `blocked_versions` when only a specific version or artifact is unsafe.

The registry workflow supports `workflow_dispatch` so maintainers can regenerate and publish the blocklist quickly without waiting for unrelated release automation.

## Sunshine Integration

Sunshine exposes:

```text
GET /api/plugins
GET /api/plugins/marketplace
```

`/api/plugins` lists installed plugins and their local state. `/api/plugins/marketplace` fetches the remote registry index and annotates each marketplace entry with:

- `installed`
- `installed_version`
- `platform_supported`
- `installable`

By default Sunshine reads:

```text
https://alkaidlab.github.io/sunshine-plugin-registry/index.json
```

For development and staging, set `SUNSHINE_PLUGIN_MARKETPLACE_INDEX_URL` before starting Sunshine.

The first UI integration is browse-only. It opens the plugin release page instead of installing directly. Direct install/update should wait until hash verification, archive extraction policy, signature or attestation checks, and rollback semantics are implemented.

## Review Checklist

Maintainers should check:

- plugin id matches reverse-DNS format and `plugin.json`;
- publisher and repository are legitimate;
- release assets are built from the listed repository;
- capabilities match the plugin behavior;
- license is compatible with ecosystem policy;
- release asset hashes match;
- platform support is accurate;
- security-sensitive capabilities have clear user-facing rationale.

## Future Work

- one-click install with SHA-256 verification;
- plugin update checks and rollback;
- attestation verification;
- verified-publisher workflow;
- per-capability install prompts;
- full plugin log viewing;
- marketplace categories, search ranking, and featured lists.
