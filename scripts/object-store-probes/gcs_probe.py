#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Google Cloud Storage request helper — JSON API with an ADC bearer token.

**The JSON API deliberately, not the XML one.** `gcp_gcs_client` is specified
over `google-cloud-cpp`'s storage client (task 9), which speaks the JSON API,
and a cell closed against a different API surface of the same service is closed
by analogy — the thing this spec's task 0 forbids. Status codes and error
reasons are what the probe measures, and those are exactly what can differ
between two APIs in front of one bucket.

The token comes from application-default credentials
(`gcloud auth application-default print-access-token`). On this project's
development host the gcloud *user* credential is expired and cannot be renewed
non-interactively, while ADC still works — that difference cost a spike cell
once and is recorded in `spike-notes.md`. The token is never printed.

Configuration:
    KYTHIRA_GCS_BUCKET   required
"""
import json
import os
import subprocess
import time
import urllib.error
import urllib.request

from probe_common import Headers

BUCKET = os.environ.get("KYTHIRA_GCS_BUCKET", "")
API = "https://storage.googleapis.com/storage/v1"
UPLOAD_API = "https://storage.googleapis.com/upload/storage/v1"


def load_token():
    cmd = ["gcloud", "auth", "application-default", "print-access-token"]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=120, check=True).stdout
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as err:
        raise SystemExit("could not resolve GCP application-default credentials: %s" % err)
    return out.strip()


TOKEN = load_token()


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


def send(method, url, headers=None, body=b""):
    """One authenticated request; return (status, headers, body).

    Connection-level failures are retried, HTTP statuses never are — the status
    is the measurement. Each retry prints, so a retried request is visible
    rather than silently becoming an extra racer.
    """
    headers = dict(headers or {})
    headers["Authorization"] = "Bearer " + TOKEN
    req = urllib.request.Request(url, data=body if body else None, method=method)
    for name, value in headers.items():
        req.add_header(name, value)
    last_error = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return resp.status, Headers(resp.headers), resp.read()
        except urllib.error.HTTPError as err:
            return err.code, Headers(err.headers), err.read()
        except (urllib.error.URLError, OSError) as err:
            last_error = err
            print("    [transport retry %d/2: %s: %s]" % (attempt + 1, method, err), flush=True)
            time.sleep(1 + attempt)
    raise SystemExit("%s failed after 3 attempts: %s" % (method, last_error))


def require_bucket():
    if not BUCKET:
        raise SystemExit("set KYTHIRA_GCS_BUCKET")


def upload(key, body, query=None, headers=None):
    """Simple media upload. Returns (status, headers, parsed-or-raw body)."""
    require_bucket()
    params = {"uploadType": "media", "name": key}
    params.update(query or {})
    url = "%s/b/%s/o?%s" % (UPLOAD_API, percent_encode(BUCKET), query_string(params))
    hdrs = {"Content-Type": "application/octet-stream"}
    hdrs.update(headers or {})
    return send("POST", url, hdrs, body)


def upload_with_md5(key, body, md5_base64, query=None):
    """Multipart upload carrying `md5Hash` in the object metadata.

    This is how `google-cloud-cpp`'s `MD5HashValue` option reaches the service,
    and therefore the only shape in which a GCS checksum-mismatch answer is
    evidence about the client this spec will ship.
    """
    require_bucket()
    params = {"uploadType": "multipart", "name": key}
    params.update(query or {})
    url = "%s/b/%s/o?%s" % (UPLOAD_API, percent_encode(BUCKET), query_string(params))
    boundary = "kythira-probe-boundary"
    metadata = json.dumps({"name": key, "md5Hash": md5_base64}).encode()
    payload = b"".join(
        [
            b"--", boundary.encode(), b"\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n",
            metadata, b"\r\n",
            b"--", boundary.encode(), b"\r\nContent-Type: application/octet-stream\r\n\r\n",
            body, b"\r\n",
            b"--", boundary.encode(), b"--\r\n",
        ]
    )
    headers = {"Content-Type": 'multipart/related; boundary="%s"' % boundary}
    return send("POST", url, headers, payload)


def get_object(key):
    require_bucket()
    url = "%s/b/%s/o/%s?alt=media" % (API, percent_encode(BUCKET), percent_encode(key))
    return send("GET", url)


def stat_object(key):
    require_bucket()
    url = "%s/b/%s/o/%s" % (API, percent_encode(BUCKET), percent_encode(key))
    return send("GET", url)


def delete_object(key, query=None):
    require_bucket()
    url = "%s/b/%s/o/%s" % (API, percent_encode(BUCKET), percent_encode(key))
    qs = query_string(query)
    if qs:
        url += "?" + qs
    return send("DELETE", url)


def list_objects(prefix):
    require_bucket()
    url = "%s/b/%s/o?%s" % (API, percent_encode(BUCKET), query_string({"prefix": prefix}))
    return send("GET", url)


def parse(body):
    try:
        return json.loads(body.decode("utf-8", "replace"))
    except (ValueError, AttributeError):
        return {}


def reason_of(body):
    """GCS's own error reason — `conditionNotMet`, `invalid`, and so on.

    The reason, not the status: the status alone cannot separate "the
    precondition was evaluated and failed" from "the request was malformed",
    and both arrive as 4xx.
    """
    error = parse(body).get("error", {})
    errors = error.get("errors") or []
    if errors and isinstance(errors[0], dict):
        return errors[0].get("reason", "")
    return error.get("status", "")


def report(label, status, headers, body, extra=""):
    parsed = parse(body)
    generation = parsed.get("generation", "")
    print(
        "  %-46s HTTP %-3s %-26s %s"
        % (label, status, reason_of(body) or "-", extra or ("gen=%s" % generation if generation else "")),
        flush=True,
    )
