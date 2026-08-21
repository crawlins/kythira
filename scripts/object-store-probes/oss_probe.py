#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Alibaba OSS request helper with V4 (OSS4-HMAC-SHA256) header signing.

The signing here is written from the canonical form documented in
`include/raft/alibaba_oss_client.hpp` and **deliberately does not call the C++
client**. Two reasons, both of which matter for a spike:

  * the client cannot send conditional headers — whether OSS honours them is
    exactly what these probes decide, so a probe built on the client could not
    ask the question;
  * an independent implementation is a second witness to the signing scheme.
    It authenticated on its first live call, which is itself a confirmation of
    the documented canonical form.

Credentials are read from `~/.aliyun/config.json` (profile from
`$ALIBABA_PROFILE`, else the CLI's `current`, else the first AK profile) and
are never printed, never placed on a command line, and never passed through
the environment.

Configuration:
    KYTHIRA_ALIBABA_REGION       default ap-southeast-1
    KYTHIRA_ALIBABA_OSS_BUCKET   required for probes; no default bucket is
                                 assumed, so a probe cannot write to someone
                                 else's bucket by forgetting a variable
    ALIBABA_PROFILE              aliyun CLI profile name
"""
import hashlib
import hmac
import json
import os
import urllib.error
import urllib.request
from datetime import datetime, timezone

REGION = os.environ.get("KYTHIRA_ALIBABA_REGION", "ap-southeast-1")
BUCKET = os.environ.get("KYTHIRA_ALIBABA_OSS_BUCKET", "")


def load_credentials(profile_name=None):
    """Return (access_key_id, access_key_secret) from the aliyun CLI config."""
    path = os.path.expanduser("~/.aliyun/config.json")
    try:
        with open(path) as fh:
            cfg = json.load(fh)
    except FileNotFoundError:
        raise SystemExit(
            "%s not found — configure the aliyun CLI, or restore the profile" % path
        )
    name = profile_name or os.environ.get("ALIBABA_PROFILE") or cfg.get("current") or "default"
    for prof in cfg.get("profiles", []):
        if prof.get("name") == name and prof.get("access_key_id"):
            return prof["access_key_id"], prof["access_key_secret"]
    for prof in cfg.get("profiles", []):
        if prof.get("access_key_id"):
            return prof["access_key_id"], prof["access_key_secret"]
    raise SystemExit("no AccessKey profile found in %s" % path)


AK_ID, AK_SECRET = load_credentials()


def percent_encode(value, keep_slash=False):
    """Percent-encode, unreserved characters literal; '/' optionally literal."""
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
    parts = []
    for key in sorted(query):
        value = query[key]
        parts.append(percent_encode(key) + ("=" + percent_encode(value) if value != "" else ""))
    return "&".join(parts)


def sign(method, key, query, headers):
    """Return the full header map for one request, Authorization included.

    x-oss-* headers plus content-type and content-md5 are signed; everything
    else (If-Match, If-None-Match) rides unsigned, which is what lets these
    probes vary preconditions without touching the signature.
    """
    now = datetime.now(timezone.utc)
    timestamp = now.strftime("%Y%m%dT%H%M%SZ")
    date = timestamp[:8]
    scope = "%s/%s/oss/aliyun_v4_request" % (date, REGION)

    signed = {"x-oss-content-sha256": "UNSIGNED-PAYLOAD", "x-oss-date": timestamp}
    for name, value in headers.items():
        lname = name.lower()
        if lname.startswith("x-oss-") or lname in ("content-type", "content-md5"):
            signed[lname] = value

    resource = "/" + BUCKET + ("/" + percent_encode(key, keep_slash=True) if key else "/")
    canonical_headers = "".join("%s:%s\n" % (n, signed[n]) for n in sorted(signed))
    canonical = "\n".join(
        [method, resource, canonical_query(query), canonical_headers, "", "UNSIGNED-PAYLOAD"]
    )
    string_to_sign = "OSS4-HMAC-SHA256\n%s\n%s\n%s" % (
        timestamp,
        scope,
        hashlib.sha256(canonical.encode()).hexdigest(),
    )

    key_material = hmac.new(
        ("aliyun_v4" + AK_SECRET).encode(), date.encode(), hashlib.sha256
    ).digest()
    for step in (REGION.encode(), b"oss", b"aliyun_v4_request"):
        key_material = hmac.new(key_material, step, hashlib.sha256).digest()
    signature = hmac.new(key_material, string_to_sign.encode(), hashlib.sha256).hexdigest()

    out = dict(headers)
    out.update(signed)
    out["Authorization"] = "OSS4-HMAC-SHA256 Credential=%s/%s,Signature=%s" % (
        AK_ID,
        scope,
        signature,
    )
    return out


def request(method, key, query=None, headers=None, body=b""):
    """Send one request; return (status, headers, body). Never raises on 4xx/5xx."""
    if not BUCKET:
        raise SystemExit("set KYTHIRA_ALIBABA_OSS_BUCKET")
    query = query or {}
    headers = dict(headers or {})
    if body and "content-type" not in {h.lower() for h in headers}:
        headers["content-type"] = "application/octet-stream"
    all_headers = sign(method, key, query, headers)

    url = "https://%s.oss-%s.aliyuncs.com/%s" % (BUCKET, REGION, percent_encode(key, True))
    query_string = canonical_query(query)
    if query_string:
        url += "?" + query_string
    req = urllib.request.Request(url, data=body if body else None, method=method)
    for name, value in all_headers.items():
        req.add_header(name, value)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as err:
        return err.code, dict(err.headers), err.read()


def xml_value(body, element):
    """Every text content of a flat <element>, the bounded scan the C++ client uses."""
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
