# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
# libldns3: chaos_node links libldns whenever the build host's pkg-config
# finds it (CMakeLists.txt), and the binary is copied in prebuilt.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libfiu0 fiu-utils \
        iproute2 \
        iptables \
        curl \
        libldns3 \
    && rm -rf /var/lib/apt/lists/*

COPY docker/chaos_node/dist/chaos_node /usr/local/bin/chaos_node
# The cairn workload client rides along; a compose service runs it with its
# own entrypoint (docker/chaos-5-compose.yml, profile "workload").
COPY docker/chaos_node/dist/chaos_workload /usr/local/bin/chaos_workload
COPY docker/chaos_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 7000 8080 9000

HEALTHCHECK --interval=2s --timeout=3s --start-period=5s --retries=10 \
    CMD curl -sf "http://localhost:${HTTP_PORT:-8080}/health" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
