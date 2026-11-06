#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Generates tests/sigv4_chunked/vectors.json with a pinned botocore.

Usage:
    python3 -m venv /tmp/v && /tmp/v/bin/pip install -r requirements.txt
    /tmp/v/bin/python gen_vectors.py > vectors.json

Each case is a request a client would send, signed by botocore's own S3
signers at a fixed time with fake credentials, for
tests/sigv4_chunk_decoder_test.cpp to verify (`.kiro/specs/
iam-compatible-auth/` task 7, Requirement 3.4):

- `streaming_signed`, `streaming_signed_trailer`: botocore signs the seed
  request (S3SigV4Auth, with x-amz-content-sha256 set to the STREAMING
  value). botocore never signs chunks itself (only the Java, Go and CRT
  SDKs do), so the chunk and trailer signatures are computed here, from
  botocore's derived signing key, as the S3 "Signature Calculations for
  the Authorization Header: Transferring Payload in Multiple Chunks" and
  "... Including Trailing Headers" pages describe. That is an independent
  second implementation of the chunk format next to the C++ one.
- `streaming_unsigned_trailer`: the body is botocore's own aws-chunked
  encoding (httpchecksum.AwsChunkedWrapper, as boto3 sends flexible
  checksums), and the seed signature is S3SigV4Auth's, which picks the
  STREAMING-UNSIGNED-PAYLOAD-TRAILER value itself for a trailer checksum.
- `presigned`: S3SigV4QueryAuth presigned URLs, checking the S3 path rule
  and the security token on the query form.

