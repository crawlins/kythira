#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Read-only live check of raft_object_backup against a real bucket
# (.kiro/specs/object-backup-oci-oss-credentials task 9).
#
# Usage: raft-object-backup-live-check.sh <binary> <provider> <bucket>
#
# Runs `list` under a destination prefix unique to this CI run, so the
# expected answer is "no finished backups" and nothing is written. What it
# proves is that the provider's credentials, taken from the KYTHIRA_*
# environment the real suites already export, authenticate a real request
# through the tool itself: before the fix, OSS failed every request with
# "access_key_id is empty" and OCI died by SIGABRT.
#
# Exit status 1 from the tool is a configuration error, which retrying cannot
# fix. Exit status 2 is retried twice, because the OCI CI tenancy
# intermittently declines valid requests with 404 BucketNotFound
# (oci_object_storage_client.hpp); a third failure is real.

set -uo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <binary> <provider> <bucket>" >&2
    exit 64
fi
binary=$1
provider=$2
bucket=$3
prefix="kythira-backup-live-check/${GITHUB_RUN_ID:-local}-${provider}"

for attempt in 1 2 3; do
    rc=0
    out=$("$binary" list --provider "$provider" --dest-bucket "$bucket" \
        --dest-prefix "$prefix" 2>&1) || rc=$?
    printf '%s\n' "$out"
    if [ $rc -eq 0 ]; then
        if printf '%s' "$out" | grep -q "no finished backups"; then
            echo "raft_object_backup list --provider $provider succeeded (attempt $attempt)"
            exit 0
        fi
        echo "::error::raft_object_backup exited 0 but did not report an empty listing under a fresh prefix." >&2
        exit 1
    fi
    if [ $rc -ne 2 ]; then
        echo "::error::raft_object_backup list --provider $provider exited $rc (a configuration error or a crash); see the output above." >&2
        exit 1
    fi
    echo "attempt $attempt exited 2" >&2
    sleep 5
done
echo "::error::raft_object_backup list --provider $provider failed three times." >&2
exit 1
