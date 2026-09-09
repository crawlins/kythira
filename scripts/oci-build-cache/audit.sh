#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Audits the OCI build-cache bucket and the IAM around it
# (.kiro/specs/oci-build-cache/ Requirement 2.6, design.md Component 1), in
# the pattern of scripts/perf-cloud/audit-aws-leaks.sh.
#
# It answers four questions:
#
#   1. What is in the bucket -- object count and bytes under vcpkg/ and
#      sccache/, which is the storage line on the bill and the input to
#      Requirement 6.5's cost check.
#   2. Are the two lifecycle rules still there. Without them the sccache
#      prefix grows without bound, and nothing else in this system would
#      ever notice.
#   3. Which customer secret keys exist and when they were created, so that
#      a key nobody remembers minting is visible and a rotation is dateable.
#   4. Does anything ELSE in this tenancy carry this spec's tag. That is the
#      leak check, and it is the only thing here that fails the script.
#
# WHY ONLY (4) FAILS. audit-aws-leaks.sh's doctrine is that a leak query which
# could not run reports UNKNOWN and fails, because an auditor that has lost
# the ability to see must never print "clean". That applies to the leak check
# and it is enforced below. It does not apply to (1) to (3): those are
# reports, not assertions, and a usage API that is briefly unavailable is not
# evidence of a leak. Their failures are printed as UNKNOWN, loudly, and do
# not change the exit status. The distinction is deliberate; do not "fix" it
# by making everything fatal, because then a flaky billing endpoint starts
# failing an audit whose actual subject is intact.
#
# Usage:
#   audit.sh [--bucket NAME] [--compartment-id OCID] [--region REGION]
#
#   --bucket          default: kythira-build-cache
#   --compartment-id  default: $OCI_CI_COMPARTMENT_ID
#   --region          default: $OCI_CI_REGION
#
# Exit status: 0 clean, 1 a leak or an unreadable leak query, 2 bad usage.

set -uo pipefail

BUCKET="kythira-build-cache"
COMPARTMENT_ID="${OCI_CI_COMPARTMENT_ID:-}"
REGION="${OCI_CI_REGION:-}"

GROUP_NAME="kythira-build-cache"
RW_USER="kythira-build-cache-rw"
RO_USER="kythira-build-cache-ro"
POLICY_NAME="kythira-build-cache-access"
SPEC_TAG_KEY="kythira-spec"
SPEC_TAG_VALUE="oci-build-cache"

usage() {
    # Content-matched, not a line range: see provision.sh for why.
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
        --bucket)         BUCKET="${2:?--bucket needs a name}"; shift 2 ;;
        --compartment-id) COMPARTMENT_ID="${2:?--compartment-id needs an OCID}"; shift 2 ;;
        --region)         REGION="${2:?--region needs a region}"; shift 2 ;;
        -h|--help)        usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v oci >/dev/null 2>&1 || { echo "error: the OCI CLI is not on PATH" >&2; exit 1; }
command -v jq  >/dev/null 2>&1 || { echo "error: jq is not on PATH" >&2; exit 1; }
[ -n "$COMPARTMENT_ID" ] || { echo "error: --compartment-id or \$OCI_CI_COMPARTMENT_ID is required" >&2; exit 2; }

# Informational queries that could not run. A file rather than a variable
# because `try` always runs in a command substitution; see its comment.
UNKNOWN_TALLY=$(mktemp)
trap 'rm -f "$UNKNOWN_TALLY"' EXIT
leaked=0    # resources tagged for this spec that this spec does not name
blind=0     # leak queries that could not run — as bad as a leak

# Runs a read-only query. $1 = label, rest = command. Prints the raw output on
# success; on failure prints the provider's own message and counts an UNKNOWN.
try() {
    local label="$1"; shift
    local out rc=0 err
    err=$(mktemp)
    # stdin closed: an unconfigured CLI prompts ("config file not found — do
    # you want to create one?") instead of failing, and an audit that blocks
    # on an invisible question reports nothing at all.
    #
    # stderr goes to a FILE, never into $out. It used to be folded in with
    # `2>&1`, which meant a command that exited 0 while printing a diagnostic
    # returned that diagnostic AS ITS VALUE. `oci iam compartment get` on the
    # tenancy root does exactly that -- it succeeds and writes "Query returned
    # empty result, no output to show." to stderr -- so `tenancy_id` became
    # that sentence, the user lookup was handed it as an OCID, and the audit
    # printed "kythira-build-cache-rw: does not exist" about the very
    # credential CI was authenticating with at that moment. A false negative
    # about a credential is worse than no answer at all.
    out=$("$@" 2>"$err" </dev/null) || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  UNKNOWN: $label could not be read (exit $rc):" >&2
        sed 's/^/    /' "$err" >&2
        # Counted through a FILE, not a variable. Every call site is
        # `$(try ...)`, a subshell, so `unknown=$((unknown + 1))` incremented
        # a copy and the parent's total stayed at zero forever -- the summary
        # that reports how much the audit could not see could never fire.
        echo x >> "$UNKNOWN_TALLY"
        rm -f "$err"
        return 1
    fi
    rm -f "$err"
    printf '%s' "$out"
}

