# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The KCL workers of the Kinesis container scenario
# (.kiro/specs/kinesis-compatible-streams/ tasks 9 and 16): the sample
# consumer in services/kinesis/tests/kcl/sample, built with the lease layer
# from source (services/kinesis/java/kythira-kcl-leases), and the same
# consumer on KCL 3.x (tests/kcl/sample3, /opt/kcl3-sample), and the KPL
# producer of the conformance case (tests/kpl, task 17). Built by the
# docker-kinesis-node-image target with the source root as its context.
#
# No capabilities, no privileged or host networking: CLAUDE.md's container
# rules require this to run under rootless Podman.

FROM gradle:8.14.3-jdk21@sha256:21bd311ed01360c189b8870c6b6e988199ff10f72d445d02fb39d3cff9da91d7 AS build

COPY services/kinesis/java/kythira-kcl-leases /src/services/kinesis/java/kythira-kcl-leases
COPY services/kinesis/tests/kcl/sample /src/services/kinesis/tests/kcl/sample
COPY services/kinesis/tests/kcl/sample3 /src/services/kinesis/tests/kcl/sample3
WORKDIR /src/services/kinesis/tests/kcl/sample
# One worker: Maven Central answers bursts of parallel downloads with 429s.
RUN gradle --no-daemon --max-workers=1 -q installDist
# The same consumer on KCL 3.x (task 16), leases in DynamoDB only.
WORKDIR /src/services/kinesis/tests/kcl/sample3
RUN gradle --no-daemon --max-workers=1 -q installDist
# The KPL producer of the conformance case (task 17).
COPY services/kinesis/tests/kpl /src/services/kinesis/tests/kpl
WORKDIR /src/services/kinesis/tests/kpl
RUN gradle --no-daemon --max-workers=1 -q installDist

FROM eclipse-temurin:21-jre@sha256:0b99b3376fbb50dbf9c0af8535fdd5b8eb0d3fdfc8b1e4967f383b6919f6901a

COPY --from=build /src/services/kinesis/tests/kcl/sample/build/install/kythira-kcl-sample /opt/kcl-sample
COPY --from=build /src/services/kinesis/tests/kcl/sample3/build/install/kythira-kcl3-sample /opt/kcl3-sample

# KPL (task 17) talks HTTPS only; kpl_produce.sh puts a socat TLS front
# with a throwaway certificate before a plaintext node.
RUN apt-get update && apt-get install -y --no-install-recommends socat openssl \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/services/kinesis/tests/kpl/build/install/kythira-kpl-producer /opt/kpl-producer
COPY services/kinesis/tests/kpl/kpl_produce.sh /opt/kpl-producer/kpl_produce.sh

ENTRYPOINT ["/opt/kcl-sample/bin/kythira-kcl-sample"]
