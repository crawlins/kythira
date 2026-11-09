# Implementation Plan — IAM-Compatible Authentication and Authorization

## Status: 13/26 tasks complete

**Last Updated**: November 2, 2026. The spec was written from the scoping
doc "IAM for Kythira — scoping" and Clark's decisions of the same day. It
was amended the same day to put IAM in a `services/iam/` module, following
the `redis-gateway-module` spec (PR #675), at Clark's request.
Amended November 6, 2026, at Clark's request ("Add STS support to the IAM
spec"): task 26 completes STS with GetFederationToken,
DecodeAuthorizationMessage and AWS's limits on temporary credentials.
Amended November 8, 2026, at Clark's request: `invariants.md` catalogues
the rules AWS's IAM and STS docs require, with the test or owning task for
each. Tasks below name the invariants they own.

## Overview

First, make Kythira an OIDC issuer so its nodes get cloud credentials
without stored keys (tasks 1–5). Then build an AWS IAM-compatible service:
- SigV4 verification (tasks 6–7);
- the IAM state machine and daemon (tasks 8–9);
- the policy engine (tasks 10–12);
- the authorizer hook (task 13);
- the IAM Query API (tasks 14–16);
- STS and federation (tasks 17–20, and 26 for the remaining STS calls).

The Redis gateway is the first consumer (tasks 21–22). Conformance and docs
close it out (tasks 23–25).

**Module layout** (design "Module layout", Requirement 17). IAM lives in
`services/iam/`. Shared pieces stay in core `include/raft/`: the JWT helpers,
the SigV4 verifier, the Query codec, the cloud credential sources and the
`aws_request_authorizer` concept. The work depends on these
`redis-gateway-module` tasks:
- Tasks 1–7 are core-only and wait on nothing.
- The first task to add files under `services/iam/` (8 or 10) needs its
  tasks 2 and 3 merged: the core INTERFACE targets, the shared test
  helpers, the boundary check and `services/README.md`. That IAM task also
  creates the module scaffolding: `services/iam/CMakeLists.txt`, the
  `Kconfig` `IAM` symbol `source`d from the top level, `boundary-allow.txt`,
  `README.md`, the top-level `add_subdirectory`, and an `iam-module.yml`
  workflow that at first runs only the boundary check and the module-on and
  module-off builds.
- Tasks 21–22 need its tasks 4 (the move) and 6 (the `credential_verifier`
  seam).

Each task is one PR unless it says otherwise. Kinesis and DynamoDB start
after task 13.

Reference material to read before starting:
- `.kiro/specs/iam-compatible-auth/design.md`
- `include/raft/ca_state_machine.hpp`: sealing, unseal, typed commands
- `cmd/ca_cluster_node/main.cpp`: mTLS client auth, leader forwarding, routes
- `include/raft/ca_http_helpers.hpp`: reserved SANs, `peer_identity_name`
- `include/raft/redis_acl.hpp`, `redis_auth_limiter.hpp`,
  `redis_gateway_impl.hpp`: AUTH and the ACL format
- `include/raft/cache_http_front_end.hpp`: the Beast listener to reuse
- `include/raft/raft.hpp`: `add_learner()`
- AWS IAM User Guide, "Policy evaluation logic" and "IAM JSON policy
  elements reference"; AWS SigV4 documentation and test suite

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": [1, 6, 8, 10], "description": "Independent foundations: issuer keys and SigV4 parsing (core, start now); IAM state machine and policy parser (services/iam, after redis-gateway-module tasks 2-3)" },
    { "wave": 2, "tasks": [2, 7, 9, 11], "description": "Token endpoint, streaming SigV4, iam_node daemon, condition operators" },
    { "wave": 3, "tasks": [3, 12], "description": "AWS and GCP exchange providers; the evaluator and corpus" },
    { "wave": 4, "tasks": [4, 13], "description": "Azure, Alibaba and OCI providers; authorizer hook and replicas" },
    { "wave": 5, "tasks": [5, 14, 17], "description": "Real-cloud federation run; Query API core; STS core" },
    { "wave": 6, "tasks": [15, 18, 21], "description": "Roles and policies API; STS session features; Redis IAM AUTH" },
    { "wave": 7, "tasks": [16, 19, 22, 26], "description": "OIDC provider and simulation APIs; web identity; Redis lifetime and modes; GetFederationToken and DecodeAuthorizationMessage" },
    { "wave": 8, "tasks": [20, 23], "description": "Kythira-CA web identity; CLI, SDK and Redis conformance" },
    { "wave": 9, "tasks": [24], "description": "Terraform conformance and optional AWS simulator comparison" },
    { "wave": 10, "tasks": [25], "description": "Docs and close-out" }
  ]
}
```

Task 5 needs Clark's go (real cloud). Task 15's call recording against real
AWS and task 24's simulator run each need his go too. They are optional,
because each has a zero-spend fallback.

## Tasks

### Phase A — OIDC issuer and outbound federation

- [x] 1. Issuer signing keys and discovery
  - `include/raft/jwt.hpp`: base64url and compact JWS sign and verify for
    RS256 and ES256, with the ES256 DER↔raw conversion. Unit tests use
    RFC 7515 and RFC 7520 vectors.
  - `ca_state_machine`: `record_oidc_key` and `retire_oidc_key`, sealed
    private key, optional snapshot field with old-snapshot compatibility.
  - `ca_cluster_node`:
    - the leader rotation timer;
    - `GET /.well-known/openid-configuration` and `/.well-known/jwks.json`,
      served from any member.
  - Tests:
    - rotation timeline with an injected clock: publish 24 h before signing,
      retain the old key after its last use;
    - determinism across a snapshot restore.
  - _Requirements: 1.1–1.5, 1.10_
  - Done November 3, 2026. Notes from the implementation:
    - `include/raft/oidc_issuer.hpp` holds the key record and a pure
      rotation planner (`plan_oidc_rotation`), so the timeline test runs a
      year of hourly ticks against the real state machine in milliseconds.
    - The commands carry a leader-stamped `now`, and the state keeps the
      latest one (`oidc_clock`) so a leader with a trailing clock cannot move
      time back. A snapshot that drops a key is accepted only if that key
      was retiring and the snapshot's clock has passed its `retire_after`.
    - The JWKS filters out keys past `retire_after` itself, because the
      replicated state only prunes them at the next OIDC command, which is
      usually the next rotation.
    - The `kid` is the RFC 7638 thumbprint of the public key.
    - Followers redirect discovery and JWKS to the leader (Requirement 1.10
      amended): `node<Types>` has no local read, only the linearisable
      `read_state()`, which followers cannot serve.

- [x] 2. Token endpoint and bucket publishing
  - `POST /v1/oidc/token`:
    - client certificate required, revocation checked;
    - subject from a node SAN or a workload URI SAN;
    - audience allowlist and lifetime cap;
    - follower forwarding.
  - `--oidc-publish-url`: upload discovery and JWKS through the existing
    object-store clients after each key change, with a failure metric.
  - Tests: wrong CA, revoked cert, unlisted audience, lifetime past the cert,
    and a workload SAN outside the prefix are each refused. A token minted
    here verifies with an independent library (Python `PyJWT` in the test
    container).
  - _Requirements: 1.6–1.9_
  - Done November 5, 2026, in two PRs: the token endpoint (#694), then
    bucket publishing. Notes from the implementation:
    - `include/raft/oidc_token.hpp` holds the whole decision
      (`decide_token`) as a pure function of the certificate facts, the
      chain and revocation results, the body and `now`. The handler only
      gathers those and signs, so `oidc_token_unit_test` drives every
      refusal against real CA-issued certificates.
    - Flags: `--oidc-audience` (repeatable; none refuses every request) and
      `--oidc-workload-uri-prefix`, which must end in `/` or `:` so a prefix
      cannot match a longer sibling, and must not overlap the reserved
      `urn:kythira:node:` namespace. The lifetime cap is the existing
      `--oidc-max-token-lifetime-secs`, which is also how long a superseded
      key stays published, so no token outlives its key.
    - The route skips the bearer-token gate: a workload holding only its own
      certificate must not need the operator's token.
    - Status codes: 401 for no certificate, wrong CA, revoked or expired;
      403 for a valid certificate naming no eligible subject (criterion
      1.6); 400 for the body, the audience (`invalid_target`) or the
      lifetime. Authentication is checked first, so an unauthenticated
      caller learns nothing about the body rules.
    - Lifetime: an explicit `lifetime` past the certificate's notAfter is
      refused; with none, the 1-hour default is cut to the certificate's
      remaining validity.
    - Subject: a cluster member's reserved peer SAN gives `node:<id>`; a
      certificate naming two members, or two workload URIs, is refused.
      Workload paths with dot segments, `?` or `#` are refused, so the
      subject a cloud trust policy matches is the one the CA issued.
    - "Follower forwarding" is the 308 redirect the other `/v1/*` routes
      use; the client presents its certificate again to the leader.
    - Unsealed signing keys are cached by `kid`, so the unseal key
      derivation runs once per key, not once per token.
    - `ca_cluster_node_test`'s `oidc_token_endpoint_over_mtls` gets a token
      over the TLS listener and verifies it with PyJWT
      (`tests/oidc_pyjwt_verify.py`, `python3-jwt` in the Build & Test
      jobs). It reports a skip, not a pass, where PyJWT is missing.
    - Publishing: the issuer URL is the bucket's public HTTPS URL and
      `--oidc-publish-url` the same place in the store's scheme, so `iss`,
      the discovery URL and `jwks_uri` never diverge and the discovery
      document needs no separate `jwks_uri` setting.
    - The trigger is "the JWKS differs from the last upload", not "a key
      command committed": a successor appears at its `publish_after` and a
      retiring key drops out at its `retire_after` with no command applied.
      A six-hour refresh restores deleted objects; failures back off from
      5 s to 5 min (`include/raft/oidc_publisher.hpp`, pure, unit tested).
    - The JWKS is uploaded before the discovery document, and nothing is
      uploaded while the key set is empty.
    - Uploads run on their own thread on the leader, so a hung object-store
      call cannot delay key rotation or certificate renewal.
    - Providers follow `cmd/raft_object_backup`: compiled in when the
      dependency is found and the persistence Kconfig gate is on. A node
      asked for a missing provider refuses to start and names what would
      enable it.
    - The failure metric is `kythira_oidc_publish_failures_total` on a new
      bearer-gated `GET /metrics` (Prometheus text), with success and
      last-success gauges beside it. ca_cluster_node had no metrics
      endpoint before.
    - The object-store clients set no content type. Whether each cloud
      accepts the store's default is left to the task 5 real-cloud run.
    - Operator documentation: `doc/oidc-issuer.md`.

