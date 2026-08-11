#!/usr/bin/env bash
# Creates the OCI IAM group and policy that Kythira's real-OCI tests run as,
# scoped to exactly the --bundles given.
#
# .kiro/specs/oci-cloud-provider/ Requirement 14.3.
#
# WHAT THIS SCRIPT DELIBERATELY DOES NOT DO: issue credentials.
#
# The AWS equivalent creates an OIDC-federated role, so running it leaves CI
# able to authenticate with no long-lived secret anywhere. The OCI equivalent
# cannot be written yet. Requirement 14.2 wants GitHub Actions OIDC federated
# to a Dynamic Group, and whether OCI supports that for this shape is Task 0(f)
# — still an open spike question. The documented fallback is a long-lived API
# key held as a CI secret, which Requirement 14.2 says requires "explicit
# sign-off ... not a silent regression from the AWS job's stronger posture".
#
# A script that quietly minted an API key would *be* that silent regression. So
# this provisions the parts both mechanisms need — the group and its policy —
# and leaves the credential decision to a human who has read README.md's
# "Credentials" section.
#
# Usage:
#   scripts/ci-cloud-credentials/oci/provision-ci-identity.sh \
#       --compartment-id OCID --bundles BUNDLE[,BUNDLE...] \
#       [--group-name NAME] [--policy-name NAME] [--dry-run]
#
#   BUNDLE is one of: instance-pool, certificates
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POLICY_DIR="${SCRIPT_DIR}/policies"

COMPARTMENT_ID=""
BUNDLES=""
GROUP_NAME="kythira-ci"
POLICY_NAME="kythira-ci-policy"
DRY_RUN=0

usage() { sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --compartment-id) COMPARTMENT_ID="$2"; shift 2 ;;
        --bundles)        BUNDLES="$2";        shift 2 ;;
        --group-name)     GROUP_NAME="$2";     shift 2 ;;
        --policy-name)    POLICY_NAME="$2";    shift 2 ;;
        --dry-run)        DRY_RUN=1;           shift ;;
        -h|--help)        usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 1 ;;
    esac
done

[[ -n "${COMPARTMENT_ID}" ]] || { echo "--compartment-id is required" >&2; usage 1; }
[[ -n "${BUNDLES}" ]]        || { echo "--bundles is required" >&2; usage 1; }

# The tenancy is where groups and policies live, regardless of which
# compartment the statements scope to.
TENANCY_ID="$(oci iam compartment list --query 'data[0]."compartment-id"' --raw-output 2>/dev/null || true)"
if [[ -z "${TENANCY_ID}" || "${TENANCY_ID}" == "null" ]]; then
    echo "could not determine the tenancy OCID from the OCI CLI config" >&2
    exit 1
fi

# Compartment OCIDs are not usable in a policy statement; the *name* is.
COMPARTMENT_NAME="$(oci iam compartment get --compartment-id "${COMPARTMENT_ID}" \
    --query 'data.name' --raw-output)"
echo "tenancy      ${TENANCY_ID}"
echo "compartment  ${COMPARTMENT_NAME} (${COMPARTMENT_ID})"
echo "group        ${GROUP_NAME}"
echo "bundles      ${BUNDLES}"

STATEMENTS=()
IFS=',' read -ra WANTED <<< "${BUNDLES}"
for bundle in "${WANTED[@]}"; do
    fragment="${POLICY_DIR}/${bundle}.txt"
    [[ -f "${fragment}" ]] || { echo "unknown bundle: ${bundle} (no ${fragment})" >&2; exit 1; }
    while IFS= read -r line; do
        # Strip comments and blanks; substitute the two placeholders.
        [[ -z "${line}" || "${line}" == \#* ]] && continue
        line="${line//\{GROUP\}/${GROUP_NAME}}"
        line="${line//\{COMPARTMENT\}/${COMPARTMENT_NAME}}"
        STATEMENTS+=("${line}")
    done < "${fragment}"
done

echo
echo "policy statements:"
printf '  %s\n' "${STATEMENTS[@]}"

if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo
    echo "--dry-run: nothing created."
    exit 0
fi

echo
GROUP_ID="$(oci iam group list --name "${GROUP_NAME}" --query 'data[0].id' --raw-output 2>/dev/null || true)"
if [[ -z "${GROUP_ID}" || "${GROUP_ID}" == "null" ]]; then
    echo "creating group ${GROUP_NAME}"
    GROUP_ID="$(oci iam group create --name "${GROUP_NAME}" \
        --description "kythira real-cloud CI tests" --query 'data.id' --raw-output)"
else
    echo "group ${GROUP_NAME} already exists"
fi
echo "  ${GROUP_ID}"

# Rendered as a JSON array for the CLI's --statements argument.
STATEMENTS_JSON="$(printf '%s\n' "${STATEMENTS[@]}" | python3 -c 'import json,sys; print(json.dumps([l.rstrip("\n") for l in sys.stdin if l.strip()]))')"

POLICY_ID="$(oci iam policy list --compartment-id "${TENANCY_ID}" --name "${POLICY_NAME}" \
    --query 'data[0].id' --raw-output 2>/dev/null || true)"
if [[ -z "${POLICY_ID}" || "${POLICY_ID}" == "null" ]]; then
    echo "creating policy ${POLICY_NAME}"
    POLICY_ID="$(oci iam policy create --compartment-id "${TENANCY_ID}" --name "${POLICY_NAME}" \
        --description "kythira real-cloud CI tests" --statements "${STATEMENTS_JSON}" \
        --query 'data.id' --raw-output)"
else
    # Replaced, not merged: the statements this script renders are the whole
    # intended grant for the given bundles, and merging would silently keep a
    # permission from a bundle that has since been turned off.
    echo "updating policy ${POLICY_NAME} (statements are replaced, not merged)"
    oci iam policy update --policy-id "${POLICY_ID}" --statements "${STATEMENTS_JSON}" \
        --force >/dev/null
fi
echo "  ${POLICY_ID}"

cat <<EOF

Done. The group and policy exist; **no credentials were created**.

Next, and it is a decision rather than a command — see README.md, "Credentials":
  * Federated (preferred, Requirement 14.2): blocked on Task 0(f).
  * API key fallback: requires explicit sign-off recorded in spike-notes.md.

Add whichever principal you choose to the ${GROUP_NAME} group, then verify with
the tenancy checker before wiring CI:

  cmake --build <build-dir> --target oci_tenancy_check
  ./<build-dir>/tests/oci_tenancy_check
EOF
