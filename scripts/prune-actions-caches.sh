#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Prune GitHub Actions caches that can no longer be restored by anything.
#
# ACCOUNTING, re-measured September 9, 2026 after the OCI build cache landed
# (.kiro/specs/oci-build-cache/ task 8). The rules below are unchanged; what
# changed is what they have left to do.
#
#                        Aug 2026     8 Sep (before)   9 Sep (after)
#     total              10.37 GB     10.00 GiB / 12    8.67 GiB / 11
#     vcpkg trees         4.49 GB      9.40 GiB /  7    8.17 GiB /  6
#     ccache families        —         0.60 GiB /  5    0.50 GiB /  5
#
# Read that carefully, because the obvious reading is wrong twice over.
#
# FIRST: moving the compiler cache to OCI did not relieve the ceiling. ccache
# was 0.60 GiB, not the "eight families at 2 GB each" the spec's introduction
# assumed; it was small precisely BECAUSE it was being evicted constantly. Most
# of the 1.33 GiB drop above is one vcpkg tree entry expiring, not the move.
# The dominant occupant is, and remains, the vcpkg_installed/ tree caches --
# which .kiro/specs/oci-build-cache/ Requirement 3.3 deliberately KEEPS as the
# L1 in front of the OCI binary cache.
#
# SECOND: the ccache entries above still exist even though nothing writes them
# any more. GitHub keeps a cache until it is evicted or goes unused for its
# retention window, so they age out rather than disappear. A reading taken
# within a week of the move is transitional by construction.
#
# What actually changed is that EVICTION STOPPED MATTERING. A tree-cache miss
# used to cost a full dependency rebuild -- 69 to 163 minutes, measured across
# nine legs -- and now costs a 4 to 7 minute download from OCI. The ceiling is
# still there and the repository still sits near it; being evicted is simply no
# longer expensive. That is a better outcome than the one originally argued
# for, and it is a different one.
#
# The accumulation problem this script was written for is now mostly historical.
# Every ccache key in ci.yml ended in `-${{ github.ref_name }}-${{ github.run_id }}`,
# so every run minted a brand new entry and nothing removed the one it
# superseded; only the newest generation per family was reachable, making every
# older generation dead weight (measured: four retained generations per family,
# of which one was useful). Those keys are gone from ci.yml. The rules stay
# because the pattern can return -- any future actions/cache key carrying a run
# id recreates it -- and because --closed-prs is about refs, not families.
#
# Two rules, both deliberately conservative:
#
#   --closed-prs   delete every cache belonging to a closed/merged pull
#                  request. A merged PR's ref can never be restored from again.
#
#   --superseded   within each (ref, key-family), keep the newest generation
#                  and delete the rest. Family = the key with its trailing
#                  `-<ref>-<run_id>` stripped.
#
# vcpkg caches are deliberately NEVER touched by either rule. Their keys are
# content-addressed by hashFiles(vcpkg.json, vcpkg-overlays/**) rather than by
# run id, so each one is the only entry for its inputs and deleting it forces
# an expensive cold dependency rebuild. They are 8.17 GiB of the 8.67 GiB total
# as of September 9, 2026 -- 94% of it -- and every one of them is currently
# live. Deleting one is no longer catastrophic now that the OCI binary cache
# backs it (the rebuild becomes a download), but it is still waste, so the rule
# stands.
#
# Usage:
#   scripts/prune-actions-caches.sh [--repo OWNER/REPO] [--closed-prs] [--superseded] [--apply]
#
#   --apply   actually delete. WITHOUT IT THIS SCRIPT ONLY REPORTS, which is
#             the default precisely because the selection logic is the part
#             worth reviewing before anything is destroyed.
#
# Requires: gh (authenticated), python3. Needs `actions: write` in CI.

set -euo pipefail

REPO="${GITHUB_REPOSITORY:-crawlins/kythira}"
DO_CLOSED=0
DO_SUPERSEDED=0
APPLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo)       REPO="$2"; shift 2 ;;
        --closed-prs) DO_CLOSED=1; shift ;;
        --superseded) DO_SUPERSEDED=1; shift ;;
        --apply)      APPLY=1; shift ;;
        *) echo "[prune-caches] unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ "$DO_CLOSED" -eq 0 && "$DO_SUPERSEDED" -eq 0 ]]; then
    echo "[prune-caches] nothing to do: pass --closed-prs and/or --superseded" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "[prune-caches] repo: $REPO"