- [x] 3. Token source plus AWS and GCP exchange providers
  - `kythira_oidc_token_source`: per-audience cache, refresh at 80% of the
    lifetime, one shared in-flight refresh.
  - The `cloud_credential_source` concept.
  - AWS `AssumeRoleWithWebIdentity` provider plus an `AWSCredentialsProvider`
    adapter for `aws_s3_client` and the ACM-PCA provider.
  - GCP STS token exchange plus optional impersonation, wired into
    `gcp_gcs_client`.
  - Tests use local mock token endpoints that check the exact request
    format: grant type, audience, `subject_token_type`.
  - _Requirements: 2.1–2.4_
  - Done November 5, 2026, in two parts. Part 1: the token source and the
    AWS and GCP exchanges in `include/raft/cloud_credential_exchange.hpp`.
    Part 2: the SDK wiring. Notes:
    - The source interface is synchronous (`current()` returns the
      credentials), not the design's future-returning concept: every
      consumer is an SDK credential hook such as
      `AWSCredentialsProvider::GetAWSCredentials`, which is synchronous
      itself, and the hot path is a cache hit under one mutex.
    - One caching rule for everything (`refreshing_value`): refresh after
      80% of the lifetime; while one caller refreshes, the others keep the
      still-valid value; a caller with nothing valid waits for the single
      in-flight fetch; a failed refresh keeps the old value until it
      really expires and backs off (1 s doubling to 1 min). The spec's
      "`async_retry` backoff" names nothing that exists in the repository,
      so this is that backoff.
    - The mTLS token fetch follows a follower's 308 itself: httplib's own
      redirect builds a new client for the other host without the client
      certificate, so the leader would see an anonymous request.
    - AWS: the Query call is unsigned, as STS documents for
      `AssumeRoleWithWebIdentity`; the regional endpoint is configurable.
      GCP: the OIDC audience is the Workload Identity provider's full
      resource name, and impersonation is optional.
    - `cloud_credential_exchange_test` checks every field of each request
      against local mock endpoints, including the grant type, audience
      and `subject_token_type`, and passes under ASan and TSan.
    - AWS wiring (`include/raft/aws_web_identity_credentials.hpp`): every
      AWS component already takes `aws_client_config::credentials_provider`,
      so one `AWSCredentialsProviderChain` subclass whose only link asks the
      web-identity source covers `aws_s3_client` and the ACM-PCA provider
      (and the EC2 and ASG components). A failed exchange gives the SDK
      empty credentials and logs why, the SDK's own convention, rather than
      throwing through SDK frames.
    - GCP wiring: `gcp_client_config::access_token`, a function returning
      the current token. google-cloud-cpp has no public hook for
      credentials that refresh themselves, only a fixed access token, so
      `gcp_gcs_client` rebuilds its SDK client whenever the token changes
      (about hourly). Only the GCS client reads the field so far; the
      Compute, MIG and Private CA components build their clients once.
    - `aws_s3_client_unit_test` and `gcp_gcs_client_unit_test` check the
      request the store receives: SigV4 `Credential=` plus
      `x-amz-security-token` from the assumed role, and the `Bearer` token
      from the GCP exchange, including a token change between requests.

