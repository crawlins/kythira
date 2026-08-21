#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The one rule every object-store probe in this directory encodes.

**Read the status code, not the effect.** The first version of the OSS probe
asked "did the stale write change the bytes?", saw that it had not, and
reported "CAS works". It does not: the write was refused `400 NotImplemented`,
so no precondition was ever evaluated. An effect-based check cannot tell a
precondition that was *evaluated and rejected* from a header the service never
implemented, and that run would have written a fence that does not exist into
the design.

`probe_alibaba_oss.py` and `probe_aws_s3.py` predate this module and carry
their own copy of the rule. They are left exactly as they were run on
August 16, 2026 rather than refactored into it unverified — a probe's value is
that a specific text produced a specific finding.
"""

# Codes that mean "the service does not implement this header", as opposed to
# "the service evaluated it and said no". The distinction is the whole point:
# the first makes a fence impossible (Requirement 9.8 → compile error), the
# second makes it usable.
UNSUPPORTED_CODES = ("NotImplemented", "NotSupported", "UnsupportedHeader",
                     "InvalidHeaderValue", "FeatureNotSupported")


class Headers(dict):
    """Response headers that answer to any case, because services differ.

    S3 and Azure return `ETag`; **OCI returns `etag`**, and a plain dict lookup
    for `"ETag"` therefore silently yields `""` — which this probe suite then
    sent as `if-match: ""`. The service dutifully answered `412 IfMatchFailed`
    to a *correct*-ETag write, which read exactly like "OCI cannot do CAS".
    Only the negative control caught it: a probe that had asserted the stale
    write's 412 and stopped would have recorded a false finding about a
    provider's fencing capability.
    """

    def get(self, key, default=None):  # noqa: A003 - dict interface
        lowered = key.lower()
        for name, value in self.items():
            if name.lower() == lowered:
                return value
        return default

    def __contains__(self, key):
        return self.get(key) is not None


# Every provider spells a checksum mismatch differently — five spellings across
# five services, which is why no probe may key its verdict on "any 4xx" and no
# client may key on the status alone.
CHECKSUM_MISMATCH_CODES = (
    "BadDigest",             # AWS S3
    "InvalidDigest",         # Alibaba OSS
    "Md5Mismatch",           # Azure Blob
    "invalid",               # GCS (JSON API `reason`)
    "UnmatchedContentMD5",   # OCI Object Storage
)


def classify_precondition(status, code, landed_bytes, bytes_if_write_did_not_happen):
    """Classify one conditional write from its status and error code.

    :param status: HTTP status of the *conditional* request.
    :param code:   the service's own error code, read from wherever that
                   service puts it — never inferred from the status.
    :param landed_bytes: what a GET returns now.
    :param bytes_if_write_did_not_happen: what would still be stored had the
                   write been refused, i.e. the **previous** content. Naming it
                   this way is deliberate: a check whose two byte-vector
                   arguments can be transposed without a type error eventually
                   will be, and that transposition is exactly how this rule
                   once labelled S3's working CAS "silently ignored".
    """
    if status in (409, 412):
        return "ENFORCED (rejected %s %s)" % (status, code or "-")
    if status in (400, 501) and code in UNSUPPORTED_CODES:
        return "UNSUPPORTED (%s %s) — the header is not implemented, not evaluated" % (status, code)
    if 200 <= status < 300 and landed_bytes == bytes_if_write_did_not_happen:
        return "SILENTLY IGNORED (%s) — accepted and disregarded" % status
    if 200 <= status < 300:
        return "ACCEPTED (%s)" % status
    return "UNEXPECTED (%s %s)" % (status, code or "-")


def cas_verdict(correct_status, stale_status):
    """The negative control matters as much as the rejection.

    A stale precondition being refused proves nothing about a fence unless a
    *correct* one is accepted — a service that refuses every conditional write
    would otherwise read as "CAS works".
    """
    if correct_status < 300 and stale_status in (409, 412):
        return "USABLE"
    if correct_status >= 300:
        return "NOT USABLE — even a correct precondition was refused (HTTP %s)" % correct_status
    return "NOT USABLE — a stale precondition was not refused (HTTP %s); Requirement 9.8 applies" % (
        stale_status,
    )
