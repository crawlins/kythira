#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

#
# Report the remaining life of the runner's Actions ID-token request token, and
# whether an ID-token exchange still succeeds right now.
#
# Why this exists
# ---------------
# `gcp_privateca_provider_real_test` has failed on every scheduled real-cloud
# run since September 2026 with:
#
#   external_account_credentials.cc:394] Fetch external account credentials
#   access token: UNAVAILABLE: ... status 403
#   {"source":"actions-run-service",
#    "errorMessage":"runner does not have permissions to generate id token"}
#
# The message reads as a missing `permissions: id-token: write`. The job grants
# it, and the same credentials had already worked twice minutes earlier, so
# that reading is wrong and two fixes derived from it have already failed.
#
# What the job timings actually show (measured, not inferred):
#
#   run 34841010171 (Sep 14)  job start 11:59:46  privateca failed 13:24:55
#   run 35762998560 (Sep 22)  job start 17:49:13  privateca failed 19:14:12
#
# Both at 85 minutes after JOB START, within ten seconds of each other. The
# successful exchanges bound it from below: the GCS bundle exchanged at
# 68m50s and passed, and the quorum-manager bundle exchanged at ~69m40s and
# passed, in both runs. Nothing there is measured from the `auth` step, which
# is why re-authenticating before the tests changed nothing:
# google-github-actions/auth rewrites the credential file, but that file's
# `credential_source` carries ACTIONS_ID_TOKEN_REQUEST_TOKEN, which the runner
# sets once at job start and no action can refresh.
#
# That is a hypothesis about the request token's lifetime, and this script is
# here to stop it being a hypothesis. Called at several points in a job it
# prints the token's own `iat`/`exp` claims and the live HTTP status of an
# exchange, so the next run answers "when does it expire, and does it still
# work" with numbers instead of a third guess.
#
# Usage: probe-id-token.sh <label>
#
#   label  where in the job this probe runs, e.g. "after build".
#
# Env: ACTIONS_ID_TOKEN_REQUEST_URL and ACTIONS_ID_TOKEN_REQUEST_TOKEN, both
#      set by the runner in any job granting `permissions: id-token: write`.
#      PROBE_AUDIENCE overrides the audience requested (default
#      `kythira-id-token-probe`); the value is immaterial, because the 403
#      under investigation comes from the Actions service before any audience
#      is looked at.
#
# This script NEVER exits non-zero for a probe result. It is a diagnostic, and
# a diagnostic that can redden an otherwise-green run would be the third piece
# of machinery in this repo to report a failure it did not actually find. It
# also never prints a token: only claim timestamps, byte lengths and HTTP
# status codes. The bearer token and the minted ID token are both credentials.
#
# It does print a loud PROBE DID NOT RUN banner when it cannot measure, so an
# absent measurement is never mistaken for a clean one.
#

set -uo pipefail

LABEL="${1:-unlabelled}"

say() { echo "[id-token-probe: ${LABEL}] $*"; }

did_not_run() {
    say "*** PROBE DID NOT RUN — ${1} ***"
    say "No conclusion may be drawn from this probe. Check by hand whether the"
    say "job grants 'permissions: id-token: write' and whether the runner still"
    say "exports ACTIONS_ID_TOKEN_REQUEST_URL/_TOKEN."
    exit 0
}

# `date -u +%s` rather than bash's $EPOCHSECONDS so this runs under the /bin/sh
# a container step might hand it as well as the runner's bash.
now="$(date -u +%s)"
say "wall clock $(date -u -d "@${now}" +%Y-%m-%dT%H:%M:%SZ) (epoch ${now})"

if [[ -z "${ACTIONS_ID_TOKEN_REQUEST_URL:-}" || -z "${ACTIONS_ID_TOKEN_REQUEST_TOKEN:-}" ]]; then
    did_not_run "ACTIONS_ID_TOKEN_REQUEST_URL and/or _TOKEN are unset"
fi