- [x] 4. Azure, Alibaba and OCI exchange providers
  - Azure: client-credentials flow with a federated `client_assertion`, wired
    into `azure_blob_client`.
  - Alibaba: `AssumeRoleWithOIDC`, wired into `alibaba_oss_client` (STS
    token header).
  - OCI: identity-domain token exchange to a UPST with a session key pair,
    signed through `oci_signing.hpp`, wired into
    `oci_object_storage_client`. First confirm the exchange exists for the
    tenancy type the project uses (design Risks). If it does not, record that
    here and drop this sub-item.
  - Mock-endpoint tests as in task 3.
  - _Requirements: 2.2–2.4_
  - Done November 5, 2026, in `include/raft/cloud_credential_exchange.hpp`:
    - `azure_federated_source` posts `client_credentials` with the Kythira
      token as a `jwt-bearer` `client_assertion` (audience
      `api://AzureADTokenExchange`) and caches one token per resource
      scope. `azure_federated_token_credential`
      (`include/raft/azure_federated_credential.hpp`) is the SDK
      `TokenCredential`, so it covers every Azure component through
      `azure_client_config::credential`, not only `azure_blob_client`.
    - `alibaba_oidc_role_source` calls the anonymous `AssumeRoleWithOIDC`
      RPC. `alibaba_client_config::credentials` is a new hook both signers
      read per request (OSS V4 and the ACS3 control plane), so the ESS
      components are covered too.
    - OCI: the exchange exists for this project's tenancy, which is on an
      identity domain. CI's OCI jobs have used the same grant with GitHub's
      token since the WIF work. `oci_upst_source` generates a session key
      per exchange, posts the token-exchange grant with the public key, and
      reads the expiry from the UPST's `exp`. `oci_signing::sign_request`
      reads the new `oci_client_config::session_credentials` hook. One
      difference from the other clouds: the exchange authenticates as the
      trust's confidential application (client id and secret over HTTP
      Basic), so that secret is stored. It can exchange tokens and has no
      OCI permissions.
    - Tests: wire format, caching and error text for each exchange against
      mock endpoints, the Alibaba and OCI signers reading exchanged
      credentials, and `azure_blob_client` sending the exchanged Entra
      token (and sending nothing when the exchange is refused).

- [x] 5. Real-cloud federation run (needs Clark's go)
  - A workflow sets up the issuer's trust in each cloud: an IAM OIDC provider
    and role, a GCP pool, an Entra app with a federated credential, an
    Alibaba OIDC provider and role, and the OCI trust if task 4 kept it.
    Each is scoped to read one tagged test object.
  - It publishes discovery to a bucket, runs one read per cloud through the
    providers, and tears everything down.
  - Follow `kythira-real-cloud-test-rules`: tag every resource, run the leak
    audit, record the spend. IAM-type resources are free; the bucket costs
    cents.
  - _Requirements: 2.5_
  - Done. Clark gave his go on November 5, 2026, and chose a
    one-time admin setup over per-run creation: the AWS CI role's
    permissions boundary denies every IAM action, and the GCP CI service
    account may not create workload identity pools, so a workflow cannot
    create the trust itself. Pieces:
    - `scripts/ci-cloud-credentials/aws/provision-oidc-federation-test.sh`:
      the issuer bucket (only `<prefix>/.well-known/*` public, the CI role
      writes there through the bucket policy), the test object, the IAM
      OIDC provider, and a role that trusts only
      `aud=sts.amazonaws.com`, `sub=workload:<uri>` and may read only the
      test object.
    - `scripts/ci-cloud-credentials/gcp/provision-oidc-federation-test.sh`:
      a pool and provider trusting the same issuer with the same subject
      condition, and direct federated `storage.objectViewer` on a bucket
      holding only the test object.
    - `.github/workflows/oidc-federation.yml` runs
      `tests/oidc_federation_real_test.cpp`: one `ca_cluster_node`
      publishes to the bucket, a workload certificate is exchanged for a
      JWT, and the JWT for AWS and GCP credentials that read the test
      object through `aws_s3_client` and `gcp_gcs_client`. A run creates
      only the two published objects and deletes them, then checks none
      are left.
    - The `ca_cluster_node` spawning helpers moved from
      `ca_cluster_node_test.cpp` to `tests/ca_cluster_node_harness.hpp` so
      both tests share them.
    - Azure, Alibaba and OCI join after task 4. Their one-time setup scripts
      landed November 6, 2026, with the test's three new legs and the
      workflow's `azure`, `alibaba` and `oci` inputs (default off until the
      setup has been run):
      - `azure/provision-oidc-federation-test.sh`: an app registration with
        no secret and one federated identity credential (the issuer,
        `workload:<uri>`, audience `api://AzureADTokenExchange`), and
        Storage Blob Data Reader on a container holding only the test
        object.
      - `alibaba/provision-oidc-federation-test.sh`: a RAM OIDC provider
        (client id `sts.aliyuncs.com`, every certificate in the issuer
        host's chain as a fingerprint, so an S3 leaf rotation does not
        break it), a role pinning `oidc:iss`, `oidc:aud` and `oidc:sub`, and
        a policy reading only the test object.
      - `oci/provision-oidc-federation-test.sh`: a service user and group, an
        identity propagation trust (the issuer's JWKS, CI's existing
        token-exchange confidential app as its client, rule
        `sub eq workload:<uri>`), and a policy reading only the test
        bucket. It reuses the confidential app, so the run needs no new
        secret.
    - AWS and GCP passed on November 5, 2026 (run 42097505918, 20/20
      assertions). The issuer documents went public under
      `kythira-oidc-fed-827617851594/ci`, the S3 test object was read
      through `AssumeRoleWithWebIdentity`, and the GCS test object through
      the GCP STS exchange. The leak audit found nothing left. Spend: two
      S3 PUTs and DELETEs, a few GETs, and one GCS read, well under a cent.
      Two earlier runs failed and were fixed: the build cache's Oracle
      `AWS_ENDPOINT_URL` sent the STS call to Oracle, and `kid_set()`
      walked a freed JSON temporary.
    - All five clouds passed on November 6, 2026 (run 42254841313, 27/27
      assertions), after Clark ran the Azure, Alibaba and OCI setup from
      his machine. The workflow's `azure`, `alibaba` and `oci` inputs now
      default on. Spend: the same two S3 objects plus one read each from
      Blob, OSS and Object Storage, well under a cent. Found on the way:
      - Alibaba's `CreateOIDCProvider` fetches the discovery document when
        it is called, and the issuer prefix is empty between runs, so the
        setup published throwaway issuer documents first and deleted them
        after. The script now checks for the document and says so.
      - Alibaba caches the issuer's JWKS, and every run signs with a fresh
        key, so a run soon after another is refused with
        `IssuerConfigurationBroken` for up to a minute; the test retries
        that refusal for up to ten minutes.
      - The `OCI_DOMAIN_URL` secret ends in a newline; the test trims the
        values it reads (but not the expected object body).

