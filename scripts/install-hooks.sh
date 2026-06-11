#!/usr/bin/env bash
# Install the pre-commit hook (format check + coverage ratchet).
# Run once after cloning: bash scripts/install-hooks.sh
set -euo pipefail

REPO=$(git rev-parse --show-toplevel)
HOOK_SRC="${REPO}/scripts/pre-commit-coverage.sh"
HOOK_DST="${REPO}/.git/hooks/pre-commit"

chmod +x "${HOOK_SRC}"

if [[ -f "${HOOK_DST}" && ! -L "${HOOK_DST}" ]]; then
    echo "WARNING: ${HOOK_DST} already exists and is not a symlink."
    echo "         Rename or remove it, then re-run this script."
    exit 1
fi

ln -sf "${HOOK_SRC}" "${HOOK_DST}"
echo "Installed: ${HOOK_DST} -> ${HOOK_SRC}"
echo ""
echo "The hook runs two checks on every commit:"
echo "  1. clang-format: staged .cpp/.hpp files must be formatted"
echo "  2. coverage ratchet: line coverage must not decrease"
echo ""
echo "Escape hatches for WIP commits:"
echo "  SKIP_FORMAT_CHECK=1   git commit -m '...'  (skip format only)"
echo "  TIDY_CHECK=1          git commit -m '...'  (opt-in tidy on staged .cpp)"
echo "  SKIP_TIDY_CHECK=1     git commit -m '...'  (hard-skip tidy)"
echo "  SKIP_COVERAGE_CHECK=1 git commit -m '...'  (skip coverage only)"
