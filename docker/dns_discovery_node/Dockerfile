FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libldns3 curl && \
    rm -rf /var/lib/apt/lists/*

COPY docker/dns_discovery_node/dist/dns_discovery_node /usr/local/bin/dns_discovery_node

HEALTHCHECK --interval=2s --timeout=3s --retries=30 \
    CMD curl -sf http://localhost:${HTTP_PORT:-9021}/health || exit 1

ENTRYPOINT ["/usr/local/bin/dns_discovery_node"]
