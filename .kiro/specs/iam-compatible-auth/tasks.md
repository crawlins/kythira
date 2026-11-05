# Implementation Plan — IAM-Compatible Authentication and Authorization

## Status: 2/25 tasks complete

**Last Updated**: November 2, 2026. The spec was written from the scoping
doc "IAM for Kythira — scoping" and Clark's decisions of the same day. It
was amended the same day to put IAM in a `services/iam/` module, following
the `redis-gateway-module` spec (PR #675), at Clark's request.

## Overview

First, make Kythira an OIDC issuer so its nodes get cloud credentials
without stored keys (tasks 1–5). Then build an AWS IAM-compatible service:
- SigV4 verification (tasks 6–7);
- the IAM state machine and daemon (tasks 8–9);
- the policy engine (tasks 10–12);
- the authorizer hook (task 13);
- the IAM Query API (tasks 14–16);
- STS and federation (tasks 17–20).

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
    { "wave": 7, "tasks": [16, 19, 22], "description": "OIDC provider and simulation APIs; web identity; Redis lifetime and modes" },
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

- [ ] 4. Azure, Alibaba and OCI exchange providers
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

- [ ] 5. Real-cloud federation run (needs Clark's go)
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

- [ ] 7. Streaming payloads and failure rate limiting
  - `sigv4_chunk_decoder`: `STREAMING-AWS4-HMAC-SHA256-PAYLOAD`, the
    unsigned and signed trailer forms, and per-chunk verification before
    release.
  - Vectors are generated by a pinned botocore script, which is checked in
    with its outputs.
  - Failure limiter keyed by source address and access key id, modelled on
    `redis_auth_limiter`.
  - _Requirements: 3.4, 3.8_

- [ ] 8. `iam_state_machine`
  - The command enum and JSON bodies of design Component 4. Leader-side
    stamping helpers.
  - `apply()` re-checks the preconditions that can race.
  - Per-account maps, the global access-key index, quotas, unique-id
    generation, case-insensitive names, and the sealed-secret format.
  - A generated header for the AWS-managed policy subset, from
    `tools/iam/aws_managed_policies.json`.
  - Snapshot round trip with a version.
  - Lives in `services/iam/include/kythira/iam/`, with tests in
    `services/iam/tests/`. If task 10 has not landed first, this task
    creates the module scaffolding (see Overview).
  - Property tests:
    - random command sequences give identical `get_state()` on two
      instances and after a restore;
    - no plaintext secret appears in the log or snapshot bytes.
  - _Requirements: 4.1–4.5, 4.7, 4.8_

- [ ] 9. `iam_node` daemon and `kythira-iam` CLI
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

- [ ] 10. Policy parser and matchers
  - `services/iam/include/kythira/iam/iam_policy.hpp`: parse with duplicate-key detection, the
    AST, per-type validation and size limits, and `MalformedPolicyDocument`
    messages.
  - Action matching (case-insensitive) and ARN matching (field-wise,
    case-sensitive) with `*` and `?`.
  - Tests start the corpus format: JSON files under
    `services/iam/tests/corpus/`. If task 8 has not landed first, this task
    creates the module scaffolding (see Overview).
  - _Requirements: 5.1–5.3_

- [ ] 11. Condition operators, variables and context keys
  - The operator table with every family, `IfExists`, the set prefixes, and
    the `Null` semantics.
  - Policy variables with escapes, and unresolved-variable behaviour.
  - The global-condition-key builder.
  - At least one corpus case per operator and per variable, including the
    tricky AWS-documented ones: `ForAllValues` with an empty set, negated
    operators with multiple values, and `IfExists` on a missing key.
  - _Requirements: 5.4–5.6_

- [ ] 12. Evaluator and corpus
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
  - _Requirements: 6.1–6.7_

- [ ] 13. Authorizer hook, service registry and replicas
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
  - _Requirements: 7.1–7.6, 14.2–14.4_

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
  - _Requirements: 9.4, 9.5, 9.7, 9.8_

- [ ] 16. OIDC providers, simulation and account calls
  - The OIDC provider actions, SimulatePrincipalPolicy, SimulateCustomPolicy,
    GetContextKeysForPrincipalPolicy, GetAccountAuthorizationDetails and
    GetAccountSummary.
  - `NotImplemented` for every other action.
  - The simulation calls go through `evaluate()`. Tests compare them with
    corpus decisions.
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
  - _Requirements: 10.1–10.4, 10.7_

- [ ] 18. Session policies, tags, source identity and revocation
  - `Policy` and `PolicyArns`, `Tags` and `TransitiveTagKeys` with
    `sts:TagSession`, and `SourceIdentity` with `sts:SetSourceIdentity`.
  - A 1-hour cap for role chaining, packed-size limits, and
    `aws:TokenIssueTime`.
  - A test of AWS's "revoke older sessions" inline-deny pattern.
  - _Requirements: 10.2, 10.5, 10.6_

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
  - Needs `redis-gateway-module` tasks 4 and 6 merged.
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
    Requirement 15.1.
  - Add it to `iam-module.yml`'s container matrix, under both Docker and
    rootless Podman, using the shared `setup-container-runtime` action. The
    workflow's path filter covers `services/iam/**` and the core headers of
    Requirement 17.2.
  - _Requirements: 15.1, 15.3, 15.4_

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
