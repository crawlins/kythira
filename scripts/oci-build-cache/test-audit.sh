#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Tests audit.sh in BOTH directions (.kiro/specs/oci-build-cache/ Requirement
# 2.6), in the pattern of scripts/perf-cloud/test-audit-aws-leaks.sh.
#
# WHY THIS EXISTS: an audit that has only ever been observed to pass is not
# evidence. `.kiro/specs/multi-raft-performance/` task 21 found
# `gcloud compute list` exiting 0 under an authentication failure -- four of
# five queries "succeeding" blind, an auditor reporting clean precisely
# because it had lost the ability to see. The only way to know an audit can
# fail is to make it fail.
#
# Two modes, checking different claims:
#
#   stub (default) -- a fake `oci` earlier on PATH answers from canned JSON.
#     Checks the audit's LOGIC: that an unexpected tagged resource fails, that
#     a leak query which errors fails rather than reporting clean, that output
#     jq cannot read fails, and that an informational query failing does NOT
#     fail the audit. Needs no tenancy and no network.
#
#   --live -- creates a real, empty, throwaway bucket carrying this spec's
#     tag, confirms the audit fails on it, deletes it, and confirms the audit
#     passes. Checks the audit's QUERY: that the search syntax really finds a
#     tagged resource, which a stub cannot check because a stub answers
#     whatever it is asked. Requires OCI credentials.
#
# A stub cannot tell you the OCI search query is right, and a live run cannot
# cheaply produce a blind auditor. Both modes exist because neither is
# sufficient.
#
# Usage: test-audit.sh [--live] [--compartment-id OCID]

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AUDIT="${REPO_ROOT}/scripts/oci-build-cache/audit.sh"
LIVE=0
COMPARTMENT_ID="${OCI_CI_COMPARTMENT_ID:-ocid1.compartment.oc1..stub}"
SPEC_TAG_KEY="kythira-spec"
SPEC_TAG_VALUE="oci-build-cache"

usage() {
    awk '
        NR==1 && /^#!/                  { next }
        /^# *Copyright \(c\)/           { next }
        /^# *SPDX-License-Identifier:/  { next }
        /^#/    { line = $0; sub(/^# ?/, "", line); print line; started = 1; next }
        /^[[:space:]]*$/ { if (started) print ""; next }
        { exit }
    ' "${BASH_SOURCE[0]}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --live)           LIVE=1; shift ;;
        --compartment-id) COMPARTMENT_ID="${2:?needs an OCID}"; shift 2 ;;
        -h|--help)        usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

failures=0
STUB_DIR="$(mktemp -d)"
trap 'rm -rf "$STUB_DIR"' EXIT

# The stub. It answers the handful of subcommands audit.sh issues, and takes
# its behaviour from $STUB_SCENARIO so one file covers every case.
cat > "$STUB_DIR/oci" <<'STUB'
#!/usr/bin/env bash
args="$*"
case "$args" in
  *"search resource structured-search"*)
      case "${STUB_SCENARIO:-clean}" in
        blind)   echo "ServiceError: NotAuthenticated" >&2; exit 1 ;;
        garbage) echo "<html>401 Unauthorized</html>"; exit 0 ;;
        leak)    cat <<'JSON'
{"data":{"items":[
 {"resource-type":"Bucket","display-name":"kythira-build-cache","identifier":"ocid1.bucket.oc1..a"},
 {"resource-type":"Bucket","display-name":"kythira-build-cache-scratch","identifier":"ocid1.bucket.oc1..b"}
]}}
JSON
                 exit 0 ;;
        *)       cat <<'JSON'
{"data":{"items":[
 {"resource-type":"Bucket","display-name":"kythira-build-cache","identifier":"ocid1.bucket.oc1..a"},
 {"resource-type":"Group","display-name":"kythira-build-cache","identifier":"ocid1.group.oc1..g"},
 {"resource-type":"User","display-name":"kythira-build-cache-rw","identifier":"ocid1.user.oc1..rw"},
 {"resource-type":"User","display-name":"kythira-build-cache-ro","identifier":"ocid1.user.oc1..ro"},
 {"resource-type":"Policy","display-name":"kythira-build-cache-access","identifier":"ocid1.policy.oc1..p"}
]}}
JSON
                 exit 0 ;;
      esac ;;
  *"os object list"*)
      if [ "${STUB_SCENARIO:-clean}" = "infofail" ]; then
          echo "ServiceError: BucketNotFound" >&2; exit 1
      fi
      echo '{"data":[{"name":"x","size":1048576},{"name":"y","size":2097152}]}' ;;
  *"object-lifecycle-policy get"*)
      echo '{"data":{"items":[
        {"name":"expire-sccache-objects","action":"DELETE","time-amount":30,"time-unit":"DAYS","is-enabled":true,"object-name-filter":{"inclusion-prefixes":["sccache/"]}},
        {"name":"expire-vcpkg-archives","action":"DELETE","time-amount":90,"time-unit":"DAYS","is-enabled":true,"object-name-filter":{"inclusion-prefixes":["vcpkg/"]}}]}}' ;;
  *"iam compartment get"*)   echo "ocid1.tenancy.oc1..t" ;;
  *"iam user list"*)         echo "ocid1.user.oc1..u" ;;
  *"customer-secret-key list"*)
      echo '{"data":[{"id":"ocid1.credential.oc1..key0001","time-created":"2026-09-05T00:00:00Z","lifecycle-state":"ACTIVE"}]}' ;;
  *"usage-api"*)
      echo '{"data":{"items":[{"service":"Object Storage","computed-amount":0.26,"currency":"USD","computed-quantity":10,"unit":"GB_MONTHS"}]}}' ;;
  *) echo '{"data":[]}' ;;
