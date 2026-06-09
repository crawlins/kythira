#!/usr/bin/env bash
# Install the coverage pre-commit hook.
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
echo "The hook will run on every commit. To skip it for a WIP commit:"
echo "  SKIP_COVERAGE_CHECK=1 git commit -m '...'"
