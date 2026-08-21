#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Read the Object Storage data-plane service log for kythira-ci-artifacts.
#
# The OCIDs below are the ones enable-object-storage-logging.sh creates (log
# group kythira-ci-logs). If that group is ever recreated they change; override
# with OCI_CI_OBJECT_LOG_GROUP_ID / _READ_ID / _WRITE_ID rather than editing.
#
# This is the tool the `404 BucketNotFound` flake is investigated with. It is
# NOT the audit log: Object Storage data-plane operations are not audited by
# default, so `oci audit event list` over a failure window returns nothing.
# That is why the "take an opc-request-id to the audit log" step this spec
# carried for weeks was never performable (spike-notes.md Finding 24).
#
# Usage:
#   read-object-storage-log.sh <start-rfc3339> <end-rfc3339> [read|write|both]
#
# Example — the window of one CI job:
#   read-object-storage-log.sh 2026-08-21T03:54:00Z 2026-08-21T03:56:00Z
#
# Reading the output:
#   1. Find the 404 and a NEARBY 200 with the SAME requestResourcePath. A
#      byte-identical pair means the client is not at fault.
#   2. A populated bucketId on the declined row means the bucket resolved and
#      the 404 is an authorization decline wearing a not-found status.
#   3. `--raw` dumps the full JSON records for a field-by-field diff.
#
# TWO TRAPS, both of which produced a wrong answer first (Finding 24):
#
#   * INGESTION LAG. A query run minutes after the event can return the
#     successes and not the decline. A short result is NOT evidence of
#     absence — re-query before concluding an entry is missing.
#   * THE SEARCH SCOPE NEEDS ALL THREE PARTS, <compartment>/<logGroup>/<log>.
#     One mistake gives three unrelated errors: the two-part form answers
#     `400 InvalidParameter — "No log sources found to be read"`, and scoping
#     to the tenancy answers `401 NotAuthenticated`. Neither says "name the
#     log". This script always passes all three.

set -euo pipefail

COMPARTMENT="${OCI_CI_COMPARTMENT_ID:-ocid1.compartment.oc1..aaaaaaaarzymnowy6xsbe5sj5z7evtpigcomd2vm5nwayhkzcjckkalau5tq}"
LOG_GROUP="${OCI_CI_OBJECT_LOG_GROUP_ID:-ocid1.loggroup.oc1.phx.amaaaaaaqu4kakiagl5e7rhyfsoshn2lbe7y5ldtijjjvlxdywya52poa7eq}"
LOG_READ="${OCI_CI_OBJECT_LOG_READ_ID:-ocid1.log.oc1.phx.amaaaaaaqu4kakia23asznnqvrn3c6q7pcl5srpdig5q6q5xa7smffxafcdq}"
LOG_WRITE="${OCI_CI_OBJECT_LOG_WRITE_ID:-ocid1.log.oc1.phx.amaaaaaaqu4kakiamdy53sam2gytixrgdc3csd7fli5sxxta3562k2m5kwba}"

raw=0
args=()
for a in "$@"; do
  if [ "$a" = "--raw" ]; then raw=1; else args+=("$a"); fi
done
set -- "${args[@]:-}"

if [ $# -lt 2 ]; then
  sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi

start="$1"; end="$2"; which="${3:-both}"

logs=()
case "$which" in
  read)  logs=("$LOG_READ") ;;
  write) logs=("$LOG_WRITE") ;;
  both)  logs=("$LOG_READ" "$LOG_WRITE") ;;
  *) echo "third argument must be read, write or both" >&2; exit 2 ;;
esac

for log in "${logs[@]}"; do
  # RETRY, and verify the payload rather than the exit code. `logging-search`
  # itself declines intermittently in this tenancy (~5 of 30 observed, then 0
  # of 10 — see spike-notes Finding 24), and a declined query returns a
  # ServiceError where a caller redirecting stderr sees an EMPTY RESULT rather
  # than a failure. That silently under-counts, which is the single most
  # dangerous failure mode for a script whose whole job is counting: it
  # produced a 6x undercount before this loop existed. Never treat a short
  # result as a small result.
  out=""
  for attempt in 1 2 3 4 5 6; do
    if out=$(oci logging-search search-logs \
               --time-start "$start" --time-end "$end" \
               --search-query "search \"${COMPARTMENT}/${LOG_GROUP}/${log}\"" \
               --limit 1000 2>&1) && [ "${out#\{}" != "$out" ]; then
      break
    fi
    out=""
    sleep 4
  done
  if [ -z "$out" ]; then
    echo "SEARCH FAILED after 6 attempts for ${log} over ${start}..${end}." >&2
    echo "DO NOT treat this as zero entries — the window was not read." >&2
    exit 1
  fi

  # A full page means the window may be truncated; the caller must narrow it.
  if printf '%s' "$out" | python3 -c 'import json,sys; sys.exit(0 if len(json.load(sys.stdin)["data"]["results"])<1000 else 1)'; then :; else
    echo "WARNING: 1000-record page limit hit for ${log} over ${start}..${end}." >&2
    echo "         Counts from this window are TRUNCATED — re-run in shorter slices." >&2
  fi

  if [ "$raw" = 1 ]; then
    printf '%s\n' "$out"
    continue
  fi

  printf '%s\n' "$out" | python3 -c '
import json, sys
try:
    results = json.load(sys.stdin)["data"]["results"]
except Exception:
    print("  (no parseable results — re-run; ingestion lags by up to a few minutes)")
    sys.exit()
rows = [r["data"]["logContent"]["data"] for r in results]
rows.sort(key=lambda c: c.get("startTime", ""))
print(f"  {len(rows)} entries")
for c in rows:
    status = c.get("statusCode")
    flag = " <-- DECLINED" if status and int(status) >= 400 else ""
    print("   {:<26} {:>4} {:<7} {:<44} {}{}".format(
        c.get("startTime", ""), status, c.get("requestAction", ""),
        (c.get("message") or "")[:44], (c.get("requestResourcePath") or "")[:70], flag))
    if flag:
        print("       opcRequestId: {}".format(c.get("opcRequestId")))
        print("       principalId : {}  ({})".format(c.get("principalId"), c.get("principalName")))
        print("       bucketId    : {}".format(c.get("bucketId") or "<absent>"))
        print("       ^ populated bucketId means the bucket RESOLVED: this is an")
        print("         authorization decline wearing a not-found status.")
'
done