### Phase B — IAM core

- [x] 6. SigV4 parsing and verification
  - `include/raft/sigv4_verifier.hpp`:
    - header and presigned forms;
    - the service rules (including the S3 path rules);
    - scope, skew and expiry checks;
    - constant-time comparison;
    - the bounded signing-key cache;
    - the AWS error mapping.
  - Vendor AWS's `aws-sig-v4-test-suite` under core `tests/sigv4_suite/` with
    its licence. CLAUDE.md treats it as third-party, so it gets no header.
    Every case must pass.
  - _Requirements: 3.1–3.3, 3.5–3.7_
  - Done November 3, 2026. Notes from the implementation:
    - The suite is AWS's current copy, from `awslabs/aws-c-auth`
      (`tests/aws-signing-test-suite/v4`, 38 cases), vendored with its
      LICENSE and NOTICE. Each case is checked in both its header and
      presigned forms: canonical request, string to sign and signature.
    - `"normalize": false` cases run under `s3_path_rules`. That flag now
      covers only the path rule; the other S3 behaviours (content hash
      header required, presigned `UNSIGNED-PAYLOAD`, signed session token)
      follow `error_protocol::s3`.
    - The session token may be unsigned outside S3, as AWS documents and
      the suite's `post-sts-header-after` case shows. For the presigned form
      the verifier also accepts the signature over the query without the
      token, computing both candidates every time.
    - For an unknown key, the JSON protocol answers
      `UnrecognizedClientException`, as Kinesis and DynamoDB do, rather than
      `InvalidClientTokenId` (Requirement 3.7's fidelity rule wins).
    - The result type is a small `parse_result`, not `std::expected`, which
      clang 18 over libstdc++ 13 lacks.

- [x] 7. Streaming payloads and failure rate limiting
  - `sigv4_chunk_decoder`: `STREAMING-AWS4-HMAC-SHA256-PAYLOAD`, the
    unsigned and signed trailer forms, and per-chunk verification before
    release.
  - Vectors are generated by a pinned botocore script, which is checked in
    with its outputs.
  - Failure limiter keyed by source address and access key id, modelled on
    `redis_auth_limiter`.
  - _Requirements: 3.4, 3.8_
  - Done November 6, 2026:
    - `include/raft/sigv4_chunk_decoder.hpp`: `sigv4::chunk_decoder`, a
      push parser for all three streaming forms. It appends a chunk's
      bytes to the caller's buffer only once the whole chunk is in and,
      in the signed forms, its signature (chained from the seed) has
      verified. Trailers must match `x-amz-trailer` exactly, and the
      signed-trailer form needs `x-amz-trailer-signature`. The decoded
      length is checked against `x-amz-decoded-content-length`. Chunk
      size, line length and trailer size are bounded by `chunk_limits`.
      Trailer checksum values go to the front end (`trailers()`), since
      the S3 layer knows which algorithms it supports. S3's 8 KiB
      minimum chunk is not enforced.
    - `include/raft/sigv4_failure_limiter.hpp`: `sigv4::failure_limiter`,
      with per-window failure budgets per source (IPv4 address or IPv6
      /64) and per access key id, and a bounded table that never evicts
      the key being charged. A failure is charged only after it happens
      (a SigV4 check is a few HMACs, not a KDF, so no concurrency cap).
      The per-key budget defaults to five times the per-source one,
      because key ids are public and anyone can spend a key's budget.
      `throttled()` gives each protocol's throttling error.
    - `tests/sigv4_chunked/`: `gen_vectors.py` with botocore pinned in
      `requirements.txt`, and its output `vectors.json` (nine cases:
      signed, signed-trailer, unsigned-trailer and presigned). botocore
      signs every seed and writes the unsigned-trailer bodies itself. It
      never signs chunks, so the script computes chunk and trailer
      signatures from botocore's signing key, as a second implementation
      next to the C++ one.
    - `tests/sigv4_chunk_decoder_test.cpp`, 19 cases:
      - every vector decodes whole, byte by byte and at random cuts;
      - every truncation, any flipped byte of a signed body, reordered or
        dropped chunks and a wrong key all fail, and a flip inside the
        first chunk releases nothing;
      - the limiter tests.
    - The presigned vectors found a task 6 bug: the S3 canonical path
      escaped RFC 3986 sub-delims (`+`, `=`, `,` and the rest) that
      clients sign raw, so a botocore URL for `/a+b` failed to verify.
      `s3_canonical_path` now keeps pchar characters.

- [x] 8. `iam_state_machine`
  - The command enum and JSON bodies of design Component 4. Leader-side
    stamping helpers.
  - `apply()` re-checks the preconditions that can race.
  - Per-account maps, the global access-key index, quotas, unique-id
    generation, case-insensitive names, and the sealed-secret format.
  - A generated header for the AWS-managed policy subset, from
    `services/iam/tools/aws_managed_policies.json`.
  - Snapshot round trip with a version.
  - Lives in `services/iam/include/kythira/iam/`, with tests in
    `services/iam/tests/`. If task 10 has not landed first, this task
    creates the module scaffolding (see Overview).
  - Property tests:
    - random command sequences give identical `get_state()` on two
      instances and after a restore;
    - no plaintext secret appears in the log or snapshot bytes.
  - _Requirements: 4.1–4.5, 4.7, 4.8_
  - Done November 6, 2026:
    - `kythira/iam/iam_state_machine.hpp` (the state machine, its
      `iam_command` enum and the `iam_leader` helpers) and
      `kythira/iam/iam_seal.hpp` (KEK derivation and AES-256-GCM
      sealing); 31 cases in `services/iam/tests/iam_state_machine_test.cpp`.
    - Commands are JSON objects with a numeric `type`; values are part of
      the log format and only ever appended. The leader stamps `now`,
      unique ids and sealed secrets; `apply()` re-checks name clashes,
      DeleteConflict and quotas and answers `{"error": <AWS code>}`
      without mutating. A command the leader could never have proposed
      (bad JSON, unknown type, missing field) throws
      `std::invalid_argument`.
    - Entities sit in ordered maps keyed by lower-cased name, so names
      are unique case-insensitively and `get_state()` is byte-identical
      across replicas. The access-key index (key id to account) is a
      hash map rebuilt on restore; attachment and boundary counts are
      recomputed on restore and checked against the entities.
    - The KEK is PBKDF2-HMAC-SHA256 (200 000 iterations, the CA's) over a
      per-cluster salt that the first `create_account` carries. A sealed
      secret is base64url(nonce | tag | ciphertext) with the record's id
      (access key id, STS kid) as associated data, so a blob moved to
      another record does not open.
    - STS keys retire on the log's clock: every STS-key command carries
      the leader's `now`, and `apply()` drops retiring keys whose
      `retire_after` has passed.
    - Access-key last-used updates batch through
      `iam_leader::last_used_batcher`, at most one write per key per
      interval (five minutes by default).
    - `services/iam/tools/aws_managed_policies.json` holds the six
      policies Requirements 4.3 and 13 name (AdministratorAccess,
      ReadOnlyAccess v191, IAMFullAccess, IAMReadOnlyAccess and the two
      ElastiCache policies), fetched verbatim from AWS on November 6, 2026
      by `services/iam/tools/refresh_aws_managed_policies.sh` with Clark's
      personal profile (the CI role's boundary denies IAM, so refreshes
      run locally). All six parse with `iam_policy.hpp`. The header is
      generated at configure time into the build tree, and a digest of
      the set is recorded in each snapshot; a restore from a member with
      another set is accepted but flagged (`aws_managed_mismatch()`), so a
      rolling upgrade is not blocked. The files live
      under the module, not a top-level `tools/iam/`, because the module
      boundary check forbids a module's CMake from reaching outside it.

- [x] 9. `iam_node` daemon and `kythira-iam` CLI
  - `services/iam/cmd/iam_node`:
    - flags, unseal, Raft over TLS-TCP RPC with reserved-SAN checks;
    - leader forwarding and health;
    - a Beast HTTPS listener, routing by credential-scope service;
    - `/v1/admin/*` for admin-SAN certificates.
  - `kythira-iam`: `create-account` (root key printed once and written with
    `private_file`), `list-accounts`, `rotate-sts-key` and `replica-add`.
  - `services/iam/cmd/kythira-iam`. Composition-root `*_impl.hpp` includes
    are listed in `services/iam/boundary-allow.txt`.
  - CMake targets, and a three-node compose file in
    `services/iam/tests/docker/`. The compose file uses service names only
    and works with Docker and rootless Podman. Add its scenario to
    `iam-module.yml`'s container matrix.
  - _Requirements: 4.6, 8.1–8.5, 14.1, 17.1, 17.3_
  - Done November 6, 2026:
    - `include/raft/beast_https_server.hpp` (core): a small Boost.Beast
      HTTPS server with optional client certificates, connection, header
      and body limits, and per-phase timeouts; 9 cases in
      `tests/beast_https_server_test.cpp`. It is in core, not the module,
      because modules may not include each other and Kinesis, DynamoDB and
      S3 front ends will reuse it. It does not share
      `cache_http_front_end`'s code as the design suggested: that front
      end is cpp-httplib, so only its limits were copied.
    - `services/iam/cmd/iam_node/` (`config.hpp`, `main.cpp`): the flags,
      the unseal passphrase file, Raft over TLS-TCP RPC whose trust policy
      binds each peer's reserved SAN to its node id (plaintext RPC only
      with `--allow-plaintext-rpc` or on loopback), plaintext HTTP only
      with `--insecure-plaintext` (Requirement 14.1), and a refusal to
      start if the RPC certificate lacks its own reserved SAN. A member
      whose passphrase does not open the cluster's sealed secrets exits 1
      with a message rather than serve.
    - Routes: `/healthz` (local, never redirected: role, leader, term,
      applied index, account and replica counts, unsealed);
      `/v1/admin/accounts` (GET, POST), `/v1/admin/sts-keys/rotate` and
      `/v1/admin/replicas` for certificates whose URI SAN is on
      `--admin-uri-san`. Admin calls on a follower answer 307 to the
      leader's URL rather than being proxied, and the CLI follows them.
      IAM and STS requests are routed by credential-scope service and
      answer `NotImplemented` Query XML until tasks 14 and 17.
    - `services/iam/include/kythira/iam/iam_admin.hpp` (the admin
      operations over an `admin_cluster` concept) and `iam_routing.hpp`;
      10 cases in `services/iam/tests/iam_admin_test.cpp`.
    - Replicas: two new commands, `register_replica` (100) and
      `remove_replica` (101), keep the registered learners in the state
      machine and its snapshot, so a new leader re-adds them. The design's
      front-end self-registration (`POST /v1/iam/replicas` with a node SAN
      allowlist) is left to the front ends' task; `replica-add` is the
      admin path to the same command.
    - `kythira-iam`: `create-account` (refuses to overwrite an existing
      key file, writes it with `raft::write_private_file`, prints the
      secret once), `list-accounts [--json]`, `rotate-sts-key`,
      `replica-add`, and `health`, which the compose scenario uses as its
      replication probe.
    - Tests: `services/iam/tests/iam_node_test.cpp` runs the binaries as
      processes (three TLS voters and a replica on `free_port_block`:
      redirects, create and list, 403 for no certificate and for a
      non-admin one, rotation, replica-add, leader loss and catch-up, and
      the wrong passphrase). `services/iam/tests/docker/compose.yml`
      is the same cluster by service name, with a one-shot `certs`
      service (`iam_test_certs --if-absent`) writing the test PKI to a
      volume; `iam_node_scenario_test` drives it with `compose exec`
      through `container_runtime()`/`compose_prefix()`. It passed 10 of
      10 runs under Docker in the sandbox; the `container` job of
      `iam-module.yml` runs it under Docker and rootless Podman.
    - No core `*_impl.hpp` is included, so `boundary-allow.txt` stays
      empty.

- [x] 10. Policy parser and matchers
  - `services/iam/include/kythira/iam/iam_policy.hpp`: parse with duplicate-key detection, the
    AST, per-type validation and size limits, and `MalformedPolicyDocument`
    messages.
  - Action matching (case-insensitive) and ARN matching (field-wise,
    case-sensitive) with `*` and `?`.
  - Tests start the corpus format: JSON files under
    `services/iam/tests/corpus/`. If task 8 has not landed first, this task
    creates the module scaffolding (see Overview).
  - _Requirements: 5.1–5.3_
  - **Done November 6, 2026.** Notes:
    - This task created the module scaffolding: `services/iam/` with its
      `Kconfig` (`IAM`, default off, under a new top-level "Service
      Modules" menu), `CMakeLists.txt`, `README.md` and an empty
      `boundary-allow.txt`; the top-level `KYTHIRA_BUILD_IAM` gate and
      `add_subdirectory`; `CONFIG_IAM=y` in `ci_full_defconfig` and a new
      `ci_no_iam_defconfig`; the format-check, clang-tidy and
      `.clang-tidy` header-filter globs for `services/*` (the same lines as
      `redis-gateway-module` task 4); and `iam-module.yml`. The workflow's
      module-off leg is a configure plus an assertion that no IAM target
      or test exists, not a full core build: `ci.yml`'s config-variants job
      already builds all of `minimal_defconfig`, where IAM is off.
    - Duplicate keys are found by a Boost.JSON SAX pass before the DOM
      parse, since `boost::json::parse` keeps one of them silently.
    - Size quotas follow IAM's counting (whitespace outside strings is
      free). The per-document defaults are 6144 for identity and boundary
      documents, 2048 for trust and session documents and 20480 (S3's
      bucket-policy limit) for resource policies; the aggregate inline
      quotas are the state machine's job (task 8).
    - ARN matching is segment by segment, except that a `*` ending a
      segment may match past colons, as the IAM User Guide's Resource
      page says. `?` and `*` count UTF-8 code points.
    - Condition entries keep their shape only (operator, key, values as
      strings); the operator table and variables are task 11. A resource
      pattern holding `${...}` is stored and never matches until then.
    - Error messages use AWS's wording where AWS has one ("Policy document
      should not specify a principal.", "Has prohibited field Resource",
      "Statement IDs (SID) in a single policy must be unique.").

