#!/usr/bin/env bash
#
# Read the Object Storage data-plane service log for kythira-ci-artifacts.
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
LOG_GROUP="${OCI_CI_OBJECT_LOG_GROUP_ID:-ocid1.loggroup.oc1.phx.amaaaaaaqu4kakiaomthhnnwv4kvwr2xdvzf3ti6wlbj6wd3szzbdmhr3m3q}"
LOG_READ="${OCI_CI_OBJECT_LOG_READ_ID:-ocid1.log.oc1.phx.amaaaaaaqu4kakiaqvxlhh4rrjii3a6k4kt6cdcz7wkdwh5z3ljitsg7mwcq}"
LOG_WRITE="${OCI_CI_OBJECT_LOG_WRITE_ID:-ocid1.log.oc1.phx.amaaaaaaqu4kakiaguujm2w3f4dqtx6zjzpwliq2a7j5imx5oqq7llbsfkoq}"

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
  out=$(oci logging-search search-logs \
          --time-start "$start" --time-end "$end" \
          --search-query "search \"${COMPARTMENT}/${LOG_GROUP}/${log}\"" \
          --limit 1000 2>&1) || {
    echo "search failed for ${log}:" >&2
    printf '%s\n' "$out" >&2
    continue
  }

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
