# grpc overlay port

Shadows vcpkg's builtin `grpc` port to carry one patch:
`00100-url-external-account-preserve-query.patch`.

## Why

`gcp_privateca_provider_real_test` failed in CI for months with

```
403 {"source":"actions-run-service",
     "errorMessage":"runner does not have permissions to generate id token"}
```

raised inside gRPC's own `external_account_credentials.cc`. It reads as a
credential or IAM problem and is not one.

`UrlExternalAccountCredentials` folds the credential source URL's query string
into what it then passes to `URI::Create` as the *path*, with query parameters
explicitly empty. The wire request target comes from that URI —
`httpcli.cc` uses `uri.EncodedPathAndQueryParams()`, not `request.path` — so the
`?`, `&` and `%` inside the "path" get percent-encoded, and GitHub's OIDC token
endpoint receives one mangled path segment with **no query parameters at all**.
`audience` and `api-version` never arrive.

The patch takes the path and the query from `url_`, which is already
`URI::Parse` of the same string, so each query value is encoded once instead of
the delimiters of an already-assembled query being encoded.

## How it was found

Five hypotheses died first — a closed GCP billing account, a stale ID-token
request token, gRPC dropping `credential_source.headers`, a missing
`Accept: api-version=2.0` header, and a malformed credential file. Each is
recorded with its refuting evidence in `scripts/ci-cloud-credentials/gcp/probe-id-token.sh`,
whose Control B and Control C are what localised it: Control C replays the
credential file's own URL and headers with curl and gets **200**, which proves
the configuration sound and moves the fault into the caller.

Two details explain why it survived so long:

- It is **403, not 401**. The `Authorization` header is sent and is valid; the
  control for an unauthenticated request returns 401. So every reading of the
  error pointed at permissions.
- Only gRPC-backed clients fail. google-cloud-cpp's REST path uses libcurl, so
  the GCS and Compute bundles pass against the same endpoint with the same
  token in the same job.

## Maintenance

- **Pinned to 1.71.0 deliberately**, matching `vcpkg.json`'s `builtin-baseline`.
  Overlay ports bypass the baseline, so taking the newer builtin port would have
  bumped gRPC five minor versions as a side effect. The port files here were
  extracted from vcpkg at that baseline commit, unmodified apart from the added
  patch and a `port-version` bump.
- `port-version` is 4, one above the baseline's 3, so vcpkg rebuilds rather than
  reusing a cached binary that lacks the patch.
- **The bug is still present in v1.78.1.** Upgrading gRPC will not remove the
  need for this patch; check upstream before assuming a newer version fixes it.
- If this is ever fixed upstream, delete the whole directory and the
  `./vcpkg-overlays/grpc` entry in `vcpkg-configuration.json` — there is nothing
  else here worth keeping.