- [x] 11. Condition operators, variables and context keys
  - The operator table with every family, `IfExists`, the set prefixes, and
    the `Null` semantics.
  - Policy variables with escapes, and unresolved-variable behaviour.
  - The global-condition-key builder.
  - At least one corpus case per operator and per variable, including the
    tricky AWS-documented ones: `ForAllValues` with an empty set, negated
    operators with multiple values, and `IfExists` on a missing key.
  - Invariants (`invariants.md`): INV-COND-1..9 and INV-VAR-1..5, all
    tested.
  - Done: `iam_conditions.hpp`; 181 corpus cases in `corpus/condition/`
    and `corpus/parse/condition_operators.json` (328 in all).
  - _Requirements: 5.4–5.6_

- [x] 12. Evaluator and corpus
  - `evaluate()`, transcribed from AWS's evaluation-logic pages:
    - explicit deny, then the SCP seam;
    - same-account resource-policy rules, including the user-ARN and
      session-ARN exceptions;
    - boundaries and session policies;
    - cross-account;
    - the root-user rule;
    - deciding-statement reporting.
  - Grow the corpus to at least 300 cases from the AWS IAM User Guide
    examples. Every case cites its source page.
  - Invariants (`invariants.md`): INV-EVAL-1..12. Each one needs a named
    unit test, and the PR updates its row there.
  - Done: `iam_evaluator.hpp` (`evaluate(evaluation_request)`; gathering a
    principal's policies is the caller's, task 13's authorizer and the
    STS handlers); `iam_evaluator_test.cpp` with one case per invariant;
    170 `evaluate` cases in `corpus/evaluate/` (498 in all).
  - _Requirements: 6.1–6.7_

