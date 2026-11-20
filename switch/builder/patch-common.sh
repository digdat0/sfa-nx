#!/usr/bin/env bash
# Not used by the SFA-NX build (build.sh applies its three Aurora patches with patch(1)); kept from Melee-NX.
# git apply filters Git-format paths when invoked from a repository subdirectory.
# Use the actual Git root and prefix all tree-relative paths explicitly, so a
# reverse check cannot silently succeed by skipping the entire patch.
git_apply_tree() {
  local tree="$1"; shift
  local root prefix
  root="$(git -C "$tree" rev-parse --show-toplevel)" || return
  prefix="$(git -C "$tree" rev-parse --show-prefix)" || return
  local directory=()
  [[ -z "$prefix" ]] || directory=("--directory=${prefix%/}")
  git -C "$root" apply -p1 "${directory[@]}" "$@"
}
