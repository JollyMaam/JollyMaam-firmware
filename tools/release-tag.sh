#!/usr/bin/env bash
# Tag HEAD as today's next release, YYYY.MM.DD.n (UTC), with an annotated tag
# (signed when tag.gpgSign is set), then print the push that starts the
# release build (.github/workflows/ci.yml).
#   tools/release-tag.sh
set -euo pipefail

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	echo "uncommitted changes: commit or stash them first" >&2
	exit 1
fi
git fetch --tags --quiet origin 2>/dev/null || echo "warning: could not fetch tags from origin" >&2

day=$(date -u +%Y.%m.%d)
n=1
while git rev-parse --quiet --verify "refs/tags/$day.$n" >/dev/null; do
	n=$((n + 1))
done
tag=$day.$n
git tag -a "$tag" -m "Release $tag"
echo "tagged $tag; start the release build with:"
echo "  git push origin $tag"
