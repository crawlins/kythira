#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The evidence scenario through the AWS CLI v2 (`aws` on PATH, or $AWS_CLI).
# Usage: awscli_v2.sh ENDPOINT
set -euo pipefail
endpoint=$1
aws=${AWS_CLI:-aws}
k() { "$aws" --endpoint-url "$endpoint" --no-cli-pager --output json kinesis "$@" >/dev/null; }
k create-stream --stream-name evidence --shard-count 1
k describe-stream-summary --stream-name evidence
k list-shards --stream-name evidence
k put-record --stream-name evidence --partition-key pk-1 --data aGVsbG8= \
  --cli-binary-format base64
k put-records --stream-name evidence --cli-binary-format base64 \
  --records Data=b25l,PartitionKey=pk-2 Data=dHdv,PartitionKey=pk-3,ExplicitHashKey=42
k get-shard-iterator --stream-name evidence --shard-id shardId-000000000000 \
  --shard-iterator-type TRIM_HORIZON
k get-records --shard-iterator AAAAAAAAAAE --limit 100
k delete-stream --stream-name evidence --enforce-consumer-deletion