# ── The request token's own claims ────────────────────────────────────────
#
# Decoded with base64 rather than verified: the point is the `exp` the Actions
# service will enforce, and reading it needs no key. If the value is not a JWT
# at all (GitHub has never promised it is one) say exactly that instead of
# printing a decode failure that looks like a measurement.
decode_claims() {
    local jwt="$1" payload
    payload="$(printf '%s' "${jwt}" | cut -d. -f2)"
    # cut echoes the whole string back when there is no delimiter, so an equal
    # result means the value carried no '.' and is not a JWT.
    [[ "${payload}" == "${jwt}" ]] && return 1
    [[ -z "${payload}" ]] && return 1
    # base64url drops the '=' padding, and `base64 -d` refuses input without
    # it. A base64 payload is never 1 mod 4, so those are the only two cases.
    case $(( ${#payload} % 4 )) in
        2) payload="${payload}==" ;;
        3) payload="${payload}=" ;;
    esac
    printf '%s' "${payload}" | tr '_-' '/+' | base64 -d 2>/dev/null
}

say "request token: ${#ACTIONS_ID_TOKEN_REQUEST_TOKEN} bytes"

if claims="$(decode_claims "${ACTIONS_ID_TOKEN_REQUEST_TOKEN}")" && [[ -n "${claims}" ]]; then
    iat="$(printf '%s' "${claims}" | jq -r '.iat // empty' 2>/dev/null)"
    exp="$(printf '%s' "${claims}" | jq -r '.exp // empty' 2>/dev/null)"
    nbf="$(printf '%s' "${claims}" | jq -r '.nbf // empty' 2>/dev/null)"
    # Claim NAMES only. The values of scp/sub/iss on a runner token describe the
    # repository and are not secret, but there is no question here they answer.
    say "request token claims present: $(printf '%s' "${claims}" | jq -r 'keys | join(",")' 2>/dev/null)"
    if [[ -n "${iat}" && -n "${exp}" ]]; then
        say "request token iat=${iat} ($(date -u -d "@${iat}" +%H:%M:%SZ))" \
            "exp=${exp} ($(date -u -d "@${exp}" +%H:%M:%SZ))"
        say "request token total lifetime $(( exp - iat ))s ($(( (exp - iat) / 60 ))m)"
        say "request token REMAINING $(( exp - now ))s ($(( (exp - now) / 60 ))m)"
        [[ -n "${nbf}" ]] && say "request token nbf=${nbf}"
        if [[ $(( exp - now )) -le 0 ]]; then
            say "^^ the request token is ALREADY EXPIRED at this point in the job."
        fi
    else
        say "request token carries no iat/exp claims — its lifetime is not readable here"
    fi
else
    say "request token is not a readable JWT — its lifetime is not readable here"
fi

# ── Does an exchange work right now? ──────────────────────────────────────
#
# The live half of the probe. A claim says when the token is meant to expire;
# this says whether the service still honours it, which is the thing that
# actually fails. -f is deliberately absent: a 403 body carries the
# actions-run-service errorMessage under investigation and must be printed, not
# swallowed. (curl without -f is what let the Alibaba audit write "Not Found"
# into a tarball and exit 0 — here the status code is read explicitly instead.)
audience="${PROBE_AUDIENCE:-kythira-id-token-probe}"
body="$(mktemp)"
trap 'rm -f "${body}"' EXIT

code="$(curl -sS -o "${body}" -w '%{http_code}' --max-time 30 \
    -H "Authorization: Bearer ${ACTIONS_ID_TOKEN_REQUEST_TOKEN}" \
    -H "Accept: application/json; api-version=2.0" \
    "${ACTIONS_ID_TOKEN_REQUEST_URL}&audience=${audience}" 2>&1)" || code="curl-failed"

say "live exchange HTTP ${code}"

if [[ "${code}" == "200" ]]; then
    # The response holds a real ID token. Print its lifetime, never its value.
    token="$(jq -r '.value // empty' < "${body}" 2>/dev/null)"
    if [[ -n "${token}" ]] && tclaims="$(decode_claims "${token}")" && [[ -n "${tclaims}" ]]; then
        tiat="$(printf '%s' "${tclaims}" | jq -r '.iat // empty' 2>/dev/null)"
        texp="$(printf '%s' "${tclaims}" | jq -r '.exp // empty' 2>/dev/null)"
        if [[ -n "${tiat}" && -n "${texp}" ]]; then
            say "minted ID token lifetime $(( texp - tiat ))s, expires $(date -u -d "@${texp}" +%H:%M:%SZ)"
        fi
    else
        say "minted ID token could not be decoded (${#token} bytes) — not a failure, just unreadable"
    fi
    say "RESULT: the exchange still works at this point in the job."
else
    # Not a secret: this body is the error document, and it is the whole reason
    # the probe exists. Truncated because a proxy error page can be enormous.
    say "RESULT: the exchange FAILED here. Response body (first 400 bytes):"
    head -c 400 "${body}" | sed "s/^/[id-token-probe: ${LABEL}] /"
    echo
fi

# ── Control: the same request with NO Authorization header ────────────────
#
# Added after run 36035268163 refuted the lifetime hypothesis this script was
# written to test. That run measured, in the same second (epoch 1790276266):
#
#   this probe's exchange             HTTP 200, ID token minted, 286 minutes
#                                     of request-token life remaining
#   gcp_privateca_provider_real_test  403 "runner does not have permissions
#                                     to generate id token"
#
# Same runner, same request token, same instant. Whatever separates them, it
# is not the token's validity. The surviving difference is the caller: the
# bundles that pass (GCS, Compute) go over google-cloud-cpp's REST path, and
# the one that fails is gRPC-backed, failing inside gRPC C-core's own
# `external_account_credentials.cc`.
#
# So the question becomes what gRPC's fetcher sends that curl does not, and
# the cheapest discriminator is this: an UNAUTHENTICATED request to the
# Actions token endpoint is refused with exactly the wording under
# investigation. If the line below reports 403 with the same errorMessage,
# then "runner does not have permissions" means "this request arrived with no
# credential" -- and a fetcher that drops `credential_source.headers` would
# produce precisely the observed failure while every REST client succeeds.
#
# It is a CONTROL, not a conclusion. It establishes what an unauthenticated
# request looks like. It does not prove that is what gRPC sends.
nobody="$(mktemp)"
trap 'rm -f "${body}" "${nobody}"' EXIT

nocode="$(curl -sS -o "${nobody}" -w '%{http_code}' --max-time 30 \
    -H "Accept: application/json; api-version=2.0" \
    "${ACTIONS_ID_TOKEN_REQUEST_URL}&audience=${audience}" 2>&1)" || nocode="curl-failed"

say "CONTROL - same request with NO Authorization header: HTTP ${nocode}"
if [[ "${nocode}" != "200" ]]; then
    say "CONTROL body (first 400 bytes):"
    head -c 400 "${nobody}" | sed "s/^/[id-token-probe: ${LABEL}] control: /"
    echo
fi
say "Compare that body with the privateca failure's. Identical wording means"
say "the 403 under investigation is what an unauthenticated request looks like."

exit 0
