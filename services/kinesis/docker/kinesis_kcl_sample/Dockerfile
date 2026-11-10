# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The KCL 2.x workers of the Kinesis container scenario
# (.kiro/specs/kinesis-compatible-streams/ task 9): the sample consumer in
# services/kinesis/tests/kcl/sample, built with the lease layer from source
# (services/kinesis/java/kythira-kcl-leases). Built by the
# docker-kinesis-node-image target with the source root as its context.
#
# No capabilities, no privileged or host networking: CLAUDE.md's container
# rules require this to run under rootless Podman.

FROM gradle:8.14.3-jdk21@sha256:21bd311ed01360c189b8870c6b6e988199ff10f72d445d02fb39d3cff9da91d7 AS build

COPY services/kinesis/java/kythira-kcl-leases /src/services/kinesis/java/kythira-kcl-leases
COPY services/kinesis/tests/kcl/sample /src/services/kinesis/tests/kcl/sample
WORKDIR /src/services/kinesis/tests/kcl/sample
# One worker: Maven Central answers bursts of parallel downloads with 429s.
RUN gradle --no-daemon --max-workers=1 -q installDist

FROM eclipse-temurin:21-jre@sha256:0b99b3376fbb50dbf9c0af8535fdd5b8eb0d3fdfc8b1e4967f383b6919f6901a

COPY --from=build /src/services/kinesis/tests/kcl/sample/build/install/kythira-kcl-sample /opt/kcl-sample

ENTRYPOINT ["/opt/kcl-sample/bin/kythira-kcl-sample"]
