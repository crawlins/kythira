# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The cairn fence's egress proxy, and the probe that checks the fence from
# inside it (docker/cairn/cairn-compose.yml). See egress_proxy.py.
# Same pinned base as docker/container_api_proxy/.

FROM python:3.12-alpine@sha256:1b668429b3511ab407d8e00648891631b0b1a4d7e15e3ca70f38ab5b91ad4ab4

COPY egress_proxy.py /usr/local/bin/egress_proxy.py
COPY fence_probe.py /usr/local/bin/fence_probe.py

EXPOSE 3128

ENTRYPOINT ["python3", "-u", "/usr/local/bin/egress_proxy.py"]
