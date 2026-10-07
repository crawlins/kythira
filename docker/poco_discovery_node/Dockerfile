# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libavahi-client3 \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY docker/poco_discovery_node/dist/poco_discovery_node /usr/local/bin/poco_discovery_node
COPY docker/poco_discovery_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

HEALTHCHECK --interval=2s --timeout=3s --start-period=15s --retries=20 \
    CMD curl -sf "http://localhost:${HTTP_PORT:-9011}/health" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
