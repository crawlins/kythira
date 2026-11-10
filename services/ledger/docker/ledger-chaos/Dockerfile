# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The ledger chaos node (.kiro/specs/verifiable-ledger/ task 11,
# Requirement 12.5): the `ledger_node` daemon, with `ledger_ctl` beside it
# so the scenario can verify a member's data directory in place with
# `exec ... ledger_ctl verify --data-dir /data`.
#
# Built like services/redis/docker/engine-chaos/Dockerfile: both binaries
# are compiled on the host against Ubuntu 24.04's libstdc++ and COPIED in
# (RocksDB and OpenSSL come statically from vcpkg). Shared libraries the
# binaries take from the build's own trees (ion-c, which the Ion job's vcpkg
# tree builds shared) are staged into dist/lib by stage_runtime_libs.cmake
# and registered with ldconfig. It runs as uid
# 10001 with a /data volume. No HEALTHCHECK: the scenario probes the
# published API itself, with the client certificate a probe would need.
# No capabilities, no privileged or host networking, so it runs under
# rootless Podman as under Docker (CLAUDE.md).

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libatomic1 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --user-group --no-create-home \
        --home-dir /data --shell /usr/sbin/nologin kythira \
    && mkdir -p /data \
    && chown 10001:10001 /data \
    && chmod 0750 /data

COPY services/ledger/docker/ledger-chaos/dist/ledger_node /usr/local/bin/ledger_node
COPY services/ledger/docker/ledger-chaos/dist/ledger_ctl /usr/local/bin/ledger_ctl
COPY services/ledger/docker/ledger-chaos/dist/lib/ /usr/local/lib/kythira/
RUN echo /usr/local/lib/kythira > /etc/ld.so.conf.d/kythira.conf && ldconfig

# Fail the image build, naming the library, rather than leave a container
# to exit 127 with nothing in its log if the host build links a shared
# library this image does not carry.
RUN missing="$(for b in /usr/local/bin/ledger_node /usr/local/bin/ledger_ctl; do \
                 ldd "$b" | sed -n "s|^|$b: |; /not found/p"; done)"; \
    if [ -n "$missing" ]; then echo "$missing" >&2; exit 1; fi

ENV KYTHIRA_LEDGER_DATA_DIR=/data

VOLUME /data
USER 10001:10001

# Raft (mutual TLS) and the HTTPS API. Configuration is entirely by
# environment; see `ledger_node --help`.
EXPOSE 7000 8443

ENTRYPOINT ["/usr/local/bin/ledger_node"]
