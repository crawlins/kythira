# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The cairn tool server (.kiro/specs/cairn-chaos-testing/ design §3):
# chaos_ctl and chaos_workload, plus the docker CLI, which reaches the
# container runtime only through the filtering proxy (DOCKER_HOST, set by
# docker/cairn/cairn-compose.yml). The CLI speaks the same API to Docker
# and to rootless Podman's compatible socket, so one image serves both.
# Agents reach it through chaos_http.py on port 7700 (task 10; Cairn has
# no MCP, so this is plain HTTP that the workers' `chaos` command calls);
# the host still runs init and fence checks with `exec chaos_ctl`.
#
# Built from the repository root after `cmake --build build --target
# docker-cairn-tool-server-image` stages chaos_ctl into dist/; the base,
# kythira-chaos-node:dev, already carries chaos_workload and its runtime.

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS docker-cli

# The static CLI from Docker's own downloads, checked by digest per
# architecture. Only the `docker` binary is kept; the daemon is not.
ARG DOCKER_CLI_VERSION=29.9.1
ARG DOCKER_CLI_SHA256_X86_64=c158ecc55adb3afb01bbd3539ed3b997d20c26dbdc418080506cc1bf5243ff57
ARG DOCKER_CLI_SHA256_AARCH64=3e93cc6d1a686456a3b18eefdde26467a837a7a2b257af22e27cba1dbc1b2018
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && arch="$(uname -m)" \
    && case "$arch" in \
         x86_64) sha="$DOCKER_CLI_SHA256_X86_64" ;; \
         aarch64) sha="$DOCKER_CLI_SHA256_AARCH64" ;; \
         *) echo "no docker CLI digest for $arch" >&2; exit 1 ;; \
       esac \
    && curl -fsSLo /tmp/docker.tgz \
         "https://download.docker.com/linux/static/stable/$arch/docker-$DOCKER_CLI_VERSION.tgz" \
    && echo "$sha  /tmp/docker.tgz" | sha256sum -c - \
    && tar -xzf /tmp/docker.tgz -C /tmp docker/docker \
    && /tmp/docker/docker --version

FROM kythira-chaos-node:dev

# python3 for chaos_http.py, nothing else.
RUN apt-get update && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=docker-cli /tmp/docker/docker /usr/local/bin/docker
COPY docker/cairn/tool_server/chaos_http.py /usr/local/bin/chaos_http.py
COPY docker/cairn/tool_server/dist/chaos_ctl /usr/local/bin/chaos_ctl
COPY docker/cairn/failpoints.allowlist /usr/local/bin/cairn-failpoints.allowlist

# chaos_ctl's runtime calls go through the docker CLI whatever the host
# runs, and it starts no compose stacks in here (init without --start).
ENV KYTHIRA_CONTAINER_RUNTIME=docker \
    KYTHIRA_CHAOS_RUN_DIR=/runs/current

# The base image's healthcheck probes a chaos_node that does not run here.
HEALTHCHECK NONE

EXPOSE 7700

ENTRYPOINT ["python3", "-u", "/usr/local/bin/chaos_http.py"]
