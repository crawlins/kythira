#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Tests audit-aws-leaks.sh in BOTH directions
# (.kiro/specs/multi-machine-placement/ task 1, Requirements 5.5 and 5.6).
#
# WHY THIS EXISTS: an audit that has only ever been observed to pass is not
# evidence. `.kiro/specs/multi-raft-performance/` task 21 found
# `gcloud compute list` exiting 0 under an authentication failure, with four of
# five queries "succeeding" blind -- an auditor reporting clean precisely
# because it had lost the ability to see anything. The only way to know an
# audit can fail is to make it fail.
#
# Two modes, and they check different claims:
#
#   stub (default) -- a fake `aws` earlier on PATH returns canned responses.
#     Checks the audit's LOGIC: that a non-empty result fails, that a query
#     which errors fails rather than reporting clean, and that output jq
#     cannot read fails. Needs no credentials and no network, so it runs in
#     CI on every change.
#
#   --live -- creates real AWS resources, tagged with a throwaway run tag,
#     and confirms the audit finds them. Checks the audit's QUERIES: that the
#     tag filter syntax is right for each resource class and that the caller
#     holds the IAM action. A stub cannot check either, because a stub answers
#     whatever the script asks.
#
# The live mode is deliberately restricted to the three resource classes AWS
# does not charge for -- security groups, key pairs and placement groups. They
# cost nothing to create and nothing to leave lying around for the seconds this
# takes, so the test that proves the auditor works does not itself need a
# budget. The three billed classes (instances, volumes, elastic IPs) are
# covered by the stub mode's logic check and by Shape 2's own runs.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AUDIT="${REPO_ROOT}/scripts/perf-cloud/audit-aws-leaks.sh"

LIVE=0
REGION="${AWS_REGION:-us-east-1}"
TAG_KEY="kythira-perf-run"

usage() {
    cat <<'USAGE'
Usage: test-audit-aws-leaks.sh [--live] [--region REGION]

  --live          Additionally create real (free) AWS resources and confirm
                  the audit sees them. Requires credentials.
  --region        default: $AWS_REGION or us-east-1
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --live) LIVE=1; shift ;;
        --region) REGION="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

passed=0
failed=0

# $1 = what is being asserted, $2 = expected exit status, $3... = command
expect_status() {
    local what="$1" want="$2"; shift 2
    local out rc=0
    out=$("$@" 2>&1) || rc=$?
    if [[ "${rc}" -eq "${want}" ]]; then
        echo "  PASS: ${what} (exit ${rc})"
        passed=$((passed + 1))
    else
        echo "  FAIL: ${what} — wanted exit ${want}, got ${rc}" >&2
        printf '%s\n' "${out}" | sed 's/^/        /' >&2
        failed=$((failed + 1))
    fi
}

# Like expect_status, but also requires the output to mention $4.
# An audit that fails for the WRONG reason is not the same as one that
# detected the leak, and only the message distinguishes them.
expect_status_saying() {
    local what="$1" want="$2" needle="$3"; shift 3
    local out rc=0
    out=$("$@" 2>&1) || rc=$?
    if [[ "${rc}" -eq "${want}" ]] && printf '%s' "${out}" | grep -qF -- "${needle}"; then
        echo "  PASS: ${what} (exit ${rc}, said '${needle}')"
        passed=$((passed + 1))
    else
        echo "  FAIL: ${what} — wanted exit ${want} mentioning '${needle}', got ${rc}" >&2
        printf '%s\n' "${out}" | sed 's/^/        /' >&2
        failed=$((failed + 1))
    fi
}

# ── Stub mode ────────────────────────────────────────────────────────────────
#
# The stub reads two variables the harness exports:
#   STUB_LEAK_CLASS  -- which describe-* subcommand returns a non-empty list
#   STUB_FAIL_CLASS  -- which one exits non-zero, as an AccessDenied would
#   STUB_GARBAGE_CLASS -- which one prints something jq cannot parse
#
# Everything else returns `[]`. Keying off the subcommand rather than a call
# counter matters: the audit is free to reorder its six queries, and a
# positional stub would then silently test the wrong class.

STUB_DIR=$(mktemp -d "${TMPDIR:-/tmp}/kythira-audit-stub-XXXXXX")
cat > "${STUB_DIR}/aws" <<'STUB'
#!/usr/bin/env bash
# Fake `aws` for test-audit-aws-leaks.sh. Answers only the describe-* calls
# audit-aws-leaks.sh makes; anything else is a test bug and says so loudly.
sub=""
for arg in "$@"; do
    case "$arg" in
        describe-*) sub="$arg"; break ;;
    esac
