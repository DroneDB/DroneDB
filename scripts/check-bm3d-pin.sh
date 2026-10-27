#!/usr/bin/env bash
# Checks that the bm3d submodule points at the release the build says it ships.
#
# The bm3d program is built from vendor/bm3d, and its GPL source offer (THIRD_PARTY_LICENSES.md)
# names the bm3d release at the tag the submodule points to. That only holds when the submodule
# commit is exactly the commit of the tag v<VERSION>, VERSION being the text of vendor/bm3d/VERSION
# (the version `bm3d --version` prints, scripts/helper-tools.txt). A submodule moved to a commit
# between releases, or a VERSION that was not bumped, would ship a binary whose source is not the
# one the offer names.
#
# The tag is asked of the bm3d repository itself (git ls-remote on the submodule's origin), so it
# is the real tag and not whatever tags the local clone happens to have; an annotated tag is
# compared by the commit it points to.
#
# Usage:  scripts/check-bm3d-pin.sh [SUBMODULE_PATH]     (default: vendor/bm3d, run from the repository root)
# Exit codes: 0 the pin is a release, 1 it is not, 2 the check could not run.

set -u -o pipefail

module="${1:-vendor/bm3d}"

fail() { echo "::error::$*" >&2; exit 1; }
cannot() { echo "error: $*" >&2; exit 2; }

git -C "$module" rev-parse --git-dir >/dev/null 2>&1 \
    || cannot "$module is not a checked-out submodule (git submodule update --init $module)"

pinned="$(git rev-parse "HEAD:$module" 2>/dev/null)" \
    || cannot "$module is not a submodule of this repository (no commit is recorded for it)"

# The version file of the pinned commit, not of whatever is checked out
version="$(git -C "$module" show "$pinned:VERSION" 2>/dev/null | sed -n 1p | tr -d '[:space:]')" \
    || cannot "cannot read VERSION at commit $pinned of $module"
[ -n "$version" ] || fail "$module/VERSION is empty at commit $pinned"
tag="v$version"

remote="$(git -C "$module" ls-remote origin "refs/tags/$tag" "refs/tags/$tag^{}")" \
    || cannot "cannot list the tags of the origin of $module"
# A lightweight tag is one line; an annotated tag is two, and the peeled "^{}" line is the commit
tagged="$(printf '%s\n' "$remote" | awk -v ref="refs/tags/$tag^{}" '$2 == ref {print $1}')"
[ -n "$tagged" ] || tagged="$(printf '%s\n' "$remote" | awk -v ref="refs/tags/$tag" '$2 == ref {print $1}')"

[ -n "$tagged" ] || fail "$module/VERSION says $version, but the bm3d repository has no tag $tag: tag the release, or fix VERSION"

if [ "$tagged" != "$pinned" ]; then
    fail "$module is at commit $pinned, but the tag $tag (VERSION $version) is commit $tagged: point the submodule at the release tag, not between releases"
fi

echo "OK: $module is at $tag ($pinned)"
