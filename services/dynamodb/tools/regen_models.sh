#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Regenerate the DynamoDB module's shape tables from its pinned botocore
# models (model/README.md), with core's scripts/gen_aws_json_shapes.py. Run
# after changing a model; commit the models and the headers together, so
# the generated diff is in the pull request (Requirement 2.4). The
# *_model_header_fresh ctests run these same commands with --check.
#
# Usage: services/dynamodb/tools/regen_models.sh   (from anywhere)

set -euo pipefail

module="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gen="${module}/../../scripts/gen_aws_json_shapes.py"
cd "${module}"
for m in dynamodb dynamodbstreams; do
    python3 "${gen}" "model/${m}-2012-08-10.json" \
        "include/kythira/dynamodb/model/${m}_model.hpp" \
        --namespace "kythira::dynamodb::${m}_model" \
        --label "services/dynamodb/model/${m}-2012-08-10.json"
    echo "wrote include/kythira/dynamodb/model/${m}_model.hpp"
done