done
if [[ -z "$sub" ]]; then
    echo "stub aws: no describe-* subcommand in: $*" >&2
    exit 90
fi
if [[ "$sub" == "${STUB_FAIL_CLASS:-}" ]]; then
    echo "An error occurred (UnauthorizedOperation) when calling the ${sub} operation: stubbed" >&2
    exit 254
fi
if [[ "$sub" == "${STUB_GARBAGE_CLASS:-}" ]]; then
    # Not JSON. The real-world shape of this is a CLI that writes a warning
    # banner on stdout, which the audit captures along with the payload.
    echo "<html>login required</html>"
    exit 0
fi
if [[ "$sub" == "${STUB_LEAK_CLASS:-}" ]]; then
    echo '[{"id":"stub-leaked-resource","state":"available"}]'
    exit 0
fi
echo '[]'
STUB
chmod +x "${STUB_DIR}/aws"

echo "=== Stub mode: the audit's logic ==========================="

echo "[case] every class empty -> clean, exit 0"
# `env` parses its own options BEFORE any NAME=VALUE assignment, so the -u
# flags have to precede PATH=. Written the other way round, `env` takes `-u`
# as the command name and the case fails with exit 127 -- which looks exactly
# like the audit being missing.
env -u STUB_LEAK_CLASS -u STUB_FAIL_CLASS -u STUB_GARBAGE_CLASS \
    PATH="${STUB_DIR}:${PATH}" \
    bash "${AUDIT}" stub-run "${REGION}" >/dev/null 2>&1
rc=$?
if [[ ${rc} -eq 0 ]]; then
    echo "  PASS: clean audit exits 0"; passed=$((passed + 1))
else
    echo "  FAIL: clean audit exited ${rc}, wanted 0" >&2; failed=$((failed + 1))
fi

# The six classes, each leaked on its own. Testing them one at a time rather
# than all together is the point: a bug that drops one class from the loop is
# invisible when every class leaks, because the total is non-zero either way.
echo "[case] one leaked resource per class -> exit 1, naming that class"
for class in describe-instances describe-volumes describe-addresses \
             describe-security-groups describe-placement-groups describe-key-pairs; do
    expect_status_saying "leaked ${class#describe-} fails the audit" 1 "stub-leaked-resource" \
        env PATH="${STUB_DIR}:${PATH}" STUB_LEAK_CLASS="${class}" \
        bash "${AUDIT}" stub-run "${REGION}"
done

# The task-21 failure, per class. This is the case that matters most: it is
# the one where the naive implementation reports success.
echo "[case] a query that ERRORS -> exit 1 and UNKNOWN, never clean"
for class in describe-instances describe-volumes describe-addresses \
             describe-security-groups describe-placement-groups describe-key-pairs; do
    expect_status_saying "failed ${class#describe-} query is UNKNOWN, not clean" 1 "UNKNOWN" \
        env PATH="${STUB_DIR}:${PATH}" STUB_FAIL_CLASS="${class}" \
        bash "${AUDIT}" stub-run "${REGION}"
done

echo "[case] output jq cannot read -> exit 1, not clean"
expect_status_saying "unparseable output is UNKNOWN, not clean" 1 "UNKNOWN" \
    env PATH="${STUB_DIR}:${PATH}" STUB_GARBAGE_CLASS=describe-instances \
    bash "${AUDIT}" stub-run "${REGION}"

rm -rf "${STUB_DIR}"