echo "OCI build-cache audit — bucket $BUCKET, compartment $COMPARTMENT_ID${REGION:+, region $REGION}"
echo

# ── 1. Contents, per prefix ──────────────────────────────────────────────────
echo "1. Bucket contents"
for prefix in vcpkg/x64-linux/ vcpkg/arm64-linux/ sccache/; do
    if out=$(try "objects under $prefix" oci os object list --bucket-name "$BUCKET" \
                 --prefix "$prefix" --fields name,size --all); then
        printf '%s' "$out" | jq -r --arg p "$prefix" '
            (.data // []) as $d
            | "  \($p): \($d | length) objects, \(($d | map(.size // 0) | add // 0) / 1048576 | floor) MiB"'
    fi
done
echo

# ── 2. Lifecycle ─────────────────────────────────────────────────────────────
# Printed as the rule set rather than checked against an expectation: the
# check that matters is a human reading "30" and "90" here, and a script that
# asserted them would have to be edited in the same commit that changed them,
# which makes the assertion circular.
echo "2. Lifecycle rules"
if out=$(try "lifecycle policy" oci os object-lifecycle-policy get --bucket-name "$BUCKET"); then
    printf '%s' "$out" | jq -r '
        (.data.items // [])
        | if length == 0 then "  NONE — the sccache prefix will grow without bound"
          else .[] | "  \(.name): \(.action) objects under \(."object-name-filter"."inclusion-prefixes" // [] | join(", ")) after \(."time-amount") \(."time-unit" | ascii_downcase) (enabled: \(."is-enabled"))"
          end'
fi
echo

# ── 3. Customer secret keys ──────────────────────────────────────────────────
# The secret itself is unreadable after creation, by design. What is readable
# is that a key exists and when it was made, which is what a rotation needs.
echo "3. Customer secret keys"
# Users live in the tenancy root, so this needs the tenancy OCID. If the
# caller already passed one -- and they will, because the bucket itself lives
# in the root and that is what `--compartment-id` gets pointed at -- then
# asking for its PARENT is both wrong and silently destructive: the root has
# no parent, the CLI exits 0 with an empty result, and whatever comes back
# gets used as an id. Take the OCID as given when it is already a tenancy.
if [[ "$COMPARTMENT_ID" == ocid1.tenancy.* ]]; then
    tenancy_id="$COMPARTMENT_ID"
else
    tenancy_id=$(try "tenancy id" oci iam compartment get --compartment-id "$COMPARTMENT_ID" \
                     --query 'data."compartment-id"' --raw-output) || tenancy_id=""
fi
# Whatever it is, it has to LOOK like a tenancy OCID before being used as one.
# This is the guard that would have caught the original bug on its own: a
# diagnostic sentence is not an OCID, and refusing to pass it to the next call
# turns a confident wrong answer into an honest UNKNOWN.
if [ -n "$tenancy_id" ] && [[ "$tenancy_id" != ocid1.tenancy.* ]]; then
    echo "  UNKNOWN: tenancy id did not look like an OCID (got: ${tenancy_id:0:60})" >&2
    echo x >> "$UNKNOWN_TALLY"
    tenancy_id=""
fi
for user in "$RW_USER" "$RO_USER"; do
    if [ -z "$tenancy_id" ]; then
        echo "  UNKNOWN: $user (tenancy id unavailable)"
        continue
    fi
    # Through `try`, so that a lookup which FAILS is reported as UNKNOWN
    # rather than as absence. "does not exist" is an assertion about the
    # tenancy; it may only be made when the query actually ran and came back
    # empty. Conflating the two is how an audit reports a missing credential
    # that is in active use.
    if ! uid=$(try "user $user" oci iam user list --compartment-id "$tenancy_id" \
                   --name "$user" --query 'data[0].id' --raw-output); then
        echo "  UNKNOWN: $user (lookup failed)"
        continue
    fi
    if [ -z "$uid" ] || [ "$uid" = "null" ]; then
        echo "  $user: does not exist"
        continue
    fi
    if out=$(try "customer secret keys for $user" oci iam customer-secret-key list --user-id "$uid"); then
        printf '%s' "$out" | jq -r --arg u "$user" '
            (.data // []) as $d
            | if ($d | length) == 0 then "  \($u): no key"
              else $d[] | "  \($u): \(.id[0:12])… created \(."time-created") state \(."lifecycle-state")"
              end'
    fi
done
echo

# ── 4. Cost inputs ───────────────────────────────────────────────────────────
# Month-to-date, so that Requirement 6.5's "under $5, against $0.50
# pre-registered" is checkable before the bill rather than after it.
echo "4. Month-to-date usage (Object Storage, this tenancy)"
if [ -n "$tenancy_id" ]; then
    # Both bounds must be midnight UTC exactly. The Usage API rejects anything
    # finer with "Passed UTC date does not have the right precision: hours,
    # minutes, seconds, and second fractions must be 0", so the end bound is
    # tomorrow's date rather than the current instant — asking for "up to now"
    # is what fails.
    month_start=$(date -u +%Y-%m-01T00:00:00Z)
    now=$(date -u -d 'tomorrow' +%Y-%m-%dT00:00:00Z 2>/dev/null || date -u -v+1d +%Y-%m-%dT00:00:00Z)
    if out=$(try "usage summary" oci usage-api usage-summary request-summarized-usages \
                 --tenant-id "$tenancy_id" --granularity MONTHLY --query-type COST \
                 --time-usage-started "$month_start" --time-usage-ended "$now"); then
        printf '%s' "$out" | jq -r '
            (.data.items // [])
            | map(select((.service // "") | test("Object Storage"; "i")))
            | if length == 0 then "  no Object Storage line yet this month"
              else .[] | "  \(.service): \(."computed-amount" // 0) \(.currency // "") (\(."computed-quantity" // 0) \(.unit // ""))"
              end'
    fi
else
    echo "  UNKNOWN: tenancy id unavailable"
fi
echo

# ── 5. The leak check ────────────────────────────────────────────────────────
# The only failing check. Everything this spec creates carries
# kythira-spec=oci-build-cache; anything else wearing that tag was created by
# a run of provision.sh that was interrupted, by an experiment, or by someone
# copying the tag — and in each case it is billing and nobody is watching it.
echo "5. Resources tagged $SPEC_TAG_KEY=$SPEC_TAG_VALUE"
expected="$BUCKET|$GROUP_NAME|$RW_USER|$RO_USER|$POLICY_NAME"
search_rc=0
# `|| search_rc=$?` rather than testing $? on the next line: the assignment
# would otherwise be the tested command under any future `set -e`, and the
# intent — "run it, remember whether it worked" — reads better here anyway.
search_out=$(oci search resource structured-search --query-text \
    "query all resources where (freeformTags.key = '${SPEC_TAG_KEY}' && freeformTags.value = '${SPEC_TAG_VALUE}')" 2>&1 </dev/null) || search_rc=$?
if [ "$search_rc" -ne 0 ]; then
    echo "::error::The leak query FAILED — the tag inventory is UNKNOWN, not clean:" >&2
    printf '%s\n' "$search_out" | sed 's/^/    /' >&2
    blind=1
else
    jq_rc=0
    rows=$(printf '%s' "$search_out" | jq -r '(.data.items // [])[] | "\(."resource-type")\t\(."display-name")\t\(.identifier)"' 2>/dev/null) || jq_rc=$?
    if [ "$jq_rc" -ne 0 ]; then
        # The query exited 0 and produced something jq cannot read. Counting
        # that as an empty inventory is the precise failure audit-aws-leaks.sh
        # was written to avoid: output that cannot be parsed is not evidence
        # of an empty tenancy.
        echo "::error::The leak query returned output jq could not read — the tag inventory is UNKNOWN:" >&2
        printf '%s\n' "$search_out" | head -20 | sed 's/^/    /' >&2
        blind=1
    elif [ -z "$rows" ]; then
        # Not necessarily wrong — an empty inventory is what a tenancy looks
        # like before provision.sh has ever run — but it is also what a
        # search index that has not caught up looks like, so say both.
        echo "  nothing found (either not provisioned yet, or the search index is behind)"
    else
        while IFS=$'\t' read -r rtype name ocid; do
            if printf '%s' "$name" | grep -qE "^($expected)$"; then
                echo "  expected: $rtype $name"
            else
                echo "::error::Unexpected resource carrying this spec's tag: $rtype $name ($ocid)" >&2
                leaked=$((leaked + 1))
            fi
        done <<< "$rows"
    fi
fi
echo

unknown=$(wc -l < "$UNKNOWN_TALLY" | tr -d ' ')
if [ "$unknown" -ne 0 ]; then
    echo "$unknown informational query/queries could not be read; those lines are UNKNOWN above." >&2
fi
if [ "$blind" -ne 0 ]; then
    echo "::error::Audit FAILED: the leak query could not run. Treat the tenancy as leaking until checked by hand." >&2
    exit 1
fi
if [ "$leaked" -ne 0 ]; then
    echo "::error::Audit FAILED: $leaked resource(s) carry $SPEC_TAG_KEY=$SPEC_TAG_VALUE and are not part of this spec." >&2
    echo "  They are billing now. Delete them, or add them to this script if they belong." >&2
    exit 1
fi
echo "Audit clean: everything tagged $SPEC_TAG_KEY=$SPEC_TAG_VALUE is named by this spec."
