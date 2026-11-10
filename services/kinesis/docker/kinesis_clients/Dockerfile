# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The boto3 client of the Kinesis container scenario
# (.kiro/specs/kinesis-compatible-streams/ task 7): Python with boto3 at the
# version the wire evidence pins (services/kinesis/tests/evidence/
# requirements.txt), the scenario's producer and polling consumer, and the
# differential tool with its recordings (task 17).
# Built by the docker-kinesis-node-image target with the source root as its
# context.

FROM python:3.12-alpine@sha256:1b668429b3511ab407d8e00648891631b0b1a4d7e15e3ca70f38ab5b91ad4ab4

RUN pip install --no-cache-dir boto3==1.43.100

COPY services/kinesis/tests/docker/kinesis_e2e.py /usr/local/bin/kinesis_e2e.py
# Task 17: the differential tool and the AWS recordings it replays (run with
# --entrypoint python3).
COPY services/kinesis/tests/differential/kinesis_differential.py /usr/local/bin/kinesis_differential.py
COPY services/kinesis/tests/differential/recordings/ /usr/local/share/kinesis-diff/

ENTRYPOINT ["python3", "/usr/local/bin/kinesis_e2e.py"]
