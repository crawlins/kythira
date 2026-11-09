#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Print an ElastiCache IAM auth token made by botocore, the way the
ElastiCache documentation builds one, at a fixed time and with made-up
credentials, for iam_redis_verifier_test's vector
(.kiro/specs/iam-compatible-auth/ task 21).

    pip install --target /tmp/pylib botocore
    PYTHONPATH=/tmp/pylib python3 gen_redis_iam_token.py \
        <k_botocore_key_id> <k_botocore_secret>

The made-up key pair is passed in, copied from the test's constants of
those names, rather than written here twice: it is not an AWS key (the
test stores it in an IAM state machine of its own), and keeping one copy
keeps secret scanners from reading a key-shaped pair into this script.
Rerun this and paste the output into the test's k_botocore_token when
changing any of the inputs.
"""

import datetime
import sys
from urllib.parse import ParseResult, urlencode, urlunparse

import botocore.auth
import botocore.session
from botocore.model import ServiceId
from botocore.signers import RequestSigner

REGION = "us-east-1"
CACHE_NAME = "kythira-cache"
USER = "farm"
AT = datetime.datetime(2026, 11, 9, 12, 0, 0)


class _FixedDatetime(datetime.datetime):
    @classmethod
    def utcnow(cls):
        return AT

    @classmethod
    def now(cls, tz=None):
        return AT.replace(tzinfo=tz) if tz is not None else AT


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gen_redis_iam_token.py <access key id> <secret access key>")
    access_key, secret_key = sys.argv[1], sys.argv[2]
    botocore.auth.datetime.datetime = _FixedDatetime
    session = botocore.session.get_session()
    session.set_credentials(access_key, secret_key)
    signer = RequestSigner(ServiceId("elasticache"), REGION, "elasticache", "v4",
                           session.get_credentials(), session.get_component("event_emitter"))
    url = urlunparse(ParseResult(scheme="https", netloc=CACHE_NAME, path="/",
                                 query=urlencode({"Action": "connect", "User": USER}),
                                 params="", fragment=""))
    signed = signer.generate_presigned_url(
        {"method": "GET", "url": url, "body": {}, "headers": {}, "context": {}},
        operation_name="connect", expires_in=900, region_name=REGION)
    print(signed.removeprefix("https://"))


if __name__ == "__main__":
    main()
