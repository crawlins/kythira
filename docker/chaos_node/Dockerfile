# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libfiu0 fiu-utils \
        iproute2 \
        iptables \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY docker/chaos_node/dist/chaos_node /usr/local/bin/chaos_node
COPY docker/chaos_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 7000 8080 9000

HEALTHCHECK --interval=2s --timeout=3s --start-period=5s --retries=10 \
    CMD curl -sf "http://localhost:${HTTP_PORT:-8080}/health" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
