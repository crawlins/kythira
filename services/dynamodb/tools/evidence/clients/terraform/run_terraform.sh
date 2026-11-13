#!/bin/sh
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Run the Terraform conformance client (main.tf) against ENDPOINT
# (.kiro/specs/dynamodb-compatible-api/ task 24): init, apply, a plan that
# must find nothing to change, an in-place change and the same check, then
# destroy. The configuration is copied out of the read-only mount so
# Terraform can write its state. POSIX sh for the hashicorp/terraform image.
#
# Usage: ENDPOINT=http://localhost:8000 run_terraform.sh

set -eu

ENDPOINT="${ENDPOINT:-http://localhost:8000}"
work="$(mktemp -d)"
cp "$(dirname "$0")/main.tf" "${work}/"
cd "${work}"
export TF_IN_AUTOMATION=1
export TF_INPUT=0
export TF_VAR_endpoint="${ENDPOINT}"

# A plan with -detailed-exitcode exits 2 when something would change.
settled() {
    if terraform plan -detailed-exitcode -no-color -var "read_capacity=$1" > plan.txt; then
        return 0
    fi
    cat plan.txt >&2
    echo "terraform plan found changes after apply (read_capacity=$1)" >&2
    exit 1
}

terraform init -no-color > /dev/null
terraform apply -auto-approve -no-color -var read_capacity=5
settled 5
terraform apply -auto-approve -no-color -var read_capacity=7
settled 7
terraform destroy -auto-approve -no-color -var read_capacity=7
echo "terraform conformance flow done"
