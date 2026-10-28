#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Builds Cairn's engine image, unmodified, from the commit task 1a pinned
# (.kiro/specs/cairn-chaos-testing/ tasks 1a and 10). Cairn publishes no
# engine image; its own docker-compose.yaml builds the server and the
# dispatcher from the repository's Dockerfile, and so does this, tagging
# the result kythira-cairn-engine:v0.2.1 for cairn-compose.yml.
#
# Usage: build-engine.sh [tag]
#
# The checkout is verified against the pinned commit before anything is
# built. Cairn's Dockerfile installs its locked dependencies from the
# mirrors.aliyun.com PyPI index; that is upstream's choice and is left as
# it is. Uses $KYTHIRA_CONTAINER_RUNTIME (default docker).

set -euo pipefail

repo_url="https://github.com/oritera/Cairn"
commit="e0ef2f850e5805f824815ee38f049e066deeb7d1"
tag="${1:-kythira-cairn-engine:v0.2.1}"
runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

git -C "$work" init -q cairn
git -C "$work/cairn" fetch -q --depth 1 "$repo_url" "$commit"
git -C "$work/cairn" checkout -q FETCH_HEAD
head="$(git -C "$work/cairn" rev-parse HEAD)"
[ "$head" = "$commit" ] || { echo "build-engine: fetched $head, not $commit" >&2; exit 1; }

"$runtime" build -t "$tag" -f "$work/cairn/Dockerfile" "$work/cairn"
echo "build-engine: $tag from Cairn $commit"
