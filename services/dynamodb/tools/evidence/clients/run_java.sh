#!/bin/sh
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Build and run one of the Java evidence clients (java-sdk, lock-client,
# kcl2, kcl3, streams-adapter) against the recording stub
# (`.kiro/specs/dynamodb-compatible-api/` task 1). Maven resolves the
# client's pinned dependencies; the client then runs on a plain classpath
# (exec:java's class loader loses SDK classes on shutdown).
#
# Usage: ENDPOINT=http://localhost:8000 run_java.sh <client>

set -eu

client="$1"
dir="$(cd "$(dirname "$0")/${client}" && pwd)"
cd "${dir}"
mvn -q -B compile dependency:build-classpath -Dmdep.outputFile=target/cp.txt
# KCL's Kinesis client speaks CBOR unless told not to; the stub speaks JSON.
exec java -Daws.cborEnabled=false -cp "target/classes:$(cat target/cp.txt)" evidence.Main
