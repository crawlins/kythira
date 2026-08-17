#!/usr/bin/env python3
"""OCI Object Storage request helper — request signatures written by hand.

Written independently of `include/raft/oci_signing.hpp` for the same reason
`oss_probe.py` was: an independent implementation of a signing scheme is a
**second witness** to it. If both agree with the live service, the canonical
form documented in that header is confirmed by something other than itself.

One divergence from the C++ signer is deliberate and is a finding in its own
right: `oci_signing.hpp` signs `content-type: application/json` as a constant,
because every existing caller is a control-plane API. **Object storage is a
data plane**, its bodies are opaque bytes, and this signer therefore signs
whatever content type it actually sends. A client that sent
`application/octet-stream` while signing `application/json` would fail with a
401 that says nothing about content types.

Credentials come from `~/.oci/config` (`$OCI_CLI_PROFILE`, else DEFAULT). The
private key is read from the path that file names, is never printed, and is
never placed on a command line.

Configuration:
    OCI_CLI_PROFILE          profile in ~/.oci/config (default DEFAULT)
    KYTHIRA_OCI_BUCKET       required
    KYTHIRA_OCI_NAMESPACE    optional — resolved from the service when absent,
                             which is exactly the question task 0.8 asks
    KYTHIRA_OCI_REGION       optional — defaults to the profile's region
"""
import base64
import configparser
import hashlib
import json
import os
import time
import urllib.error
import urllib.request
from email.utils import formatdate

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

from probe_common import Headers

PROFILE = os.environ.get("OCI_CLI_PROFILE", "DEFAULT")
BUCKET = os.environ.get("KYTHIRA_OCI_BUCKET", "")

_config = configparser.ConfigParser()
_config.read(os.path.expanduser("~/.oci/config"))
if PROFILE not in _config:
    raise SystemExit("no profile %r in ~/.oci/config" % PROFILE)
_profile = _config[PROFILE]

REGION = os.environ.get("KYTHIRA_OCI_REGION", _profile.get("region", ""))
KEY_ID = "%s/%s/%s" % (_profile.get("tenancy"), _profile.get("user"), _profile.get("fingerprint"))
HOST = "objectstorage.%s.oraclecloud.com" % REGION

with open(os.path.expanduser(_profile.get("key_file")), "rb") as handle:
    _passphrase = _profile.get("pass_phrase")
    PRIVATE_KEY = serialization.load_pem_private_key(
        handle.read(), password=_passphrase.encode() if _passphrase else None
    )


def sha256_base64(body):
    return base64.b64encode(hashlib.sha256(body).digest()).decode()


def signing_string(method, request_target, body, date_header, content_type):
    """The canonical form, in the order `oci_signing.hpp` records as the real
    one: `date` first, then `(request-target)`, then `host` — and for a body,
    `content-length`, `content-type`, `x-content-sha256`."""
    lines = [
        "date: " + date_header,
        "(request-target): %s %s" % (method.lower(), request_target),
        "host: " + HOST,
    ]
    if body:
        lines += [
            "content-length: %d" % len(body),
            "content-type: " + content_type,
            "x-content-sha256: " + sha256_base64(body),
        ]
    return "\n".join(lines)


def percent_encode(value, keep_slash=False):
    out = []
    for byte in value.encode("utf-8"):
        char = chr(byte)
        if char.isalnum() or char in "-._~" or (keep_slash and char == "/"):
            out.append(char)
        else:
            out.append("%%%02X" % byte)
    return "".join(out)


def query_string(query):
    if not query:
        return ""
    return "&".join(
        "%s=%s" % (percent_encode(k), percent_encode(str(query[k]))) for k in sorted(query)
    )


