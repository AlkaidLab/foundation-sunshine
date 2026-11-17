#!/usr/bin/env bash
# Create the pre-release of a manual publish, or refresh the assets of the pre-release that is already there.
#
# usage: publish-prerelease.sh <dist dir>        (environment: TAG, BODY, COMMIT, GH_TOKEN, GITHUB_REPOSITORY)
#
# A tag that belongs to a published (full) release is never touched: replacing the assets of a release people already
# downloaded from would change what they got after the fact. Publishing the same commit twice is fine while the release is
# still a pre-release.
set -euo pipefail

dist="${1:?dist directory required}"
: "${TAG:?}" "${COMMIT:?}" "${GITHUB_REPOSITORY:?}"
body="${BODY:-}"

ls -l "$dist"
err="$(mktemp)"
if state="$(gh release view "$TAG" --repo "$GITHUB_REPOSITORY" --json isPrerelease --jq .isPrerelease 2>"$err")"; then
  if [[ "$state" != true ]]; then
    echo "::error title=Publish::release $TAG exists and is not a pre-release; its assets are not replaced"
    exit 1
  fi
  gh release upload "$TAG" "$dist"/* --clobber --repo "$GITHUB_REPOSITORY"
elif grep -qi "not found" "$err"; then
  gh release create "$TAG" "$dist"/* --repo "$GITHUB_REPOSITORY" --target "$COMMIT" --prerelease --title "$TAG" --notes "$body" --generate-notes
else
  cat "$err" >&2
  echo "::error title=Publish::could not read the release $TAG"
  exit 1
fi
gh release view "$TAG" --repo "$GITHUB_REPOSITORY" --json url,tagName,assets --jq '"\(.url)\n\(.assets|map(.name)|join("\n"))"'
