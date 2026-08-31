# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Tier E host image (.kiro/specs/multi-raft-host-binary/ task 8/10).
#
# Ubuntu 24.04 because the binary is built against that libstdc++ and
# `.kiro/specs/multi-raft-performance/` doctrine 134 applies here for the same
# reason it applies to a cloud row: ship the binary, do not rebuild it. A
# container that rebuilt would fold a different compiler and a different
# dependency resolution into the very delta the tier exists to isolate.
#
# No NET_ADMIN and no privileged networking: CLAUDE.md's container rules
# require this to run under rootless Podman, which cannot grant them without
# explicit configuration. The chaos images ask for NET_ADMIN because they
# inject network faults; a measurement host does not.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY docker/multi_raft_node/dist/multi_raft_node /usr/local/bin/multi_raft_node
COPY docker/multi_raft_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Raft, data and control, in that order. Three ports because Requirement 2.4
# keeps the data path off the control surface, and the surest separation is a
# separate socket.
EXPOSE 7000 7001 7002

# Readiness is "every group has a leader", not "the process is up". A driver
# that starts offering load while the cluster is still electing measures the
# election.
HEALTHCHECK --interval=2s --timeout=3s --start-period=5s --retries=30 \
    CMD curl -sf "http://localhost:${CONTROL_PORT:-7002}/ready" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