- [x] 13. Authorizer hook, service registry and replicas
  - `iam_authorizer`:
    - `authenticate` covers AKIA and ASIA keys, the latter through task
      17's token decoder, which is stubbed until then;
    - `authorize`;
    - the asynchronous audit-log writer.
  - `iam_service_registry`, with the `iam`, `sts` and `elasticache` tables
    under `services/iam/include/kythira/iam/services/`. The core
    `aws_request_authorizer.hpp` concept and types (`principal_context`,
    `decision`, `aws_action_table`) are what other modules call.
  - Replicas: `POST /v1/iam/replicas` on `iam_node`, plus the learner join
    and apply loop embeddable in any front end. A lag metric, and failing
    closed past the staleness bound.
  - `--arn-partition` threaded through every ARN.
  - Tests:
    - a policy change reaches a replica within 5 s;
    - a replica with a blocked network fails closed after the bound;
    - the audit lines hold no secrets.
  - Invariants (`invariants.md`): INV-KEY-4. Each one needs a named
    unit test, and the PR updates its row there.
  - _Requirements: 7.1–7.6, 14.2–14.4_
  - Done in two PRs.
    - Part 1: core `aws_request_authorizer.hpp`; `iam_service_registry.hpp`;
      `services/iam_actions.hpp` (99 actions) and `sts_actions.hpp` (9);
      `iam_authorizer.hpp` (`authenticate` for AKIA keys, with ASIA keys
      answered InvalidClientTokenId until task 17; `authorize`;
      `audit_log`); `iam_authorizer_test.cpp` (INV-KEY-4, no secrets in
      audit lines).
    - Part 2: core `node::last_caught_up()`, the staleness clock;
      `iam_replica.hpp` (`iam_replica<Types>`, `replica_freshness`, the
      `kythira_iam_replica_lag_seconds` gauge); `iam_authorizer::set_freshness`
      (fail closed); `services/elasticache_actions.hpp` (`Connect`);
      `iam_node --iam-max-staleness-secs`, with the lag in `/healthz` and
      `/metrics`; `iam_replica_test.cpp` (5 s propagation, a partition
      past the bound fails closed); `--arn-partition` was already in every
      ARN built, and a test now covers a non-`aws` partition.
    - The replica route is `POST /v1/admin/replicas` (task 9), not
      `/v1/iam/replicas`; a front end's self-registration arrives with the
      front ends.