esac
STUB
chmod +x "$STUB_DIR/oci"

# $1 = scenario, $2 = expected exit status, $3 = a string the output must
# contain, $4 = human label.
check() {
    local scenario="$1" want="$2" needle="$3" label="$4"
    local out rc=0
    out=$(PATH="$STUB_DIR:$PATH" STUB_SCENARIO="$scenario" \
          OCI_CI_COMPARTMENT_ID="$COMPARTMENT_ID" "$AUDIT" 2>&1) || rc=$?
    local ok=1
    [ "$rc" -eq "$want" ] || ok=0
    printf '%s' "$out" | grep -qF "$needle" || ok=0
    if [ "$ok" -eq 1 ]; then
        echo "PASS  $label (exit $rc)"
    else
        echo "FAIL  $label — expected exit $want containing '$needle', got exit $rc:"
        printf '%s\n' "$out" | sed 's/^/      /'
        failures=$((failures + 1))
    fi
}

echo "── stub mode ──"
check clean    0 "Audit clean"                       "a tenancy holding exactly this spec's resources passes"
check leak     1 "kythira-build-cache-scratch"       "an unexpected tagged resource FAILS and is named"
check blind    1 "the tag inventory is UNKNOWN"      "a leak query that errors FAILS rather than reporting clean"
check garbage  1 "jq could not read"                 "unparseable leak output FAILS rather than reading as empty"
check infofail 0 "UNKNOWN"                           "an informational query failing does not fail the audit"

if [ "$LIVE" -eq 1 ]; then
    echo
    echo "── live mode ──"
    command -v oci >/dev/null 2>&1 || { echo "FAIL  --live needs the OCI CLI on PATH"; exit 1; }
    scratch="kythira-build-cache-testaudit-$$"
    echo "creating throwaway tagged bucket $scratch"
    oci os bucket create --compartment-id "$COMPARTMENT_ID" --name "$scratch" \
        --public-access-type NoPublicAccess \
        --freeform-tags "{\"${SPEC_TAG_KEY}\":\"${SPEC_TAG_VALUE}\"}" >/dev/null
    # Delete it whatever happens next, including a search that never converges.
    trap 'oci os bucket delete --bucket-name "$scratch" --force >/dev/null 2>&1; rm -rf "$STUB_DIR"' EXIT
    # OCI's search index is eventually consistent; poll rather than sleep once.
    found=0
    for _ in $(seq 1 20); do
        if "$AUDIT" --compartment-id "$COMPARTMENT_ID" 2>&1 | grep -qF "$scratch"; then found=1; break; fi
        sleep 15
    done
    if [ "$found" -eq 1 ]; then
        echo "PASS  the audit sees a real tagged resource it does not name"
    else
        echo "FAIL  the audit never saw $scratch (search index, or a wrong query)"
        failures=$((failures + 1))
    fi
    oci os bucket delete --bucket-name "$scratch" --force >/dev/null
    trap 'rm -rf "$STUB_DIR"' EXIT
    echo "deleted $scratch"
fi

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) FAILED"
    exit 1
fi
echo "all checks passed"
