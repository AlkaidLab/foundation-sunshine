#!/usr/bin/env bash
# Create the stable release of a manual publish: a full release (not a pre-release) marked Latest, with the assets of this run.
#
# usage: publish-stable.sh <dist dir>        (environment: TAG, BODY, COMMIT, GH_TOKEN, GITHUB_REPOSITORY)
#
# The stable build is compiled as channel stable (SUNSHINE_BUILD_CHANNEL) and goes up directly, so nothing downstream ever sees it as a
# pre-release first. A tag that already has a release, of any kind, is an error: replacing the assets of a pre-release would
# publish binaries compiled as beta under a stable name, and a published release is never touched after people downloaded from it.
# A stable version is therefore a commit that has no release yet (the version comes from the committer date).
set -euo pipefail

dist="${1:?dist directory required}"
: "${TAG:?}" "${COMMIT:?}" "${GITHUB_REPOSITORY:?}"
# The bot token (secrets.GH_BOT_TOKEN), never the workflow token: a release created with the workflow token starts no other workflow,
# so the signing workflow (`release: published`) would not run for a stable release published this way.
if [[ -z "${GH_TOKEN:-}" ]]; then
  echo "::error title=Publish::GH_TOKEN is empty: the stable release is created with the bot token (GH_BOT_TOKEN) so that its release event starts the signing workflow"
  exit 1
fi
body="${BODY:-}"

ls -l "$dist"
err="$(mktemp)"
if gh release view "$TAG" --repo "$GITHUB_REPOSITORY" --json tagName >/dev/null 2>"$err"; then
  echo "::error title=Publish::release $TAG already exists; a stable release needs a commit that has no release yet"
  exit 1
elif ! grep -qi "not found" "$err"; then
  cat "$err" >&2
  echo "::error title=Publish::could not read the release $TAG"
  exit 1
fi
gh release create "$TAG" "$dist"/* --repo "$GITHUB_REPOSITORY" --target "$COMMIT" --latest --title "$TAG" --notes "$body" --generate-notes
if [[ "$(gh release view "$TAG" --repo "$GITHUB_REPOSITORY" --json isPrerelease --jq .isPrerelease)" != false ]]; then
  echo "::error title=Publish::release $TAG was created as a pre-release"
  exit 1
fi
gh release view "$TAG" --repo "$GITHUB_REPOSITORY" --json url,tagName,assets --jq '"\(.url)\n\(.assets|map(.name)|join("\n"))"'
