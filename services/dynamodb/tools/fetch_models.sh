#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Download the DynamoDB and DynamoDB Streams botocore models at a botocore
# release tag into model/, and print their SHA-256 for model/README.md.
#
# Usage: services/dynamodb/tools/fetch_models.sh 1.43.126

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <botocore version>" >&2
    exit 2
fi
version="$1"
module="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
base="https://raw.githubusercontent.com/boto/botocore/${version}/botocore/data"
for m in dynamodb dynamodbstreams; do
    curl -fsSL -o "${module}/model/${m}-2012-08-10.json" \
        "${base}/${m}/2012-08-10/service-2.json"
done
cd "${module}/model"
sha256sum ./*.json