### Phase C — IAM Query API

- [ ] 14. Query codec, users, groups and access keys
  - `include/raft/aws_query_protocol.hpp`:
    - form decoding with member lists;
    - the XML writer;
    - the error shape;
    - sealed pagination markers.
  - The user, group and access-key actions of Requirements 9.2–9.3, each
    authorized as `iam:<Action>`.
  - Tests with the AWS CLI v2 container: create a user, create a key, call
    with the key, deactivate the key (the next call fails after
    propagation), delete.
  - Invariants (`invariants.md`): INV-KEY-3 and INV-KEY-4, and calls the `iam_validation.hpp` checks for INV-NAME-4, INV-TAG-1..6 and INV-ACCT-2. Each one needs a named
    unit test, and the PR updates its row there.
  - _Requirements: 9.1–9.3, 9.8, 9.9_

- [ ] 15. Roles, policies, versions, attachments and boundaries
  - Record the Terraform AWS provider's IAM call list for users, groups,
    roles, policies, attachments, boundaries and OIDC providers:
    - preferred: one `apply` and `destroy` against a real AWS account, using
      IAM only, which is free; this needs Clark's go;
    - otherwise, from the pinned provider's source.

    Check the list in as `services/iam/tests/terraform_calls.txt`.
  - The role and policy actions of Requirements 9.4–9.5.
  - Instance-profile records and every instance-profile call on the
    recorded list.
  - `iam:PassedToService`, `iam:PolicyARN` and `iam:PermissionsBoundary`
    condition keys.
  - Invariants (`invariants.md`): INV-ROLE-1 and INV-ROLE-2 (CreateRole and UpdateRole call `iam_validation.hpp`), INV-POL-1 and INV-POL-6. Each one needs a named
    unit test, and the PR updates its row there.
  - _Requirements: 9.4, 9.5, 9.7, 9.8_

- [ ] 16. OIDC providers, simulation and account calls
  - The OIDC provider actions, SimulatePrincipalPolicy, SimulateCustomPolicy,
    GetContextKeysForPrincipalPolicy, GetAccountAuthorizationDetails and
    GetAccountSummary.
  - `NotImplemented` for every other action.
  - The simulation calls go through `evaluate()`. Tests compare them with
    corpus decisions.
  - Invariants (`invariants.md`): INV-OIDC-1..3 (CreateOpenIDConnectProvider calls `iam_validation.hpp`). Each one needs a named
    unit test, and the PR updates its row there.
  - _Requirements: 9.6, 9.7, 6.7_

### Phase D — STS and federation

- [ ] 17. STS core and session tokens
  - The STS Query handlers: AssumeRole (including cross-account and
    `ExternalId`), GetSessionToken, GetCallerIdentity and GetAccessKeyInfo.
  - The session-token format of design Component 9: CBOR payload,
    AES-256-GCM, `kid`.
  - The `record_sts_key` and `retire_sts_key` rotation.
  - Fill in the authorizer's ASIA path.
  - Tests:
    - cross-account AssumeRole allowed and denied by each side;
    - `ExternalId` mismatch;
    - a token across a key rotation;
    - an expired token;
    - GetCallerIdentity never denied.
  - Invariants (`invariants.md`): INV-STS-1, INV-STS-3..5, INV-STS-7, INV-DUR-1, INV-DUR-2, INV-DUR-5, INV-DUR-6 and INV-STS-B1..B4, INV-STS-B8. Each one needs a named
    unit test, and the PR updates its row there.
  - _Requirements: 10.1–10.4, 10.7_

- [ ] 18. Session policies, tags, source identity and revocation
  - `Policy` and `PolicyArns`, `Tags` and `TransitiveTagKeys` with
    `sts:TagSession`, and `SourceIdentity` with `sts:SetSourceIdentity`.
  - A 1-hour cap for role chaining, packed-size limits, and
    `aws:TokenIssueTime`.
  - A test of AWS's "revoke older sessions" inline-deny pattern.
  - Invariants (`invariants.md`): INV-STS-6, INV-DUR-3, INV-SPOL-1..4 and INV-STAG-1..5. Each one needs a named
    unit test, and the PR updates its row there.
  - _Requirements: 10.2, 10.5, 10.6_

- [ ] 26. GetFederationToken, DecodeAuthorizationMessage and credential limits
  - Numbered 26 because it was added on November 6, 2026, at Clark's
    request ("Add STS support to the IAM spec"); it follows task 18.
  - GetFederationToken (design Component 9): the `federated` token, the
    federated-user principal and `aws:userid`, the 129600-second cap (3600
    for the root), and the user-policy ∩ session-policy evaluation.
  - DecodeAuthorizationMessage: `decision::encoded_message` behind the
    authorizer's `want_encoded_message` flag, the `0x02` sealed record, and
    AWS's JSON rendering.
  - The temporary-credential limits of Requirement 10.10 in the Query
    router, and `NotImplemented` for AssumeRoleWithSAML and AssumeRoot.
  - Add the new actions to the `sts` action table.
  - Tests:
    - GetFederationToken with no session policy can do nothing; with one,
      exactly the intersection;
    - a resource policy naming the federated-user ARN, and one naming the
      user ARN;
    - a user-policy change applying to a live federated session;
    - a root call capped at 3600 seconds;
    - federated and GetSessionToken credentials refused on IAM and on the
      STS calls they may not make, whatever the policies say;
    - an encoded message that round-trips, one decoded from another
      account, a session token fed to DecodeAuthorizationMessage, and an
      expired message.
  - Invariants (`invariants.md`): INV-STS-2, INV-DUR-5, INV-DUR-6, INV-CRED-1..6, INV-EVAL-7 and INV-STS-B5. Each one needs a named
    unit test, and the PR updates its row there.
  - _Requirements: 10.1, 10.8–10.10_

