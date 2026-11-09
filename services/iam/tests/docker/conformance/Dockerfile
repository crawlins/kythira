# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The conformance driver (../conformance.yml, task 23): boto3 and botocore
# at the versions Requirement 15.1 pins, and cryptography for the driver's
# own JWTs and certificate requests. Alpine, the base the test issuer
# already pulls (cryptography ships musllinux wheels). The script is
# copied in, so the image is all a run needs.

FROM docker.io/library/python:3.12-alpine

RUN pip install --no-cache-dir \
        boto3==1.35.0 \
        botocore==1.35.0 \
        cryptography==43.0.1

COPY driver.py /conformance/driver.py

ENTRYPOINT ["python3", "/conformance/driver.py"]
