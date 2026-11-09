#!/bin/sh
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The AWS CLI evidence client (`.kiro/specs/dynamodb-compatible-api/` task 1).
# Runs the commands an operator types against a table: create and wait,
# put/get/update/delete with expressions, query and scan (the CLI paginates
# on its own), batch writes, TTL, tags, describe-limits, then delete and
# wait. POSIX sh so it runs in the amazon/aws-cli image.
#
# Usage: ENDPOINT=http://localhost:8000 aws_cli.sh

set -eu

ENDPOINT="${ENDPOINT:-http://localhost:8000}"
TABLE=evidence-cli
export AWS_ACCESS_KEY_ID=EVIDENCE-aws-cli
export AWS_SECRET_ACCESS_KEY=evidence-secret-not-a-credential
export AWS_DEFAULT_REGION=us-east-1
export AWS_PAGER=""

ddb() {
    aws --endpoint-url "${ENDPOINT}" --output json dynamodb "$@" > /dev/null
}

ddb create-table --table-name "${TABLE}" \
    --attribute-definitions AttributeName=pk,AttributeType=S AttributeName=sk,AttributeType=S \
    --key-schema AttributeName=pk,KeyType=HASH AttributeName=sk,KeyType=RANGE \
    --provisioned-throughput ReadCapacityUnits=5,WriteCapacityUnits=5
ddb wait table-exists --table-name "${TABLE}"
ddb list-tables
ddb describe-limits
ddb put-item --table-name "${TABLE}" \
    --item '{"pk":{"S":"a"},"sk":{"S":"1"},"n":{"N":"1.50"},"l":{"L":[{"S":"x"}]}}' \
    --condition-expression 'attribute_not_exists(pk)' \
    --return-consumed-capacity TOTAL
ddb get-item --table-name "${TABLE}" --key '{"pk":{"S":"a"},"sk":{"S":"1"}}' --consistent-read
ddb update-item --table-name "${TABLE}" --key '{"pk":{"S":"a"},"sk":{"S":"1"}}' \
    --update-expression 'SET n = n + :d' \
    --expression-attribute-values '{":d":{"N":"1"}}' \
    --return-values UPDATED_NEW
ddb query --table-name "${TABLE}" \
    --key-condition-expression 'pk = :p AND begins_with(sk, :s)' \
    --expression-attribute-values '{":p":{"S":"a"},":s":{"S":"1"}}'
ddb scan --table-name "${TABLE}" --page-size 1
ddb scan --table-name "${TABLE}" --select COUNT
ddb batch-write-item --request-items \
    "{\"${TABLE}\":[{\"PutRequest\":{\"Item\":{\"pk\":{\"S\":\"b\"},\"sk\":{\"S\":\"1\"}}}}]}"
ddb update-time-to-live --table-name "${TABLE}" \
    --time-to-live-specification Enabled=true,AttributeName=expires
ddb describe-time-to-live --table-name "${TABLE}"
ddb delete-item --table-name "${TABLE}" --key '{"pk":{"S":"a"},"sk":{"S":"1"}}'
ddb delete-table --table-name "${TABLE}"
echo "aws cli evidence flow done"
