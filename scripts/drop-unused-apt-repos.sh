#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Remove third-party apt repositories the GitHub runner image ships and this
# project does not use.
#
# ## Why this exists
#
# `apt-get update` exits non-zero if *any* configured source fails, and the
# `ubuntu-24.04` runner image ships two `packages.microsoft.com` repositories
# (azure-cli, and the Microsoft "prod" repo) that nothing in this tree
# references. On 2026-08-29 both began returning `403 Forbidden`:
#
#     E: Failed to fetch https://packages.microsoft.com/repos/azure-cli/dists/noble/InRelease  403  Forbidden
#     E: The repository '...' is no longer signed.
#
# Every Ubuntu archive source was `Hit:` and healthy. But because the install
# steps are written `apt-get update && apt-get install`, the failing update
# short-circuited the install, and a repository the build never reads took down
# `Build & Test (g++-13, x64)` on PR #288. Retrying did not help: all three
# attempts failed identically in about a second each, and the resolved IP
# rotated between them, so it was not a single bad edge node.
#
# ## Why removal rather than `apt-get update || true`
#
# Ignoring the exit code would also swallow a genuine failure of the Ubuntu
# archive — the repositories this project actually installs from — and let the
# build proceed against stale package lists, which fails later and less
# legibly. Deleting the sources we do not use keeps `apt-get update`'s exit
# code meaningful for the ones we do.
#
# ## Safety
#
# `rm -f` on a list of fixed paths: a no-op on any image that does not have
# them, which is why this is safe to call unconditionally from every job,
# including ARM runners and any future non-Azure image.

set -euo pipefail

# Repositories the runner image adds that this project never installs from.
#
# **Both the `.list` and the deb822 `.sources` spelling of each are listed, and
# that is not belt-and-braces.** The `ubuntu-24.04` image mixes the two formats:
# a real run on 2026-08-29 dropped `microsoft-prod.list` *and*
# `azure-cli.sources`. Listing only `.list` would have left the azure-cli
# repository in place — one of the two that returned 403 — and fixed nothing.
# Do not "tidy" the apparent duplicates away.
#
# Add to this list only for a repo that is genuinely unused; removing one the
# build depends on would turn a clear "404 on package X" into a confusing
# "package X has no installation candidate".
readonly UNUSED_SOURCES=(
    /etc/apt/sources.list.d/microsoft-prod.list
    /etc/apt/sources.list.d/azure-cli.list
    /etc/apt/sources.list.d/microsoft-prod.sources
    /etc/apt/sources.list.d/azure-cli.sources
)

for source in "${UNUSED_SOURCES[@]}"; do
    if [[ -e "$source" ]]; then
        echo "dropping unused apt source: $source"
        sudo rm -f "$source"
    fi
done
