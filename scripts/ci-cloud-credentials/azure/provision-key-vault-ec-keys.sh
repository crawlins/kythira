#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Creates the P-256 and P-384 keys behind the key-vault bundle's ES256/ES384
# cases, each with a self-signed certificate, in the vault the bundle already
# uses, then stores their names and certificates as repository settings:
#
#   variables  AZURE_TEST_KEY_VAULT_EC256_KEY_NAME, AZURE_TEST_KEY_VAULT_EC384_KEY_NAME
#   secrets    AZURE_TEST_KEY_VAULT_EC256_CA_CERT_PEM, AZURE_TEST_KEY_VAULT_EC384_CA_CERT_PEM
#
# Key Vault's certificate create makes the key and its certificate together,
# so the certificate's public key is the vault key by construction; that is
# all azure_key_vault_ca_provider_real_test needs from a CA certificate.
#
# The vault comes from the AZURE_TEST_KEY_VAULT_URL repository variable unless
# --vault-name is given. Re-running is safe: a certificate that already exists
# is reused, not recreated, and the settings are rewritten with its values.
#
# Needs `az` logged in as an account that can create certificates in the vault
# (Key Vault Certificates Officer, or the vault's owner) and `gh` logged in
# with admin on the repository. The CI identity needs nothing new: its Key
# Vault Crypto User role is scoped to the whole vault.
#
# Usage:
#   scripts/ci-cloud-credentials/azure/provision-key-vault-ec-keys.sh [--vault-name NAME] [--repo OWNER/REPO]
set -euo pipefail

REPO="crawlins/kythira"
VAULT_NAME=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --vault-name) VAULT_NAME="$2"; shift 2 ;;
        --repo) REPO="$2"; shift 2 ;;
        # The header after the licence lines, matched on content rather
        # than a line range so editing the header never shifts it.
        -h|--help) awk '/^# Creates the P-256/{p=1} p&&!/^#/{exit} p{sub(/^# ?/,""); print}' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

for tool in az gh; do
    command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} is not installed." >&2; exit 1; }
done

if [[ -z "${VAULT_NAME}" ]]; then
    # The REST endpoint rather than `gh variable get`, which only exists
    # from gh 2.47 on.
    url=$(gh api "repos/${REPO}/actions/variables/AZURE_TEST_KEY_VAULT_URL" --jq .value) || {
        echo "ERROR: could not read AZURE_TEST_KEY_VAULT_URL from ${REPO}; pass --vault-name." >&2
        exit 1
    }
    # https://<name>.vault.azure.net[/]
    VAULT_NAME=$(printf '%s' "${url}" | sed -E 's#^https?://([^./]+)\..*$#\1#')
    if [[ -z "${VAULT_NAME}" || "${VAULT_NAME}" == "${url}" ]]; then
        echo "ERROR: AZURE_TEST_KEY_VAULT_URL ('${url}') is not a vault URL; pass --vault-name." >&2
        exit 1
    fi
fi
echo "Vault: ${VAULT_NAME}  Repository: ${REPO}"

work=$(mktemp -d)
trap 'rm -rf "${work}"' EXIT

for curve in 256 384; do
    name="kythira-test-ca-es${curve}"
    if az keyvault certificate show --vault-name "${VAULT_NAME}" -n "${name}" >/dev/null 2>&1; then
        echo "${name}: exists, reusing it"
    else
        cat > "${work}/policy-${curve}.json" <<EOF
{"issuerParameters": {"name": "Self"},
 "keyProperties": {"exportable": false, "keyType": "EC", "curve": "P-${curve}", "reuseKey": false},
 "x509CertificateProperties": {"subject": "CN=${name}",
                               "keyUsage": ["keyCertSign", "digitalSignature"],
                               "validityInMonths": 24},
 "secretProperties": {"contentType": "application/x-pem-file"}}
EOF
        echo "${name}: creating"
        az keyvault certificate create --vault-name "${VAULT_NAME}" -n "${name}" \
            -p "@${work}/policy-${curve}.json" -o none
    fi

    pem="${work}/es${curve}-ca.pem"
    az keyvault certificate download --vault-name "${VAULT_NAME}" -n "${name}" -e PEM -f "${pem}"
    grep -q 'BEGIN CERTIFICATE' "${pem}" || {
        echo "ERROR: ${name}'s downloaded certificate is not PEM." >&2
        exit 1
    }

    gh variable set "AZURE_TEST_KEY_VAULT_EC${curve}_KEY_NAME" -R "${REPO}" --body "${name}"
    gh secret set "AZURE_TEST_KEY_VAULT_EC${curve}_CA_CERT_PEM" -R "${REPO}" < "${pem}"
    echo "${name}: repository settings written"
done

echo "Done. The key-vault bundle's ES256/ES384 cases run on the next dispatch with azure_bundle_key_vault=true."