gh api --paginate "repos/${REPO}/actions/caches?per_page=100" \
    -q '.actions_caches[] | [.id, .size_in_bytes, .key, .ref, .created_at] | @tsv' \
    > "$WORK/caches.tsv"

# Open PR numbers, so "closed" is derived from the API rather than assumed.
gh pr list --repo "$REPO" --state open --json number -q '.[].number' > "$WORK/open-prs.txt" || true

python3 - "$WORK/caches.tsv" "$WORK/open-prs.txt" "$DO_CLOSED" "$DO_SUPERSEDED" > "$WORK/doomed.tsv" <<'PY'
import sys, re, collections

caches_path, open_path, do_closed, do_superseded = sys.argv[1:5]
do_closed, do_superseded = int(do_closed), int(do_superseded)

rows = []
for line in open(caches_path):
    line = line.rstrip("\n")
    if not line:
        continue
    cid, size, key, ref, created = line.split("\t")
    rows.append((int(cid), int(size), key, ref, created))

open_prs = {l.strip() for l in open(open_path) if l.strip()}

# vcpkg entries are content-addressed, not generational -- never a candidate.
def protected(key):
    return key.startswith("vcpkg")

doomed = {}   # id -> (size, key, ref, reason)

if do_closed:
    for cid, size, key, ref, created in rows:
        m = re.match(r"^refs/pull/(\d+)/merge$", ref)
        if m and m.group(1) not in open_prs and not protected(key):
            doomed[cid] = (size, key, ref, f"PR #{m.group(1)} is closed")

if do_superseded:
    fam = collections.defaultdict(list)
    for cid, size, key, ref, created in rows:
        if protected(key):
            continue
        # ccache keys end with -<ref_name>-<run_id>; ref_name may contain '/'
        m = re.match(r"^(.*)-([\w./+-]+)-(\d{9,})$", key)
        if not m:
            continue
        fam[(ref, m.group(1))].append((created, cid, size, key))
    for (ref, family), v in fam.items():
        v.sort(reverse=True)               # newest generation first
        for created, cid, size, key in v[1:]:
            doomed.setdefault(cid, (size, key, ref, f"superseded in {family}"))

for cid, (size, key, ref, reason) in sorted(doomed.items(), key=lambda kv: -kv[1][0]):
    print(f"{cid}\t{size}\t{key}\t{ref}\t{reason}")
PY

TOTAL_BYTES=$(awk -F'\t' '{s+=$2} END {print s+0}' "$WORK/caches.tsv")
DOOMED_BYTES=$(awk -F'\t' '{s+=$2} END {print s+0}' "$WORK/doomed.tsv")
N_ALL=$(wc -l < "$WORK/caches.tsv")
N_DOOMED=$(wc -l < "$WORK/doomed.tsv")

awk -F'\t' '{ printf "  %8.2f MB  %-58s  %s\n", $2/1048576, $3, $5 }' "$WORK/doomed.tsv"

python3 - "$TOTAL_BYTES" "$DOOMED_BYTES" "$N_ALL" "$N_DOOMED" <<'PY'
import sys
G = 1073741824
tot, doomed, n_all, n_doomed = (int(x) for x in sys.argv[1:5])
print(f"\n[prune-caches] {n_all} caches, {tot/G:.2f} GB total")
print(f"[prune-caches] {n_doomed} unreachable, {doomed/G:.2f} GB")
print(f"[prune-caches] after pruning: {(tot-doomed)/G:.2f} GB against a 10.00 GB ceiling")
PY

if [[ "$N_DOOMED" -eq 0 ]]; then
    echo "[prune-caches] nothing to delete"
    exit 0
fi

if [[ "$APPLY" -ne 1 ]]; then
    echo "[prune-caches] DRY RUN — nothing deleted. Re-run with --apply to delete."
    exit 0
fi

FAILED=0
while IFS=$'\t' read -r cid size key ref reason; do
    if gh api -X DELETE "repos/${REPO}/actions/caches/${cid}" >/dev/null 2>&1; then
        echo "[prune-caches] deleted $key ($ref)"
    else
        # A cache GitHub already evicted between the list and the delete is not
        # an error -- the desired state was reached either way.
        echo "::warning::could not delete cache $cid ($key) — it may already be gone"
        FAILED=$((FAILED + 1))
    fi
done < "$WORK/doomed.tsv"

echo "[prune-caches] done; $((N_DOOMED - FAILED))/${N_DOOMED} deleted"
