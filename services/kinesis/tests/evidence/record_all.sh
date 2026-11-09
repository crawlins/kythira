#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Re-record every client's evidence into data/. Needs the pinned clients:
#   AWS_CLI       the AWS CLI v2 binary (2.31.0)
#   EVIDENCE_PYTHON  a Python with requirements.txt installed (boto3, cbor2)
#   java 21 and gradle, for the Java SDK v2 and KPL projects under clients/
# Every client talks only to the recorder on 127.0.0.1 with dummy keys.
set -euo pipefail
cd "$(dirname "$0")"
py=${EVIDENCE_PYTHON:-python3}
export EVIDENCE_PYTHON=$py
mkdir -p data

aws_cli=${AWS_CLI:-aws}
./run_client.sh "awscli-$("$aws_cli" --version | cut -d' ' -f1 | cut -d/ -f2)" \
  data/awscli-v2.json env AWS_CLI="$aws_cli" clients/awscli_v2.sh
./run_client.sh "boto3-$("$py" -c 'import boto3; print(boto3.__version__)')" \
  data/boto3.json "$py" clients/boto3_client.py

for p in java-sdk-v2 kpl; do
  (cd "clients/$p" && gradle -q --no-daemon installDist)
done
./run_client.sh aws-sdk-java-v2-2.57.11 data/java-sdk-v2.json \
  clients/java-sdk-v2/build/install/kinesis-evidence-java-sdk-v2/bin/kinesis-evidence-java-sdk-v2
# KPL never creates streams and only speaks HTTPS.
EVIDENCE_STREAM_EXISTS=1 EVIDENCE_TLS=1 ./run_client.sh kpl-0.15.12 data/kpl.json \
  clients/kpl/build/install/kinesis-evidence-kpl/bin/kinesis-evidence-kpl
