#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Azure Blob request helper — hand-rolled REST with an AAD bearer token.

Written for the same reason as `aws_probe.py` and `oss_probe.py`: the probes
need the **raw HTTP status code and the service's own error code**, which an
SDK reports as an exception message. It is also the direct evidence task 0.6
asks for — the design's recorded decision is that `azure_blob_client` speaks
Blob REST over httplib with a bearer token rather than taking a dependency on
`azure-storage-blobs-cpp`, and the honest way to check that the surface is as
workable as documented is to write it once, by hand, against the live service.

The token comes from the Azure CLI (`az account get-access-token`), so device
logins, service principals and managed identities all work and nothing reads a
credential file. It is never printed and never placed on a command line.

Two things this file pins on purpose, both of which are client obligations in
the design:

* **`x-ms-version` is a named, dated constant.** An unpinned version is how a
  working client breaks on a service update.
* **`x-ms-blob-type: BlockBlob` on every PUT.** Omitting it is a 400, not a
  default.

Configuration:
    KYTHIRA_AZURE_STORAGE_ACCOUNT   required (e.g. kythirarealtestobj)
    KYTHIRA_AZURE_BLOB_CONTAINER    required (e.g. kythira-raft)
"""
import json
import os
import subprocess
import time
import urllib.error
import urllib.request

from probe_common import Headers

# Pinned deliberately. 2023-11-03 is a GA version supported by every current
# Azure Storage endpoint; the client header will carry the same constant and
# say why it is a constant.
API_VERSION = "2023-11-03"

ACCOUNT = os.environ.get("KYTHIRA_AZURE_STORAGE_ACCOUNT", "")
CONTAINER = os.environ.get("KYTHIRA_AZURE_BLOB_CONTAINER", "")
RESOURCE = "https://storage.azure.com/"


def load_token():
    """A bearer token for the Storage data plane, from the CLI's resolver."""
    cmd = ["az", "account", "get-access-token", "--resource", RESOURCE, "-o", "json"]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=120, check=True).stdout
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as err:
        raise SystemExit("could not resolve an Azure storage token: %s" % err)
    return json.loads(out)["accessToken"]


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
    return "&".join("%s=%s" % (percent_encode(k), percent_encode(query[k])) for k in sorted(query))


def request(method, key, query=None, headers=None, body=b""):
    """Send one bearer-authenticated Blob REST request; return (status, headers, body).

    `key` is the blob name; pass "" to address the container itself (listing).
    """
    if not ACCOUNT or not CONTAINER:
        raise SystemExit("set KYTHIRA_AZURE_STORAGE_ACCOUNT and KYTHIRA_AZURE_BLOB_CONTAINER")
    headers = {k: v for k, v in (headers or {}).items()}
    headers["Authorization"] = "Bearer " + TOKEN
    headers["x-ms-version"] = API_VERSION
    if method == "PUT" and "x-ms-blob-type" not in {k.lower(): k for k in headers}:
        headers["x-ms-blob-type"] = "BlockBlob"

    path = "/%s/%s" % (CONTAINER, percent_encode(key, keep_slash=True)) if key else "/" + CONTAINER
    url = "https://%s.blob.core.windows.net%s" % (ACCOUNT, path)
    qs = query_string(query)
    if qs:
        url += "?" + qs

    req = urllib.request.Request(url, data=body if body else None, method=method)
    for name, value in headers.items():
        req.add_header(name, value)
    # A PUT with no body still needs an explicit zero length.
    if method == "PUT" and not body:
        req.add_header("Content-Length", "0")

    # Connection-level failures are retried; HTTP statuses never are, because
    # the status *is* the measurement. Azure reset the connection mid-cleanup
    # on this probe's first live run, which lost the residual count — the one
    # number that says whether the probe left the bucket dirty. Each retry is
    # printed rather than swallowed, so a retried request is visible in the
    # transcript instead of quietly becoming one of the racers.
    last_error = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return resp.status, Headers(resp.headers), resp.read()
        except urllib.error.HTTPError as err:
            return err.code, Headers(err.headers), err.read()
        except (urllib.error.URLError, OSError) as err:
            last_error = err
            print("    [transport retry %d/2: %s %s: %s]" % (attempt + 1, method, key, err),
                  flush=True)
            time.sleep(1 + attempt)
    raise SystemExit("%s %s failed after 3 attempts: %s" % (method, key, last_error))


def code_of(headers, body=b""):
    """Azure's own error code.

    Read from the `x-ms-error-code` **response header**, which is present on
    every error and is the authoritative spelling; the XML body is the
    fallback. Reading the status alone would collapse exactly the distinctions
    these probes exist to make.
    """
    for name, value in headers.items():
        if name.lower() == "x-ms-error-code":
            return value
    text = body.decode("utf-8", "replace")
    start = text.find("<Code>")
    if start < 0:
        return ""
    end = text.find("</Code>", start)
    return text[start + 6 : end] if end > 0 else ""


def xml_values(body, element):
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


def report(label, status, headers, body, extra=""):
    print(
        "  %-46s HTTP %-3s %-26s %s"
        % (label, status, code_of(headers, body) or "-", extra or headers.get("ETag", "")),
        flush=True,
    )