Bodies are hex so the JSON stays ASCII. Output is deterministic: rerunning
with the pinned botocore reproduces the checked-in file byte for byte.
"""

import base64
import datetime
import hashlib
import hmac
import io
import json
import sys
import zlib
from unittest import mock

import botocore
import botocore.auth
from botocore.awsrequest import AWSRequest
from botocore.credentials import Credentials
from botocore.httpchecksum import AwsChunkedWrapper, Crc32Checksum

# Fake credentials. Never use AWS's documentation example keys here: secret
# scanners flag them.
ACCESS_KEY = "KYTHIRA-TEST-ACCESS-KEY-ID"
SECRET_KEY = "kythira-test-secret-key-not-a-real-credential"
SESSION_TOKEN = "kythira-test-session-token"
REGION = "us-east-1"
WHEN = datetime.datetime(2026, 11, 6, 12, 0, 0, tzinfo=datetime.timezone.utc)
HOST = "bucket.s3.us-east-1.amazonaws.com"
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()


def body_of(n):
    """n deterministic bytes covering every byte value, CR and LF included."""
    return bytes((i * 31 + 7) % 256 for i in range(n))


def headers_list(req):
    return [[k.lower(), v] for k, v in req.headers.items()]


def scope():
    return f"{WHEN:%Y%m%d}/{REGION}/s3/aws4_request"


def signing_key():
    def h(key, msg):
        return hmac.new(key, msg.encode(), hashlib.sha256).digest()

    k = h(("AWS4" + SECRET_KEY).encode(), f"{WHEN:%Y%m%d}")
    return h(h(h(k, REGION), "s3"), "aws4_request")


class StreamingS3SigV4Auth(botocore.auth.S3SigV4Auth):
    """botocore's S3 signer with the STREAMING payload value it would
    otherwise replace with a body hash."""

    def __init__(self, credentials, payload_value):
        super().__init__(credentials, "s3", REGION)
        self._payload_value = payload_value

    def payload(self, request):
        return self._payload_value


def sign(req, signer):
    with mock.patch.object(
        botocore.auth, "get_current_datetime", return_value=WHEN.replace(tzinfo=None)
    ):
        signer.add_auth(req)
    return req


def seed_signature(req):
    return req.headers["Authorization"].rsplit("Signature=", 1)[1]


def chunk_signature(prev, data):
    sts = "\n".join(
        [
            "AWS4-HMAC-SHA256-PAYLOAD",
            f"{WHEN:%Y%m%dT%H%M%SZ}",
            scope(),
            prev,
            EMPTY_SHA256,
            hashlib.sha256(data).hexdigest(),
        ]
    )
    return hmac.new(signing_key(), sts.encode(), hashlib.sha256).hexdigest()


def trailer_signature(prev, trailers):
    canonical = "".join(f"{k}:{v}\n" for k, v in trailers)
    sts = "\n".join(
        [
            "AWS4-HMAC-SHA256-TRAILER",
            f"{WHEN:%Y%m%dT%H%M%SZ}",
            scope(),
            prev,
            hashlib.sha256(canonical.encode()).hexdigest(),
        ]
    )
    return hmac.new(signing_key(), sts.encode(), hashlib.sha256).hexdigest()


def crc32_b64(data):
    return base64.b64encode(zlib.crc32(data).to_bytes(4, "big")).decode()


def signed_case(name, size, chunk, trailer):
    payload = body_of(size)
    mode = (
        "STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER"
        if trailer
        else "STREAMING-AWS4-HMAC-SHA256-PAYLOAD"
    )
    trailers = [["x-amz-checksum-crc32", crc32_b64(payload)]] if trailer else []
    headers = {
        "Host": HOST,
        "Content-Encoding": "aws-chunked",
        "X-Amz-Decoded-Content-Length": str(size),
        "X-Amz-Content-SHA256": mode,
    }
    if trailer:
        headers["X-Amz-Trailer"] = "x-amz-checksum-crc32"
    req = AWSRequest(method="PUT", url=f"https://{HOST}/key%20one.bin", headers=headers)
    sign(req, StreamingS3SigV4Auth(Credentials(ACCESS_KEY, SECRET_KEY), mode))

    prev = seed_signature(req)
    out = b""
    pieces = [payload[i : i + chunk] for i in range(0, size, chunk)] + [b""]
    for piece in pieces:
        prev = chunk_signature(prev, piece)
        out += f"{len(piece):x};chunk-signature={prev}\r\n".encode()
        if piece or not trailer:
            out += piece + b"\r\n"
    if trailer:
        for k, v in trailers:
            out += f"{k}:{v}\r\n".encode()
        out += f"x-amz-trailer-signature:{trailer_signature(prev, trailers)}\r\n\r\n".encode()
    return {
        "name": name,
        "mode": "streaming_signed_trailer" if trailer else "streaming_signed",
        "method": "PUT",
        "path": "/key%20one.bin",
        "query": "",
        "headers": headers_list(req),
        "body_hex": out.hex(),
        "payload_hex": payload.hex(),
        "trailers": trailers,
    }


def unsigned_trailer_case(name, size, chunk, checksum):
    payload = body_of(size)
    wrapper = AwsChunkedWrapper(
        io.BytesIO(payload),
        checksum_cls=Crc32Checksum if checksum else None,
        checksum_name="x-amz-checksum-crc32",
        chunk_size=chunk,
    )
    out = wrapper.read()
    headers = {
        "Host": HOST,
        "Content-Encoding": "aws-chunked",
        "X-Amz-Decoded-Content-Length": str(size),
    }
    if checksum:
        headers["X-Amz-Trailer"] = "x-amz-checksum-crc32"
    req = AWSRequest(method="PUT", url=f"https://{HOST}/upload", headers=headers)
    req.context["checksum"] = {
        "request_algorithm": {"in": "trailer", "name": "x-amz-checksum-crc32"}
    }
    sign(
        req,
        botocore.auth.S3SigV4Auth(Credentials(ACCESS_KEY, SECRET_KEY), "s3", REGION),
    )
    assert req.headers["X-Amz-Content-SHA256"] == "STREAMING-UNSIGNED-PAYLOAD-TRAILER"
    return {
        "name": name,
        "mode": "streaming_unsigned_trailer",
        "method": "PUT",
        "path": "/upload",
        "query": "",
        "headers": headers_list(req),
        "body_hex": out.hex(),
        "payload_hex": payload.hex(),
        "trailers": [["x-amz-checksum-crc32", crc32_b64(payload)]] if checksum else [],
    }


def presigned_case(name, path, token):
    creds = Credentials(ACCESS_KEY, SECRET_KEY, SESSION_TOKEN if token else None)
    req = AWSRequest(method="GET", url=f"https://{HOST}{path}", headers={"Host": HOST})
    sign(req, botocore.auth.S3SigV4QueryAuth(creds, "s3", REGION, expires=900))
    url = req.url
    rest = url.split(HOST, 1)[1]
    raw_path, _, query = rest.partition("?")
    return {
        "name": name,
        "mode": "presigned",
        "method": "GET",
        "path": raw_path,
        "query": query,
        "headers": [["host", HOST]],
        "body_hex": "",
        "payload_hex": "",
        "trailers": [],
    }


def main():
    cases = [
        signed_case("signed-three-chunks", 1000, 400, trailer=False),
        signed_case("signed-exact-multiple", 800, 400, trailer=False),
        signed_case("signed-empty", 0, 400, trailer=False),
        signed_case("signed-trailer-crc32", 1000, 400, trailer=True),
        signed_case("signed-trailer-empty", 0, 400, trailer=True),
        unsigned_trailer_case("unsigned-trailer-crc32", 1000, 300, checksum=True),
        unsigned_trailer_case("unsigned-trailer-no-checksum", 700, 300, checksum=False),
        presigned_case("presigned-get", "/dir/a%20b+c~d.txt", token=False),
        presigned_case("presigned-get-token", "/k%2Fey", token=True),
    ]
    doc = {
        "_comment": "Generated by tests/sigv4_chunked/gen_vectors.py; do not edit.",
        "botocore": botocore.__version__,
        "access_key": ACCESS_KEY,
        "secret_key": SECRET_KEY,
        "session_token": SESSION_TOKEN,
        "region": REGION,
        "now": f"{WHEN:%Y%m%dT%H%M%SZ}",
        "cases": cases,
    }
    json.dump(doc, sys.stdout, indent=1)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
