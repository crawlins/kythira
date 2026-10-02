# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# A filtering front for the container API socket, for the scenario tests
# whose nodes provision containers (docker-compose.quorum.yml,
# elastic-capacity-compose.yml). See proxy.py for what it lets through.

FROM python:3.12-alpine

COPY proxy.py /usr/local/bin/container_api_proxy.py

EXPOSE 2375

ENTRYPOINT ["python3", "-u", "/usr/local/bin/container_api_proxy.py"]
