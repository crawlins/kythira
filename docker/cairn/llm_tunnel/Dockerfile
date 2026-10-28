# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The workers' route to llama.cpp (.kiro/specs/cairn-chaos-testing/ task
# 10): an SSH local forward from llm-tunnel:8080 on cairn_net to
# llama-server on the GPU host's loopback. llama-server never listens on a
# public address, and the GPU host's security group admits SSH only. A
# worker can reach this one port and nothing behind it but the model.

FROM python:3.12-alpine@sha256:1b668429b3511ab407d8e00648891631b0b1a4d7e15e3ca70f38ab5b91ad4ab4

RUN apk add --no-cache openssh-client

COPY entrypoint.sh /usr/local/bin/llm-tunnel

EXPOSE 8080

ENTRYPOINT ["/bin/sh", "/usr/local/bin/llm-tunnel"]
