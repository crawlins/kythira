# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The cairn worker image (.kiro/specs/cairn-chaos-testing/ task 10,
# Requirement 7.4): the Pi agent CLI that Cairn's dispatcher runs, and the
# `chaos` command that is the agents' only way to the cluster. Nothing else.
#
# Not FROM Cairn's own worker image, though task 1a pinned it: that image is
# Kali with a security toolchain, which Requirement 7.4 rules out, and it
# carries Codex and Claude Code, which this setup does not use (only the Pi
# worker can reach llama.cpp, task 1b). What the dispatcher needs from a
# worker is /bin/sh, coreutils `timeout` and `pi` on PATH
# (cairn/dispatcher/workers/adapters/pi.py, runtime/containers.py at
# v0.2.1), and Pi's grep and find tools look for rg and fd/fdfind and
# would otherwise download them at run time.
#
# Pi 0.73.0 is the version Cairn's image carries (task 1a); package-lock.json
# pins its whole dependency tree. Install scripts are skipped: Pi does not
# need them, and nothing fetched may run code at build time.
#
# Build from the repository root:
#   <runtime> build -f docker/cairn/worker/Dockerfile -t kythira-cairn-worker:dev .

FROM node:22.21.1-bookworm-slim@sha256:25b3eb23a00590b7499f2a2ce939322727fcce1b15fdd69754fcd09536a3ae2c

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ripgrep fd-find \
    && rm -rf /var/lib/apt/lists/*

# Workers have no route to the internet (the fence, Requirement 7.2), so
# Pi must not try: no tool downloads, version checks or install telemetry.
ENV PI_OFFLINE=1 \
    PI_SKIP_VERSION_CHECK=1 \
    PI_TELEMETRY=0 \
    CHAOS_URL=http://tool-server:7700

WORKDIR /opt/pi
COPY docker/cairn/worker/package.json docker/cairn/worker/package-lock.json ./
RUN npm ci --ignore-scripts --no-audit --no-fund \
    && npm cache clean --force \
    && ln -s /opt/pi/node_modules/.bin/pi /usr/local/bin/pi \
    && pi --version

COPY docker/cairn/worker/chaos /usr/local/bin/chaos

WORKDIR /root
CMD ["sleep", "infinity"]
