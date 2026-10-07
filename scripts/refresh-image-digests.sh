#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Re-resolves every Docker Hub image pinned as `name:tag@sha256:<digest>` in
# the Dockerfiles, compose files and the few test/CI files that pull one
# directly, and rewrites the digest to what the tag points at now.
#
# Upstream images are pinned by digest so a re-pushed tag cannot change what
# a fixture or builder stage runs. The tag stays in the reference so a reader
# sees the version, and so this script knows what to re-resolve. The digest
# is the multi-arch index, so x86_64 and arm64 hosts both resolve it.
#
# Usage: scripts/refresh-image-digests.sh [--check]
#   --check  print stale pins and exit 1 if any, change nothing
#
# Needs curl and jq; talks to Docker Hub anonymously (rate-limited, so the
# script sleeps between lookups and retries a refused one).

set -euo pipefail

check=0
[[ ${1:-} == --check ]] && check=1

cd "$(dirname "$0")/.."

files=$(git ls-files 'docker/**/Dockerfile' 'docker/*.yml' \
    tests/docker_chaos/cloud_monitoring_config_validation_test.cpp .github/workflows/ci.yml)

resolve() {
    local repo=$1 tag=$2 token
    [[ $repo == */* ]] || repo=library/$repo
    token=$(curl -fsS "https://auth.docker.io/token?service=registry.docker.io&scope=repository:${repo}:pull" |
        jq -r .token)
    curl -fsSI -H "Authorization: Bearer $token" \
        -H "Accept: application/vnd.oci.image.index.v1+json" \
        -H "Accept: application/vnd.docker.distribution.manifest.list.v2+json" \
        -H "Accept: application/vnd.docker.distribution.manifest.v2+json" \
        "https://registry-1.docker.io/v2/${repo}/manifests/${tag}" |
        tr -d '\r' | awk 'tolower($1) == "docker-content-digest:" { print $2 }'
}

stale=0
# shellcheck disable=SC2086
refs=$(grep -ohE '[a-z0-9./-]+:[A-Za-z0-9._-]+@sha256:[0-9a-f]{64}' $files | sort -u)
for ref in $refs; do
    name_tag=${ref%@*}
    old=${ref#*@}
    name=${name_tag%:*}
    tag=${name_tag##*:}
    repo=${name#docker.io/}
    repo=${repo#library/}
    new=
    for delay in 5 15 30 60; do
        new=$(resolve "$repo" "$tag" 2>/dev/null) && [[ -n $new ]] && break
        sleep "$delay"
    done
    if [[ -z $new ]]; then
        echo "cannot resolve $name_tag" >&2
        exit 2
    fi
    if [[ $new != "$old" ]]; then
        stale=1
        echo "$name_tag: $old -> $new"
        if [[ $check -eq 0 ]]; then
            # shellcheck disable=SC2086
            sed -i "s|${name_tag}@${old}|${name_tag}@${new}|g" $files
        fi
    fi
    sleep 2
done

if [[ $check -eq 1 && $stale -eq 1 ]]; then
    exit 1
fi