def request(method, path, query=None, headers=None, body=b"", content_type="application/octet-stream"):
    """Send one signed request against the Object Storage data plane.

    `path` is the full request path (`/n/<ns>/b/<bucket>/o/<key>`); the signed
    `(request-target)` includes the query string, which is where a signer that
    builds the URL and the signing string separately drifts apart.
    """
    headers = {k.lower(): v for k, v in (headers or {}).items()}
    qs = query_string(query)
    request_target = path + ("?" + qs if qs else "")
    date_header = formatdate(usegmt=True)

    to_sign = signing_string(method, request_target, body, date_header, content_type)
    signature = base64.b64encode(
        PRIVATE_KEY.sign(to_sign.encode(), padding.PKCS1v15(), hashes.SHA256())
    ).decode()
    signed = ["date", "(request-target)", "host"]
    if body:
        signed += ["content-length", "content-type", "x-content-sha256"]

    headers["date"] = date_header
    headers["authorization"] = (
        'Signature version="1",headers="%s",keyId="%s",algorithm="rsa-sha256",signature="%s"'
        % (" ".join(signed), KEY_ID, signature)
    )
    if body:
        headers["content-length"] = str(len(body))
        headers["content-type"] = content_type
        headers["x-content-sha256"] = sha256_base64(body)

    url = "https://%s%s" % (HOST, request_target)
    req = urllib.request.Request(url, data=body if body else None, method=method)
    for name, value in headers.items():
        req.add_header(name, value)

    # Connection-level failures are retried; HTTP statuses never are — the
    # status is the measurement. Retries print rather than hiding.
    last_error = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return resp.status, Headers(resp.headers), resp.read()
        except urllib.error.HTTPError as err:
            return err.code, Headers(err.headers), err.read()
        except (urllib.error.URLError, OSError) as err:
            last_error = err
            print("    [transport retry %d/2: %s %s: %s]" % (attempt + 1, method, path, err),
                  flush=True)
            time.sleep(1 + attempt)
    raise SystemExit("%s %s failed after 3 attempts: %s" % (method, path, last_error))


def resolve_namespace():
    """Task 0.8: is the namespace configurable, or must it be asked for?

    Both, as it turns out — this is the call that answers it, and the answer
    decides whether `oci_object_storage_client` needs a resolve-and-cache step
    or can take the namespace as configuration.
    """
    configured = os.environ.get("KYTHIRA_OCI_NAMESPACE", "")
    if configured:
        return configured, "configured"
    status, _, body = request("GET", "/n/")
    if status != 200:
        raise SystemExit("namespace lookup failed HTTP %s: %s" % (status, body[:200]))
    return json.loads(body.decode()), "resolved from GET /n/"


def parse(body):
    try:
        return json.loads(body.decode("utf-8", "replace"))
    except (ValueError, AttributeError):
        return {}


def code_of(headers, body=b""):
    """OCI's own error code, from the JSON body's `code` field.

    `opc-request-id` is the header worth quoting to support; the machine-
    readable code is in the body.
    """
    parsed = parse(body)
    if isinstance(parsed, dict):
        return parsed.get("code", "")
    return ""


# This tenancy intermittently declines an otherwise valid request with
# `404 BucketNotFound` — the message OCI uses for "does not exist **or you are
# not authorized**". Measured at roughly 3–16% across every request shape,
# body-carrying or not, against a bucket that plainly exists (the surrounding
# requests in the same second succeed). It is recorded as an environment
# observation, never retried silently: a flake that lands on a measurement
# request produces a *wrong cell*, which is worse than a slow probe.
DECLINES = {"count": 0}


def measure(method, path, **kwargs):
    """Issue a measurement request, re-issuing once if the tenancy declines it.

    Prints every re-issue. A cell must not be closed on a status the service
    returned for a reason unrelated to the question being asked — that is the
    same error as reading an effect instead of a status code, one level up.
    """
    status, headers, body = request(method, path, **kwargs)
    if status == 404 and code_of(headers, body) == "BucketNotFound":
        DECLINES["count"] += 1
        print("    [tenancy declined %s %s with 404 BucketNotFound — re-issuing once]"
              % (method, path), flush=True)
        time.sleep(1)
        status, headers, body = request(method, path, **kwargs)
    return status, headers, body


def report(label, status, headers, body, extra=""):
    print(
        "  %-46s HTTP %-3s %-26s %s"
        % (label, status, code_of(headers, body) or "-", extra or headers.get("ETag", "")),
        flush=True,
    )
