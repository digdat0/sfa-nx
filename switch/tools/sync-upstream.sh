#!/bin/bash
# Refresh the local branch "upstream-clean": Foxhollow's main branch with Nintendo assets removed from every
# commit, so SFA-NX can keep Foxhollow's history (authors, dates, messages) and merge its future commits
# without the repository ever containing those files.
#
#   switch/tools/sync-upstream.sh          then:   git merge upstream-clean
#
# How it works: a throwaway clone of Foxhollow is run through git filter-repo, which drops the paths below
# from every commit. The filter is deterministic: the same upstream commit always becomes the same filtered
# commit, so each run only appends Foxhollow's new commits and "upstream-clean" moves forward normally
# (a fetch that is not a fast-forward stops the script instead of rewriting anything).
#
# Needs git-filter-repo (py -3 -m pip install git-filter-repo). Override the source with SFA_NX_UPSTREAM.
set -e
cd "$(dirname "$0")/../.."

UPSTREAM_URL="${SFA_NX_UPSTREAM:-https://github.com/JackPriceBurns/foxhollow.git}"

# Paths removed from upstream history. Keep this list in step with the pre-push guard (switch/tools/hooks/
# pre-push): extern/aurora/assets/screenshot.png is a Twilight Princess screenshot from upstream Aurora,
# CLAUDE.md is Foxhollow's assistant notes file.
STRIP=(extern/aurora/assets/screenshot.png CLAUDE.md)

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== cloning $UPSTREAM_URL"
git clone -q --single-branch --branch main --no-tags "$UPSTREAM_URL" "$WORK/up"

echo "== removing ${STRIP[*]} from every commit"
ARGS=(--force --quiet --invert-paths)
for p in "${STRIP[@]}"; do ARGS+=(--path "$p"); done
git -C "$WORK/up" filter-repo "${ARGS[@]}"

# Fast-forward only (no "+"): if this ever fails, the filter produced different commits than last time and
# merging would duplicate history, so stop and look instead.
git fetch -q "$WORK/up" main:refs/heads/upstream-clean
echo "== upstream-clean is at $(git log -1 --format='%h %s' upstream-clean)"
echo "   merge new Foxhollow commits with: git merge upstream-clean"