# ── Live mode ────────────────────────────────────────────────────────────────
if [[ "${LIVE}" == "1" ]]; then
    echo ""
    echo "=== Live mode: the audit's queries ========================="
    echo "  region ${REGION}"

    LIVE_TAG="audit-selftest-$(date -u +%s)-$$"
    TAGSPEC="ResourceType=%s,Tags=[{Key=${TAG_KEY},Value=${LIVE_TAG}}]"
    aws_() { command aws --region "${REGION}" "$@"; }

    SG_ID=""
    KEY_MADE=0
    PG_MADE=0
    KEY_NAME="kythira-${LIVE_TAG}"
    PG_NAME="kythira-${LIVE_TAG}"
    SG_NAME="kythira-${LIVE_TAG}"

    # An EXIT trap, for the same reason Shape 1 uses one: the run most likely
    # to leave something behind is the one that failed partway, which is
    # exactly when a trailing cleanup block does not run. This script's whole
    # subject is resources that outlive their run; leaking one here would be
    # a poor joke.
    live_cleanup() {
        local rc=$?
        set +e
        echo "[live] cleanup"
        [[ -n "${SG_ID}" ]] && aws_ ec2 delete-security-group --group-id "${SG_ID}" >/dev/null 2>&1
        [[ "${KEY_MADE}" == "1" ]] && aws_ ec2 delete-key-pair --key-name "${KEY_NAME}" >/dev/null 2>&1
        [[ "${PG_MADE}" == "1" ]] && aws_ ec2 delete-placement-group --group-name "${PG_NAME}" >/dev/null 2>&1
        return $rc
    }
    trap live_cleanup EXIT

    echo "[live] audit before anything exists"
    expect_status "empty tag audits clean" 0 bash "${AUDIT}" "${LIVE_TAG}" "${REGION}"

    # Created one at a time, each followed by an audit, so the failure names
    # which class the audit could not see. Creating all three and auditing
    # once would prove only that it saw at least one of them.
    echo "[live] leak a key pair (free)"
    if aws_ ec2 create-key-pair --key-name "${KEY_NAME}" \
        --tag-specifications "$(printf "${TAGSPEC}" key-pair)" \
        --query 'KeyMaterial' --output text >/dev/null 2>&1; then
        KEY_MADE=1
        expect_status_saying "audit sees the leaked key pair" 1 "key pair" \
            bash "${AUDIT}" "${LIVE_TAG}" "${REGION}"
    else
        echo "  SKIP: could not create a key pair (missing IAM action?)" >&2
    fi

    # The placement group is the class this spec makes load-bearing, so its
    # skip carries the provider's own message rather than a guess. Reading
    # DescribePlacementGroups and creating one are different IAM actions, and
    # the audit having always been able to *see* a placement group says
    # nothing about whether this principal can *make* one -- which is exactly
    # the confusion the anticipatory audit check created in the first place.
    echo "[live] leak a placement group (free) — the class Shape 1 never created"
    pg_rc=0
    pg_err=$(aws_ ec2 create-placement-group --group-name "${PG_NAME}" --strategy cluster \
        --tag-specifications "$(printf "${TAGSPEC}" placement-group)" 2>&1) || pg_rc=$?
    if [[ ${pg_rc} -eq 0 ]]; then
        PG_MADE=1
        expect_status_saying "audit sees the leaked placement group" 1 "placement group" \
            bash "${AUDIT}" "${LIVE_TAG}" "${REGION}"
    else
        echo "  SKIP: could not create a placement group. AWS said:" >&2
        printf '%s\n' "${pg_err}" | sed 's/^/        /' >&2
        if printf '%s' "${pg_err}" | grep -q 'ec2:CreatePlacementGroup'; then
            echo "        This principal can DescribePlacementGroups but not create one." >&2
            echo "        scripts/ci-cloud-credentials/aws/policies/perf-cloud.json grants" >&2
            echo "        both; a principal provisioned before that change will not have" >&2
            echo "        them. Shape 2's --placement cluster arm needs this action." >&2
        fi
    fi

    echo "[live] leak a security group (free)"
    VPC_ID=$(aws_ ec2 describe-vpcs --filters Name=isDefault,Values=true \
        --query 'Vpcs[0].VpcId' --output text 2>/dev/null)
    if [[ -n "${VPC_ID}" && "${VPC_ID}" != "None" ]] && SG_ID=$(aws_ ec2 create-security-group \
        --group-name "${SG_NAME}" --description "kythira audit self-test ${LIVE_TAG}" \
        --vpc-id "${VPC_ID}" \
        --tag-specifications "$(printf "${TAGSPEC}" security-group)" \
        --query 'GroupId' --output text 2>/dev/null); then
        expect_status_saying "audit sees the leaked security group" 1 "security group" \
            bash "${AUDIT}" "${LIVE_TAG}" "${REGION}"
    else
        SG_ID=""
        echo "  SKIP: could not create a security group (missing IAM action?)" >&2
    fi

    echo "[live] delete them, then audit again"
    live_cleanup >/dev/null 2>&1
    SG_ID=""; KEY_MADE=0; PG_MADE=0
    # Placement-group deletion is not instantaneous; the audit is allowed a
    # few seconds to agree with reality before it is called wrong.
    for _ in 1 2 3 4 5 6; do
        bash "${AUDIT}" "${LIVE_TAG}" "${REGION}" >/dev/null 2>&1 && break
        sleep 5
    done
    expect_status "audit clean again after deletion" 0 bash "${AUDIT}" "${LIVE_TAG}" "${REGION}"
fi

echo ""
echo "=== ${passed} passed, ${failed} failed ==="
[[ "${failed}" -eq 0 ]]
