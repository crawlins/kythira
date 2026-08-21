#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""AWS S3 request helper with SigV4 signing.

Written for the same reason as `oss_probe.py`: these probes need the **raw
HTTP status code**, and the decisive S3 question is precisely a status-code
distinction (412 `PreconditionFailed` versus 409 `ConditionalRequestConflict`)
that an SDK or the CLI reports as an exception message. Getting that wrong in
a client is not a cosmetic error — mapping 409 to a precondition failure
latches a healthy engine permanently, and mapping 412 to a retry silently
overwrites another writer.

Credentials come from the AWS CLI via `aws configure export-credentials`, so
static keys, SSO sessions and credential processes all work and nothing is
read out of a credentials file directly. They are never printed.

Configuration:
    KYTHIRA_AWS_PROFILE    CLI profile (default: the CLI's own default)
    KYTHIRA_AWS_REGION     default us-east-1
    KYTHIRA_S3_BUCKET      required
"""
import hashlib
import hmac
import json
import os
import subprocess
import urllib.error
import urllib.request
from datetime import datetime, timezone

REGION = os.environ.get("KYTHIRA_AWS_REGION", "us-east-1")
BUCKET = os.environ.get("KYTHIRA_S3_BUCKET", "")
PROFILE = os.environ.get("KYTHIRA_AWS_PROFILE", "")
SERVICE = "s3"


def load_credentials():
    """(access_key, secret_key, session_token) from the AWS CLI's resolver."""
    cmd = ["aws", "configure", "export-credentials", "--format", "process"]
    if PROFILE:
        cmd += ["--profile", PROFILE]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60, check=True).stdout
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as err:
        raise SystemExit("could not resolve AWS credentials: %s" % err)
    creds = json.loads(out)
    return creds["AccessKeyId"], creds["SecretAccessKey"], creds.get("SessionToken")


AK_ID, AK_SECRET, SESSION_TOKEN = load_credentials()


def percent_encode(value, keep_slash=False):
    out = []
    for byte in value.encode("utf-8"):
        char = chr(byte)
        if char.isalnum() or char in "-._~" or (keep_slash and char == "/"):
            out.append(char)
        else:
            out.append("%%%02X" % byte)
    return "".join(out)


def canonical_query(query):
    if not query:
        return ""
    return "&".join(
        "%s=%s" % (percent_encode(k), percent_encode(query[k])) for k in sorted(query)
    )


def request(method, key, query=None, headers=None, body=b""):
    """Send one SigV4-signed request; return (status, headers, body)."""
    if not BUCKET:
        raise SystemExit("set KYTHIRA_S3_BUCKET")
    query = query or {}
    headers = {k.lower(): v for k, v in (headers or {}).items()}

    host = "%s.s3.%s.amazonaws.com" % (BUCKET, REGION)
    now = datetime.now(timezone.utc)
    amz_date = now.strftime("%Y%m%dT%H%M%SZ")
    date = amz_date[:8]
    scope = "%s/%s/%s/aws4_request" % (date, REGION, SERVICE)
    payload_hash = hashlib.sha256(body).hexdigest()

    headers["host"] = host
    headers["x-amz-date"] = amz_date
    headers["x-amz-content-sha256"] = payload_hash
    if SESSION_TOKEN:
        headers["x-amz-security-token"] = SESSION_TOKEN

    signed_names = sorted(headers)
    canonical_headers = "".join("%s:%s\n" % (n, headers[n].strip()) for n in signed_names)
    signed_headers = ";".join(signed_names)
    canonical_uri = "/" + percent_encode(key, keep_slash=True) if key else "/"
    canonical_request = "\n".join(
        [
            method,
            canonical_uri,
            canonical_query(query),
            canonical_headers,
            signed_headers,
            payload_hash,
        ]
    )
    string_to_sign = "\n".join(
        [
            "AWS4-HMAC-SHA256",
            amz_date,
            scope,
            hashlib.sha256(canonical_request.encode()).hexdigest(),
        ]
    )

    key_material = ("AWS4" + AK_SECRET).encode()
    for step in (date.encode(), REGION.encode(), SERVICE.encode(), b"aws4_request"):
        key_material = hmac.new(key_material, step, hashlib.sha256).digest()
    signature = hmac.new(key_material, string_to_sign.encode(), hashlib.sha256).hexdigest()
    headers["authorization"] = (
        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s"
        % (AK_ID, scope, signed_headers, signature)
    )

    url = "https://%s%s" % (host, canonical_uri)
    query_string = canonical_query(query)
    if query_string:
        url += "?" + query_string
    req = urllib.request.Request(url, data=body if body else None, method=method)
    for name, value in headers.items():
        if name != "host":  # urllib sets Host itself from the URL
            req.add_header(name, value)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as err:
        return err.code, dict(err.headers), err.read()


def xml_value(body, element):
    text = body.decode("utf-8", "replace")
    open_tag, close_tag = "<%s>" % element, "</%s>" % element
    out = []
    pos = 0
    while True:
        start = text.find(open_tag, pos)
        if start < 0:
            return out
        end = text.find(close_tag, start)
        if end < 0:
            return out
        out.append(text[start + len(open_tag) : end])
        pos = end + len(close_tag)


def code_of(body):
    codes = xml_value(body, "Code")
    return codes[0] if codes else ""


def report(label, status, headers, body, extra=""):
    print(
        "  %-46s HTTP %-3s %-26s %s"
        % (label, status, code_of(body) or "-", extra or headers.get("ETag", "")),
        flush=True,
    )