- [ ] 19. AssumeRoleWithWebIdentity (external OIDC)
  - `oidc_jwks_cache` with timeouts, a size cap, the host allowlist, private
    address refusal and DNS-rebinding-safe connects.
  - JWT validation and the federated principal with its `:sub`, `:aud` and
    `:amr` keys.
  - Tests:
    - a local test issuer, a Python container serving discovery and JWKS;
    - a wrong `aud`;
    - an unknown `kid` that triggers a single refetch;
    - an expired token;
    - an SSRF attempt at `169.254.169.254` refused.
  - Invariants (`invariants.md`): INV-STS-1, INV-WEB-1, INV-DUR-4, INV-STS-B6 and INV-STS-B7. Each one needs a named
    unit test, and the PR updates its row there.
  - _Requirements: 11.1–11.4_

- [ ] 20. Kythira-CA web identity
  - Register the Kythira issuer as a provider (`--kythira-issuer-local`
    shortcut), and expose the `kythira:*` claims as condition keys.
  - The optional direct exchange `POST /v1/iam/credentials` with
    certificate-SAN condition keys.
  - An end-to-end test: a node certificate gets a JWT from the CA cluster,
    exchanges it at `iam_node`, and calls IAM with the session.
  - _Requirements: 12.1–12.3_

### Phase E — Redis

- [ ] 21. Redis AUTH with IAM tokens
  - Needs `redis-gateway-module` tasks 4 and 6 merged (both done). Paths:
    the gateway and ACL are under `services/redis/include/kythira/redis/`,
    the seam is `include/raft/credential_verifier.hpp`, and the hooks are
    `redis_gateway_config::_credential_verifier` /
    `_credential_verifier_users` and `cache_http_config::_credential_verifier`
    / `_credential_verifier_selects`, described in `services/redis/README.md`
    "Authentication seam".
  - In the Redis module:
    - parse the ACL `auth=password|iam|both` as a reserved attribute, like
      `cert=`;
    - reject a repeated or unknown value, and default to `password`;
    - update the test that `redis-gateway-module` task 1 pinned;
    - add `CONFIG_REDIS_GATEWAY_IAM` (depends on `REDIS_GATEWAY && IAM`)
      in `services/redis/Kconfig`.
  - In the IAM module, `iam_redis_verifier` satisfies the core
    `credential_verifier` concept:
    1. parse the presigned token;
    2. check the user and host;
    3. authorize `elasticache:Connect` on the user and replication-group
       ARNs, against the local replica;
    4. return the principal and expiry.

    The `elasticache` action table ships with it.
  - `redis_gateway_node`'s composition root wires the verifier and the
    replica from task 13 when `CONFIG_REDIS_GATEWAY_IAM` is set. That
    include is a `module-adapter` line in
    `services/redis/boundary-allow.txt`. The gateway then applies the ACL
    role and key prefixes.
  - Tests:
    - a boto3-generated token, built the way the ElastiCache docs show;
    - a wrong user, an expired token, and a denied policy;
    - a password user unchanged;
    - a build with `CONFIG_REDIS_GATEWAY_IAM` off has no IAM flags or code.
  - _Requirements: 13.1–13.4, 13.6, 13.7, 17.5_

- [ ] 22. Connection lifetime, limiter and rollout
  - The 12-hour deadline on IAM-authenticated connections, through the
    per-connection `expires` from `redis-gateway-module` task 6, extended by
    re-AUTH.
  - IAM denials counted by `redis_auth_limiter`.
  - Make sure forwarded internal links are unaffected.
  - Document the rolling-upgrade rule in `doc/redis-gateway.md`: every
    gateway runs the new build before any ACL file uses `auth=`, or
    IAM-only users get secret `disabled`.
  - Extend `redis-gateway-module` task 7's rolling-upgrade scenario with an
    `auth=iam` user.
  - Tests with an injected clock.
  - _Requirements: 13.1, 13.5, 13.7, 3.8_

### Phase F — Conformance and close-out

- [ ] 23. AWS CLI, SDK and Redis conformance job
  - A compose scenario: three `iam_node`s, the CA cluster, and two Redis
    gateways as replicas.
  - Drive it with the pinned AWS CLI v2 and boto3, covering everything in
    Requirement 15.1, including the STS calls of tasks 17–19 and 26 and the
    SDK credential providers of Requirement 10.11 (a `source_profile` role
    chain, `web_identity_token_file`, and the `AWS_ROLE_ARN` environment
    pair).
  - Add it to `iam-module.yml`'s container matrix, under both Docker and
    rootless Podman, using the shared `setup-container-runtime` action. The
    workflow's path filter covers `services/iam/**` and the core headers of
    Requirement 17.2.
  - _Requirements: 10.11, 15.1, 15.3, 15.4_

- [ ] 24. Terraform conformance and simulator comparison
  - `terraform apply` and `destroy` of a configuration covering
    Requirement 15.2 against the compose cluster, with the provider pinned.
    Fail on any `NotImplemented`.
  - Optional, only on Clark's go: run the corpus through AWS
    `SimulateCustomPolicy` in a real account, which is free. Add every
    disagreement as a corpus case, with AWS's answer as the expected
    decision.
  - _Requirements: 15.2, 15.5_

- [ ] 25. Documentation and close-out
  - `doc/iam.md`, `doc/oidc-issuer.md`, and the `doc/redis-gateway.md`
    additions of Requirement 16.
  - A security pass over the new listeners against the 2026-10-02 audit
    categories (Requirement 14.5).
  - Update `doc/TODO.md` and `doc/CHANGELOG.md`.
  - Record the corpus size, the Terraform call coverage, and any AWS
    differences found.
  - _Requirements: 14.5, 16.1–16.3_
