# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# kythira-chaos-node:dev plus ca_service, so docker/mtls-chaos-compose.yml can
# mint its certificates with the same image its nodes run. Both binaries are
# staged from the host build (the docker-mtls-chaos-image target), like
# docker/chaos_node/Dockerfile's chaos_node; the base image already carries
# libssl3 (through curl), which is all ca_service needs at run time.
FROM kythira-chaos-node:dev

COPY docker/mtls_chaos/dist/ca_service /usr/local/bin/ca_service
