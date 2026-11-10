# Design Document — IAM-Compatible Authentication and Authorization

## Overview

Two layers, built in this order:

1. **Outbound: Kythira as an OIDC issuer** (Components 1–2). The CA cluster
   signs short-lived JWTs for holders of Kythira-CA certificates. Per-cloud
   providers exchange those JWTs for native cloud credentials.
2. **Inbound: an IAM-compatible service** (Components 3–11). A new
   `iam_node` daemon hosts the IAM Raft group and serves the IAM Query API and
   STS. Front ends embed a learner replica of that group plus an
   `iam_authorizer`, and authenticate and authorize every request locally.
   The Redis gateway is the first front end. Kinesis, DynamoDB and later S3
   plug into the same hook.

```
  AWS CLI / SDK / Terraform        redis-cli / Redis clients      Kythira nodes
          │ SigV4 (Query)                 │ AUTH user <IAM token>      │ mTLS (CA cert)
          ▼                               ▼                            ▼
 ┌──────────────────────────┐   ┌──────────────────────────┐   ┌───────────────────────┐
 │ iam_node (voters)        │   │ redis_gateway_node       │   │ ca_cluster_node       │
 │  Query codec: IAM, STS   │   │  RESP AUTH               │   │  /.well-known/*       │
 │  iam_authorizer          │   │  iam_authorizer          │   │  POST /v1/oidc/token  │
 │  iam_state_machine ◄─────┼───┼─ learner replica         │   │  oidc keys in         │
 │  STS sealing keys        │   │  (same state machine)    │   │  ca_state_machine     │
 └──────────────────────────┘   └──────────────────────────┘   └──────────┬────────────┘
          ▲  AssumeRoleWithWebIdentity (Kythira JWT or external JWT)      │ JWT
          └───────────────────────────────────────────────────────────────┤
                                                                          ▼
                          AWS STS · GCP STS · Entra ID · Alibaba STS · OCI IAM domain
```

Everything that touches a decision goes through one evaluator (Component 6).
Everything that touches a secret is sealed outside `apply()` by the leader,
the same way `ca_state_machine` handles the CA key.

## Module layout (Requirement 17)

IAM is a `services/iam/` module, following the pattern of the
`redis-gateway-module` spec (PR #675) and its `services/README.md`.

| Piece | Location | Why there |
|---|---|---|
| `jwt.hpp` | core `include/raft/` | The CA cluster's issuer (core) signs with it. IAM verifies with it. |
| `sigv4_verifier.hpp` and its test suite | core `include/raft/`, `tests/sigv4_suite/` | Shared by IAM and the future Kinesis, DynamoDB and S3 modules. Modules may not include each other. |
| `aws_query_protocol.hpp` | core `include/raft/` | IAM and STS use it now; S3-adjacent and SQS-style APIs could later. |
| `cloud_credential_source.hpp`, `kythira_oidc_token_source.hpp`, the per-cloud exchange providers | core `include/raft/` | Core's object-store and CA clients consume them. |
| `aws_request_authorizer.hpp` | core `include/raft/` | A concept and plain types: `principal_context`, `decision`, `aws_action_table`. Other modules call IAM only through these. |
| `credential_verifier.hpp` | core `include/raft/` | Added by `redis-gateway-module` task 6. IAM implements an adapter to it. |
| State machine, policy engine, evaluator, authorizer, replica client, STS, session tokens, JWKS cache, Query handlers, action tables | `services/iam/include/kythira/iam/` | IAM only. |
| `iam_node`, `kythira-iam` | `services/iam/cmd/` | Composition roots. Their `*_impl.hpp` includes are listed in `services/iam/boundary-allow.txt`. |
| Unit tests, policy corpus, Terraform call list | `services/iam/tests/` | |
| Compose scenarios and images | `services/iam/tests/docker/`, `services/iam/docker/` | |
| Kconfig `IAM` | `services/iam/Kconfig`, `source`d from the top-level `Kconfig` | |
| CMake | `services/iam/CMakeLists.txt`, added with `if(KYTHIRA_BUILD_IAM) add_subdirectory(services/iam)` after core and `tests/` | Links `kythira_core_headers`, and `kythira_test_support` for tests. It references nothing outside its own directory. |
| CI | `.github/workflows/iam-module.yml` | Path-filtered like `redis-module.yml`, so it is not a required check. |

**Dependencies.**
- The module scaffolding needs `redis-gateway-module` task 2 (the core
  INTERFACE targets and `cmake/kythira_test_helpers.cmake`) and task 3 (the
  boundary check and `services/README.md`).
- Tasks 1, 6 and 7 are core-only and do not wait on them.
- The Redis integration needs `redis-gateway-module` task 4 (the move) and
  task 6 (the seam).

## Component 1: OIDC issuer (Requirement 1)

**Where.** In `ca_cluster_node`. It already has the CA trust root, mTLS
client authentication, revocation, an unseal key, leader forwarding and an
HTTP API. A separate issuer daemon would duplicate all of that.

**State.** `ca_state_machine` gains two commands:

| Command | Body | Effect in `apply()` |
|---|---|---|
| `record_oidc_key` | `kid`, `alg`, public JWK, sealed private key, `publish_after`, `sign_after` | Adds a key in state `pending` |
| `retire_oidc_key` | `kid`, `retire_after` | Marks a key `retiring`, then removes it once `retire_after` has passed and a later command is applied |

`apply()` compares only the leader-stamped times carried in commands, never a
clock. The leader's rotation timer proposes `record_oidc_key` at
`rotation_period - 24 h`, and switches signing once its local clock passes
`sign_after`. The snapshot format gains an optional `oidc_keys` array. Old
snapshots without it restore with no keys, and the leader creates the first
key after election.

**Signing.** `include/raft/jwt.hpp` holds base64url, a compact JWS
(`header.payload.signature`) with OpenSSL `EVP_DigestSign`, RS256 with
PKCS#1 v1.5, and ES256 with the DER signature converted to raw R||S.
Claims are serialised with Boost.JSON. The verifier side (Component 10)
lives in the same header.

**Token endpoint.** `POST /v1/oidc/token` with a form body `audience=<aud>`
and an optional `lifetime=<seconds>`:

1. Require the client certificate, check its chain against the CA, and check
   the revocation list (the `/renew` path already does this).
2. Derive the subject:
   - a reserved node SAN gives `node:<canonical id>`;
   - a URI SAN under `--oidc-workload-uri-prefix` gives `workload:<uri>`;
   - anything else is refused with 403.
3. Check `aud` against `--oidc-audience` (repeatable).
4. Mint and return
   `{"token":"…","expires_at":"…","token_type":"urn:ietf:params:oauth:token-type:jwt"}`.

**Publishing.** `--oidc-publish-url=<s3|gs|az|oci|oss URL>` makes the leader
upload `/.well-known/openid-configuration` and `/.well-known/jwks.json`
through the existing object-store clients after each key change. The
discovery document's `jwks_uri` then points at the bucket. AWS and GCP need
the discovery document on a publicly trusted HTTPS origin. A bucket URL
satisfies that without exposing the CA cluster.

**Audience values** (documented in `doc/oidc-issuer.md`):

| Cloud | Audience |
|---|---|
| AWS | `sts.amazonaws.com`, or the client id registered on the IAM OIDC provider |
| GCP | `//iam.googleapis.com/projects/<n>/locations/global/workloadIdentityPools/<pool>/providers/<provider>` |
| Azure | `api://AzureADTokenExchange` |
| Alibaba | the client id registered on the RAM OIDC provider |
| OCI | the client id configured on the identity domain's token-exchange trust |

## Component 2: Cloud credential exchange (Requirement 2)

```cpp
template <typename T>
concept cloud_credential_source = requires(T& s) {
    { s.current() } -> kythira::future<cloud_credentials>;  // the project's future concept, so no-folly builds work
};
```

`cloud_credentials` is a variant of AWS keys plus session token, a GCP access
token, an Azure bearer token, Alibaba STS keys and an OCI UPST plus session
key. Each provider wraps a `kythira_oidc_token_source`, which is an mTLS
client of Component 1, does its exchange, and caches the result until 80% of
its lifetime has passed. A single in-flight refresh is shared by all callers.
On failure the last good credentials are served until they expire, and a
`kythira_cloud_credential_refresh_failures_total{cloud}` counter rises.

| Cloud | Exchange call | Result |
|---|---|---|
| AWS | `sts:AssumeRoleWithWebIdentity` (unsigned Query call) | `ASIA…` key, secret and session token |
| GCP | `POST https://sts.googleapis.com/v1/token` (token exchange), then optionally `iamcredentials…:generateAccessToken` | OAuth access token |
| Azure | `POST https://login.microsoftonline.com/<tenant>/oauth2/v2.0/token` with `client_assertion_type=jwt-bearer` | Bearer token per resource scope |
| Alibaba | `AssumeRoleWithOIDC` | STS key, secret and token |
| OCI | Identity-domain `/oauth2/v1/token` with `grant_type=urn:ietf:params:oauth:grant-type:token-exchange`, `requested_token_type=…upst` and a session public key | UPST, signed with the session key in `oci_signing.hpp` |

Existing clients take a new `credential_source` option. The AWS SDK clients
get an `AWSCredentialsProvider` adapter. GCP, Azure, OSS and OCI use the
token or keys directly in their hand-written request paths.

## Component 3: SigV4 verifier (Requirement 3)

`include/raft/sigv4_verifier.hpp`, independent of any HTTP library:

```cpp
struct sigv4_request {
    std::string_view method;
    std::string_view raw_path;           // as received, before any decoding
    std::string_view raw_query;
    header_list headers;                 // name lower-cased, values untrimmed
    std::optional<std::string_view> body; // whole body, when not streaming
};

struct sigv4_service_rules {
    std::string service;                 // "iam", "sts", "elasticache", "s3", …
    bool s3_path_rules = false;          // no double-encode, no normalisation
    std::bitset<5> payload_modes;        // which x-amz-content-sha256 forms are accepted
};

auto sigv4_parse(const sigv4_request&, const sigv4_service_rules&) -> result<sigv4_claim, aws_error>;
auto sigv4_verify(const sigv4_claim&, std::span<const std::byte> signing_key) -> bool;
class sigv4_chunk_decoder;  // streaming payloads: verifies each chunk, then signed trailers
```

Parsing is split from verification so the key lookup (Component 6) runs in
between. The chunk decoder carries the previous signature across chunks, as
the spec requires, and releases a chunk's bytes only after it has verified.

**Test vectors.**
- AWS's `aws-sig-v4-test-suite` (Apache-2.0) is copied under
  core `tests/sigv4_suite/` with its licence.
- botocore's S3 presign and chunked-upload vectors are generated by a pinned
  script whose output is checked in.

## Component 4: `iam_state_machine` (Requirement 4)

Modelled on `ca_state_machine`: a typed command enum with a Boost.JSON body.

| Group | Commands |
|---|---|
| Accounts | `create_account`, `set_account_alias` |
| Users | `create_user`, `update_user`, `delete_user`, `tag_entity`, `untag_entity` |
| User groups | `create_group`, `update_group`, `delete_group`, `add_user_to_group`, `remove_user_from_group` |
| Roles | `create_role`, `update_role`, `update_trust_policy`, `delete_role` |
| Instance profiles | `create_instance_profile`, `delete_instance_profile`, `add_role_to_instance_profile`, `remove_role_from_instance_profile` |
| Policies | `create_policy`, `create_policy_version`, `delete_policy_version`, `set_default_version`, `delete_policy`, `attach_policy`, `detach_policy`, `put_inline_policy`, `delete_inline_policy` |
| Permission boundaries | `put_boundary`, `delete_boundary` |
| Access keys | `create_access_key` (carries the sealed secret), `update_access_key`, `delete_access_key`, `record_key_last_used` (batched) |
| OIDC providers | `create_oidc_provider`, `update_oidc_provider`, `delete_oidc_provider` |
| Signing keys | `record_sts_key`, `retire_sts_key` |

**Rules shared by every command.**
- The Query handler validates the request on the leader: names, quotas,
  policy grammar (Component 5), and existence of referenced entities.
- The leader stamps `create_date`, the generated unique id and any sealed
  material into the command.
- `apply()` re-checks the preconditions that can race: name uniqueness,
  `DeleteConflict` and quota counts. It returns an error result rather than
  mutating, so two concurrent creates converge on the same outcome.
- Each command returns its result bytes for the handler to turn into XML.

**In-memory layout.** One `account_state` per account, with
`boost::unordered_flat_map`s keyed by lower-cased name and by unique id.
There is a global index from access key id to (account, owner, sealed
secret, status), the hot path for authentication. Parsed and compiled policies are cached
beside their JSON, keyed by (policy ARN, version). They are rebuilt on
restore, never serialised.

**Sealing.**
- The key-encryption key is PBKDF2-HMAC-SHA256 of the unseal passphrase, with
  a per-cluster salt stored in the `create_account` of the first account.
  The parameters are the CA's.
- An access-key secret is sealed with AES-256-GCM, a 96-bit random nonce, and
  the access key id as associated data.
- STS sealing keys are generated by the leader and sealed the same way.
- A replica needs the unseal passphrase to decrypt secrets, so a front end
  is given it through the same `--unseal-key-file` mechanism.
- A replica without the passphrase can authorize but cannot authenticate;
  it fails closed.

**AWS-managed policies.**
- `services/iam/tools/aws_managed_policies.json` is a pinned subset,
  generated once from the public policy reference and refreshed by hand.
- It is compiled into the binary as a generated header, and is never stored
  in the log.
- Its version is part of the snapshot header, so every member evaluates the
  same text.

## Component 5: Policy language (Requirement 5)

`services/iam/include/kythira/iam/iam_policy.hpp`.

- **Parse.**
  - Boost.JSON, with duplicate-key detection, because AWS rejects duplicate
    keys.
  - The result is an AST of `statement{effect, principal_set, action_matcher,
    resource_matcher, conditions}` with `not_` flags.
- **Validate per policy type** (identity, trust, resource, boundary,
  session). Each type has its own allowed elements and the size limits of
  Requirement 4.4.
- **Matchers.**
  - Wildcards compile into a small glob program, `*` and `?` only, with no
    regex.
  - Actions are lower-cased once at parse time.
  - ARNs are matched field by field when the pattern has the ARN shape, and
    as a whole string otherwise, as AWS does.
- **Conditions.**
  - A table maps operator name to `(type coercion, comparison)`.
  - The `IfExists` suffix and the set prefixes wrap any entry.
  - A missing key gives false, except for `IfExists`, `Null` and
    `ForAllValues` on an empty set, which follow AWS.
  - Multiple keys in one block are ANDed, multiple values of one key are
    ORed, and a negated operator applies the negation per value, as
    documented.
- **Variables** are substituted at evaluation time from the request
  context. An unresolvable variable makes that element not match.

## Component 6: Evaluator and authorizer (Requirements 6–7)

```cpp
struct request_context {
    principal_context principal;    // ARN, account, type, tags, session data
    std::string action;             // "iam:CreateUser"
    std::string resource_arn;       // possibly "*"
    std::string resource_account;
    context_keys keys;              // global + service keys, multi-valued
};

struct decision {
    enum class effect { allow, explicit_deny, implicit_deny } effect;
    std::optional<statement_ref> deciding;   // policy ARN + Sid/index
    std::optional<std::string> encoded_message;  // Component 9, on request
};

auto evaluate(const iam_view&, const request_context&) -> decision;
```

As built (task 12, `iam_evaluator.hpp`), `evaluate` takes one
`evaluation_request` holding the principal, the request and the policies
already gathered, so it has no view of the state machine and the same call
serves the authorizer, STS and the Simulate* actions. Gathering (step 1
below) belongs to the caller, which holds the `iam_view`.

`evaluate` is a direct transcription of AWS's "Policy evaluation logic"
pages.

1. Gather the applicable policies:
   - identity: user inline, user attached, group inline and group attached;
     or the role's inline and attached policies;
   - the resource policy, from the service through a callback;
   - the permissions boundary and the session policies.
2. Return `explicit_deny` if any of them denies.
3. The SCP seam (always passes).
4. Same account: apply the resource-policy allow rules, including the
   user-ARN and session-ARN exceptions. Otherwise identity ∧ boundary ∧
   session.
5. Cross account: (identity ∧ boundary ∧ session in the principal's account)
   ∧ (the resource policy in the resource's account).

The flow is one function under 200 lines, with a comment per step citing
the AWS page section.

**The authorizer.**
- `iam_authorizer` owns an `iam_view`: a read-locked view of the replica's
  state machine.
- `authenticate`:
  1. Parse SigV4.
  2. Look up the key: an `AKIA` key in the index; an `ASIA` key by unsealing
     the session token (Component 9).
  3. Derive the signing key, through the cache.
  4. Verify the signature.
- It then builds the principal context.
- `authorize` fills the request context from the service registry and calls
  `evaluate`.
- Audit lines go to a lock-free queue drained by one writer thread, so a slow
  sink never blocks requests. When the queue is full, the number of dropped
  lines is counted and logged.

**Service registry.**
- `iam_service_registry::add(service_prefix, action, resource_templates,
  condition_keys)`.
- The registry's input type, `aws_action_table`, is a core type in
  `aws_request_authorizer.hpp`. Each service module owns its table and hands
  it to IAM through its composition root.
- The `iam` and `sts` tables live in
  `services/iam/include/kythira/iam/services/`. The `elasticache` table
  ships with IAM's Redis adapter (Component 11), because the Redis module
  stays IAM-free.
- They are generated once from the AWS Service Authorization Reference and
  then maintained by hand. This spec ships `iam`, `sts` and `elasticache`.

**Replica.**
- Each front end runs a `raft` instance for the IAM Raft group as a learner,
  added with `add_learner()` by `iam_node`'s leader in response to
  `POST /v1/iam/replicas`. That call is authenticated by the front end's CA
  certificate, with a node SAN on a configured allowlist.
- The learner applies the same `iam_state_machine`.
- Lag is `leader_commit - replica_applied`, in entries and in seconds since
  the last heartbeat. It is exported as `kythira_iam_replica_lag_seconds`.
- Past `--iam-max-staleness` (default 60 s), `authenticate` fails closed with
  `ServiceUnavailable`.
- As built (task 13):
  - Lag is the time since the replica was last caught up: core's
    `node::last_caught_up()` moves when an accepted AppendEntries leaves
    the node holding and having applied the leader's commit index, so a
    healthy replica's lag stays under a heartbeat interval and a cut-off
    one grows without bound. An entry count is not exported.
  - `iam_replica<Types>` takes the front end's own `node_config` and
    needs no timers (a learner never stands for election).
  - `iam_authorizer::set_freshness` makes both `authenticate`
    (`ServiceUnavailable`, 503) and `authorize` (deny, audited) fail
    closed. The flag is `--iam-max-staleness-secs`.
  - `iam_node --replica` reports the lag in `/healthz`, serves the gauge
    on `/metrics`, and answers IAM and STS requests 503 when stale.

## Component 7: `iam_node` and `kythira-iam` (Requirement 8)

`services/iam/cmd/iam_node/main.cpp` follows `ca_cluster_node` for flags, unseal,
Raft-over-TLS-TCP RPC with reserved-SAN peer checks, leader forwarding and
health. The public listener uses Boost.Beast, sharing the connection limits
and TLS setup of `cache_http_front_end`. That is the front end Kinesis,
DynamoDB and S3 will reuse.

**Routing.** A request goes to the IAM, STS or replica handlers by the
service in its credential scope, or by path for `/v1/*` admin routes. Writes
on a follower are forwarded to the leader. Reads are served from the local
state.

**`kythira-iam` subcommands.** `create-account`, `list-accounts`,
`rotate-sts-key` and `replica-add`. Each talks mTLS to `/v1/admin/*` with a
certificate whose URI SAN is on `--admin-uri-san`.

**Kconfig.** `IAM` in `services/iam/Kconfig` (default n). It is enabled in the CI
configurations that run Requirement 15.

**As built (task 9).**
- The public listener is core's `include/raft/beast_https_server.hpp`, with
  the limits of `cache_http_front_end` (which is cpp-httplib, so no code is
  shared).
- Admin writes on a follower are redirected (307 to the leader's URL from
  `--peers`), not proxied; `kythira-iam` follows the redirect.
- Admin routes: `GET`/`POST /v1/admin/accounts`,
  `POST /v1/admin/sts-keys/rotate`, `POST /v1/admin/replicas`. `/healthz`
  is local and open.
- Registered replicas are state-machine records (`register_replica`,
  `remove_replica`, in the snapshot), so any leader re-adds them as
  learners. The front ends' self-registration route above arrives with the
  front ends.

## Component 8: Query protocol codec (Requirement 9)

`include/raft/aws_query_protocol.hpp`:
- Form decoding with AWS's member-list flattening: `Tags.member.1.Key`,
  `PolicyArns.member.N.arn`.
- A small streaming XML writer with escaping.
- A per-action shape table that names the request fields, their types and
  required flags, and the response element names.

Each action is one handler function:
`(account_view, principal, parsed request) -> result<xml, aws_error>`.

**Errors** are written as
`<ErrorResponse><Error><Type>Sender|Receiver</Type><Code/><Message/></Error><RequestId/></ErrorResponse>`,
with HTTP statuses from AWS's error tables.

**Pagination.** `Marker` is an opaque base64url of the last key, sealed with
an HMAC so clients cannot forge positions. `MaxItems` defaults to 100.

**As built (task 14).**
- Handlers are `iam_query_api<C>` member functions rather than a shape
  table; each validates its own parameters with `iam_validation.hpp` and
  the leader-side name and path rules.
- A marker's HMAC key is derived from the KEK, and its message names the
  action, account and list scope, so a marker opens on any member and only
  for the list that issued it.
- `iam_node` serves IAM on the leader, reading its applied state;
  a follower answers 307 like the admin routes. Proxying writes from a
  follower is left for the front ends' shared listener.
- A missing entity is authorized against its would-be ARN before
  `NoSuchEntity`, so a caller without access cannot probe names.

**As built (task 15).**
- The role, managed-policy, version, attachment, inline-policy,
  boundary and instance-profile actions share small loaders (one per
  entity kind) and generic attach, inline, boundary and tag handlers
  keyed by `user`, `group`, `role`, `policy` or `instance-profile`.
- Policy documents are checked in two steps: INV-POL-1's character and
  length rule (`ValidationError`), then the parser for the document's type
  within the configured quota, whose own code (`MalformedPolicyDocument`
  or `LimitExceeded`) is returned. Documents come back URL-encoded, as
  AWS returns them.
- Bundled AWS-managed policies are readable and listable (`Scope=AWS`)
  with this account's attachment counts. Their `PolicyId` is derived from
  the name's SHA-256, so every member gives the same one. They cannot be
  tagged, versioned or deleted (`InvalidInput`).
- `AddRoleToInstanceProfile` also authorizes `iam:PassRole` on the role,
  with `iam:PassedToService` `ec2.amazonaws.com` and the profile as
  `iam:AssociatedResourceArn`, as AWS does. Attach and detach carry
  `iam:PolicyARN`. Boundary actions and `CreateRole`/`CreateUser` carry
  `iam:PermissionsBoundary`. A delete is keyed on the boundary being
  removed.

**As built (task 16).**
- OIDC provider URLs are stored and named without `https://`, as AWS
  names them in ARNs and returns them from GetOpenIDConnectProvider.
  Tags on providers use the same generic tag handlers as task 15.
- SimulatePrincipalPolicy gathers the source entity's stored policies
  the way the authorizer does (a user with its groups and boundary, a
  group alone, or a role), adds `PolicyInputList`, replaces the boundary
  with `PermissionsBoundaryPolicyInputList` when given, and calls
  `evaluate()` for each action and resource. Its context is only the
  `ContextEntries`, as on AWS. `MissingContextValues` lists the
  condition keys of the statements covering the action that the context
  lacks. SimulateCustomPolicy evaluates its inputs alone, as `CallerArn`
  or an unnamed user. Both are checked against every corpus `evaluate`
  case they can express.
- GetAccountAuthorizationDetails lists only the AWS-managed policies the
  account uses, as AWS does, and pages users, groups, roles and policies
  with one marker. GetAccountSummary reports this cluster's enforced
  quotas, and AWS's defaults for the account-wide counts it does not
  enforce.

**Terraform coverage.** The provider's call list is taken from a recorded
`terraform apply` and `destroy` against real AWS, or from the provider
source. The instance-profile calls and anything else it needs are added in
task 15. The recording uses IAM only, which is free, but it is still
real-cloud and needs Clark's go. Reading the provider source is the
zero-spend fallback.

## Component 9: STS and session tokens (Requirement 10)

**Token format.**

```
token = base64url( 0x01 | kid (8 bytes) | nonce (12) | AES-256-GCM(payload) | tag (16) )
payload (CBOR, via the existing cbor serializer) = {
  v: 1, acct, principal_arn, role_id, session_name, source_identity,
  iat, exp, access_key_id, secret_access_key,
  session_policies: [ JSON text | managed ARN ], tags, transitive_tag_keys,
  chained: bool, mfa: false, issuer_kind: role|federated|session_token
}
```

The secret travels inside the sealed token, so a replica can verify an
`ASIA` signature with no stored state. The `kid` selects the STS key. A key
older than the retention window (Requirement 10.4) makes the token
`ExpiredToken`. The packed size limit mirrors AWS: session policy plus tags
compressed, refused with `PackedPolicyTooLarge` past the limit.

**AssumeRole.**
1. Authenticate the caller.
2. Evaluate `sts:AssumeRole` in the caller's account, against the role ARN.
3. Evaluate the role's trust policy, with `sts:ExternalId`,
   `sts:RoleSessionName`, `sts:TransitiveTagKeys`, `aws:RequestTag/*` and
   `sts:SourceIdentity`.
4. Evaluate `sts:TagSession` and `sts:SetSourceIdentity` when they apply.
5. Mint the token.

No Raft write takes place. **GetSessionToken** wraps a user's identity. Its
token has `issuer_kind=session_token` and the user's policies apply.

**GetFederationToken** (Requirement 10.8). The caller must be a user (or
the root) signing with an `AKIA` key; the call is evaluated as
`sts:GetFederationToken` against
`arn:aws:sts::<account>:federated-user/<Name>`. The token has
`issuer_kind=federated` and carries the calling user's ARN as well as the
federated-user ARN. At request time the evaluator gathers the calling
user's identity policies (current ones, as AWS does: a policy change
applies to live federated sessions) and intersects them with the session
policies in the token. With no session policy the intersection is empty.
For the resource-policy step:
- a statement naming the federated-user ARN allows on its own;
- a statement naming the user's ARN still needs the session policies;
both are the same "session ARN" and "user ARN" exceptions the evaluator
already has for role sessions (step 4 of Component 6).

**Temporary-credential limits** (Requirement 10.10). The Query router
checks `issuer_kind` before evaluation: `session_token` tokens may call
only AssumeRole and GetCallerIdentity in STS and nothing in IAM;
`federated` tokens may call only GetCallerIdentity in STS and nothing in
IAM. These refusals are `AccessDenied` with AWS's wording and are not
policy decisions, so no policy can lift them. Any `SerialNumber` or
`TokenCode` parameter is refused the same way.

**DecodeAuthorizationMessage** (Requirement 10.9). `decision` gains an
optional `encoded_message`, filled only when the caller of `evaluate()`
asks for it (the `aws_request_authorizer` concept's
`want_encoded_message` flag; off by default, and off for the IAM and STS
handlers). The message is a CBOR record of the decision, the matched and
failed statements and the request context, sealed exactly like a session
token (same `kid` scheme, its own `0x02` version byte and associated data
`"authz-message" | account`, so a session token can never be decoded as a
message or the reverse). DecodeAuthorizationMessage checks
`sts:DecodeAuthorizationMessage`, unseals under the caller's account, and
renders AWS's JSON. A message older than the STS key retention window
fails with `InvalidAuthorizationMessageException`, AWS's error.

**As built (task 17, `iam_sts_token.hpp`, `iam_sts_api.hpp`).**
- The token is `base64url(0x01 | kid8 | sealed)`, where `kid8` is the
  first 8 bytes of SHA-256 of the STS key's id and `sealed` is
  `seal_secret`'s nonce, tag and ciphertext (the same AES-256-GCM helper
  that seals access-key secrets), with associated data
  `"kythira-sts-session-v1" | kid8`. The CBOR payload is written by a
  small codec in `iam_sts_token.hpp` rather than the Raft serializer, so
  the token format does not move with the log format.
- A token whose `kid8` matches no held key reads as `ExpiredToken`: a
  dropped key is the only way an honest token loses its key. A token
  that fails to open under a held key is `InvalidClientTokenId`, as is
  one presented with an access key id other than the one sealed in it.
- AssumeRole authorizes `sts:AssumeRole` once, through the authorizer,
  with the trust policy as the resource policy and the role's account as
  the resource account, so same-account trust alone allows and
  cross-account needs both sides (INV-STS-B1). A missing role is
  `AccessDenied`, as on AWS, so role names cannot be probed. Role
  chaining is capped at one hour here already (task 18 adds the rest of
  its rules).
- Parameters of later tasks (`Policy`, `PolicyArns`, `Tags`,
  `TransitiveTagKeys`, `SourceIdentity`) and MFA (`SerialNumber`,
  `TokenCode`) are validated where the rule is task 17's, then refused,
  never ignored: a session that silently dropped a session policy would
  be broader than asked.
- `ASIA` key ids are `ASIA` + 8 base32 characters of the account + 8
  random ones, so GetAccessKeyInfo answers for a temporary key without
  opening its token.
- `iam_node` serves STS on the leader, as it does IAM, signed for
  service `sts`; the AWS CLI scenario assumes a role and calls
  GetCallerIdentity with the session credentials.

**As built (task 18).**
- `PolicyArns` travel in the token as ARNs and are read when each
  request is evaluated, as AWS does, so a token stays small. A session
  policy that no longer parses or exists still counts, as one granting
  nothing: dropping it would leave a role session with no session policy
  and so unlimited by one.
- Session tags are the request tags of `sts:AssumeRole`, so a trust
  policy can condition on `aws:RequestTag/*` and `aws:TagKeys`; then
  `sts:TagSession` is authorized with the same request. A chained call
  carrying inherited transitive tags needs `sts:TagSession` too, as on
  AWS. The token holds the session tags and which are transitive;
  principal tags are merged with the role's at request time
  (INV-STAG-5).
- `SourceIdentity` needs `sts:SetSourceIdentity`, is kept down a chain
  (which then also needs `sts:SetSourceIdentity`), and cannot be changed
  there.
- `PackedPolicySize` is the session-policy characters (white space not
  counted) plus the ARNs, as a percentage of 2048. Tags are bounded by
  their own count and length limits rather than the packed size.
- Revocation is AWS's inline deny on `aws:TokenIssueTime`, which the
  token's `iat` supplies; nothing in STS tracks sessions.

**As built (task 26).**
- GetFederationToken mints an `issuer_kind=federated` token holding the
  federated-user ARN, `aws:userid` `<account>:<Name>`, the calling
  user's ARN (empty for the root) and the session policies and tags. It
  takes `Tags` (as the request tags of `sts:GetFederationToken`) but
  refuses `TransitiveTagKeys` and `SourceIdentity`, which AWS does not
  accept on it. `PackedPolicySize` is always reported.
- The root's federated session has no user to gather policies from. The
  authorizer gives it an allow-everything identity policy, so the
  session policies alone decide, which is what the root's unlimited
  identity side means once intersected.
- The temporary-credential limits were already in STS's dispatcher
  (task 17's `limit_credentials`); the IAM Query router now refuses
  GetSessionToken and GetFederationToken credentials before any handler
  runs, with `AccessDenied` ("Cannot call <action> with session
  credentials"). `credential_of` moved to the Query API's `detail` so
  both routers share it.
- `aws_auth::authz_request` gained `want_encoded_message` and
  `authz_decision` gained `encoded_message` (core, so any service module
  can ask). The message is sealed with `sts::seal_message`: version byte
  `0x02`, associated data `"kythira-authz-message-v1|" | account | "|" |
  kid8`. It carries its `iat`; DecodeAuthorizationMessage refuses one
  older than 36 hours (the key retention window) or whose key is gone.
  The rendering follows AWS's shape; a matched statement carries its
  `statementId` and effect but not the policy's name, which stays in
  the audit log (Requirement 6.5). Condition keys are listed under their
  normalized (lower-cased) names.
- `iam_authorizer::now()` exposes its clock so DecodeAuthorizationMessage
  dates messages as the authorizer that sealed them does.

## Component 10: Web identity (Requirements 11–12)

**`oidc_jwks_cache`.**
- Keyed by issuer URL.
- Fetches discovery and then `jwks_uri` with `cpp-httplib` over TLS using the
  system trust store.
- Has a 5 s timeout and a 64 KiB cap, and checks the scheme and host
  allowlist.
- Resolves names itself and refuses private, loopback and link-local
  addresses when the allowlist is enabled. It connects to the address it
  checked, so DNS rebinding cannot defeat the check.

**AssumeRoleWithWebIdentity.**
1. Decode the JWT.
2. Find the provider by `iss` in the role's account.
3. Check `aud` against the provider's client ids.
4. Verify the signature with `jwt.hpp`.
5. Check the times.
6. Build the federated principal and the condition keys, then evaluate the
   trust policy.

**The Kythira issuer** is just a provider whose URL is
`--oidc-issuer-url`. Its `kythira:*` claims are exposed as
`<host/path>:kythira:node_id` and similar keys. When `iam_node` runs beside
the CA cluster, the JWKS can be read from the CA's replica instead of over
HTTPS (`--kythira-issuer-local`).

**The direct certificate exchange** (Requirement 12.3) is
`POST /v1/iam/credentials` with `roleArn`, `sessionName` and
`durationSeconds`. The client certificate supplies the identity, exposed as
`kythira:cert/san-uri`, `kythira:cert/san-dns` and `kythira:cert/serial`.
The principal is `arn:<p>:iam::<acct>:oidc-provider/<issuer>` so trust
policies stay in one style.

**As built (task 19, `iam_oidc_jwks.hpp`).**
- `jwks_cache` takes a `fetcher`, so the cache, its one-hour age, the
  single refetch on an unknown `kid` (no more often than every 30 s per
  issuer, so made-up `kid`s cannot make this endpoint hammer an issuer)
  and its caps (256 issuers, 64 keys each) are tested without a network.
  The discovery document must name the issuer it was fetched for, and
  only keys with `use` absent or `sig` are kept.
- `https_fetcher` checks the URL (https only; the allowlist when set),
  resolves the name itself, refuses it if any address is not public
  (private, loopback, link-local and metadata, CGNAT, multicast, ULA,
  and IPv4-mapped forms of those), and pins cpp-httplib to the address it
  checked with `set_hostname_addr_map`, so SNI and the certificate check
  still use the name but a second DNS answer cannot rebind it. Redirects
  are not followed. `--oidc-proxy` sends fetches through a proxy, which
  then resolves for itself; the allowlist check still runs first.
  `--oidc-ca-file` replaces the system trust store for a test issuer.
- With no `--oidc-allowed-hosts` any host is fetched and private
  addresses are allowed, which a compose network's issuer needs; a
  deployment that serves untrusted callers sets the allowlist.
- AssumeRoleWithWebIdentity is answered unsigned (`iam_node` routes an
  unauthenticated request whose `Action` is that one to STS). The trust
  policy is evaluated by `iam_authorizer::authorize_web_identity` with the
  provider as a `web_identity` principal and no identity policies, with
  `<host/path>:sub`, `:aud` (the `azp` when present), `:oaud` and `:amr`.
  A missing role is AccessDenied only after the token has verified, so
  role names cannot be probed with junk tokens. Session policies are
  accepted; session tags and source identity from token claims are not
  read yet.

**As built (task 20).**
- `--kythira-issuer <https-url>` names the CA cluster's
  `--oidc-issuer-url`; IAM knows it as an ordinary OIDC provider
  `<host/path>`, which an account still has to create before any role can
  trust it. `--kythira-issuer-local <jwks-url>` points `jwks_cache` straight
  at a CA member's `/.well-known/jwks.json` (`set_direct`), skipping the
  discovery document. That fetch has no host allowlist, since the member
  is usually on a private address, and trusts `--oidc-ca-file` when set,
  else the cluster's own CA file, never the system store.
- Every string claim under `kythira:` becomes `<host/path>:kythira:<name>`.
  A composite `kythira:node_id` (`provider:scope:native`) is also split
  into `:kythira:node_provider`, `:node_scope` and `:node_native_id`, so a
  trust policy can say "any EC2 node in us-east-1" with `StringEquals` and
  `StringLike`. A numeric id has no parts and none are invented.
- The direct exchange takes the STS parameter names (`RoleArn`,
  `RoleSessionName`, `DurationSeconds`) in a form body rather than the
  camel-case names above, so the same validators and errors apply. It
  answers JSON, `{"credentials":{accessKeyId, secretAccessKey,
  sessionToken, expiration}, "assumedRoleUser":{arn, assumedRoleId}}`, or
  `{"code","message"}`. It is off unless `--iam-credentials-exchange` is
  given, needs `--kythira-issuer`, and is refused with
  `--insecure-plaintext`. The HTTPS server now passes the verified client
  certificate's DNS SANs and lower-case hex serial to handlers beside its
  URI SANs.
- The exchange still needs the account to have registered the Kythira
  issuer as a provider: the certificate is trusted because the account
  chose to trust that issuer, not because `iam_node` trusts the CA for
  TLS.
- `iam_kythira_issuer_test` runs a ca_cluster_node and three `iam_node`
  members as processes. A workload certificate and its token are
  exchanged both ways and the sessions sign IAM and STS calls. The test
  uses a workload, not a node certificate: a node certificate needs the
  CA's peer-enrollment MAC, and the node claims are covered by
  `iam_sts_test`. Its test PKI root has its own common name, because two
  roots with one subject and no key identifiers in one trust file are
  told apart by trying the first.

## Component 11: Redis (Requirement 13)

The Redis module never includes IAM. The two meet only in
`redis_gateway_node`'s composition root, as the `redis-gateway-module`
design lays out:

```
 services/redis (gateway)  ──calls──►  credential_verifier (core concept)
                                                ▲
 services/iam  iam_redis_verifier  ──implements─┘
 wired in services/redis/cmd/redis_gateway_node only when CONFIG_REDIS_GATEWAY_IAM
 (depends on REDIS_GATEWAY && IAM); the include is a `module-adapter` line in
 services/redis/boundary-allow.txt
```

**ACL syntax** (in the Redis module, task 21):
- `auth=password|iam|both` is parsed as a reserved attribute, like `cert=`.
  A repeated or unknown value is an error.
- An absent value means `password`.
- `redis-gateway-module` task 1 pins today's behaviour, where `auth=iam`
  reads as a key prefix. Task 21 changes that pinned test on purpose.

**`iam_redis_verifier::verify(credential_request)`** (in the IAM module)
checks a `credential` of the form
`<host>/?Action=connect&User=…&X-Amz-…`:

1. Run `sigv4_parse` with service `elasticache` and
   `payload_modes=UNSIGNED`. The presigned canonical request uses `GET`, an
   empty path and the query as given.
2. Check that `User` equals `credential_request.user`, and that the host
   equals the configured cache name.
3. Authenticate and authorize `elasticache:Connect` on both ARNs against the
   local replica.
4. Return `credential_accepted{principal ARN, expires = min(now + 12 h,
   session expiry)}`, or `credential_denied`.

**The gateway** then applies the ACL user's role and key prefixes. Its
per-connection deadline (`redis-gateway-module` task 6) closes the
connection unless a later AUTH extends it. Password AUTH is unchanged. A
denial is a `WRONGPASS` counted by `redis_auth_limiter`.

**Configuration.** `--iam-account-id`, `--iam-region`, `--iam-cache-name`,
and the replica flags shared with every front end. These are registered by
the adapter, so a build without `CONFIG_REDIS_GATEWAY_IAM` has none of them.

**As built (task 21).**
- The ACL's `auth=` is parsed before the secret field, because it changes
  what `nopass` (no `cert=` needed) and `disabled` (no password, but still a
  login through the verifier) mean for an `iam` or `both` user. That makes
  `disabled` the rolling-upgrade guard the requirement asks for: an old
  build refuses the user, a new one admits it by token only.
- The gateway asks the verifier first for a `both` user and falls back to
  the password in the same admission slot when it refuses. A PBKDF2 check
  costs far more than a local token check, so the cheap test goes first.
- botocore 1.35.0 hashes the empty body into the presigned canonical
  request (as SigV4 does outside S3), not `UNSIGNED-PAYLOAD` as written
  above; the verifier checks that form and, on a signature mismatch, S3's
  `UNSIGNED-PAYLOAD` form, so either kind of presigner works.
- The daemon is configured by environment, so the flags above are
  `KYTHIRA_IAM_ACCOUNT_ID`, `KYTHIRA_IAM_REGION`, `KYTHIRA_IAM_CACHE_NAME`
  and the replica's `KYTHIRA_IAM_VOTERS`, `_REPLICA_NODE_ID`,
  `_REPLICA_RPC_BIND`, `_REPLICA_RPC_PORT`, `_DATA_DIR`,
  `_UNSEAL_KEY_FILE`, `_RPC_TLS_CERT/_KEY/_CA`, `_MAX_STALENESS_SECS` and
  `_ARN_PARTITION`. They live in the composition root's `iam_auth.cpp`,
  not in the adapter, so the adapter stays usable by front ends configured
  by flags.
- `iam_embedded_replica.hpp` is the reusable piece every front end needs:
  `iam_replica` over TLS TCP RPC with file persistence, the KEK cache from
  the unseal passphrase, a registry the caller fills, and the authorizer
  with the replica's freshness. It has no plaintext mode, since it holds
  the unsealing passphrase.
- `scripts/check_module_boundaries.py` gained the `module-adapter` line
  kind: `<cmd/ file> module-adapter <other module's header> <why>`, allowed
  only for the composition root. Redis lists the three IAM headers
  `iam_auth.cpp` includes.
- `CONFIG_REDIS_GATEWAY_IAM` defaults to `y` whenever the gateway and IAM
  are both on, so every CI leg that builds both covers the wiring.

**As built (task 22).**
- The connection deadline, its re-arming on re-AUTH, and the per-address
  failure budget were already in the gateway from `redis-gateway-module`
  task 6, so this task is mostly tests: re-AUTH moving the deadline, refused
  tokens spending the budget until even a good token is turned away, and an
  IAM user's commands forwarded from a follower over the internal link.
- Requirement 3.8 also names a per-access-key budget, which the gateway's
  limiter cannot keep (it never sees the key id). `iam_redis_verifier`
  keeps it with core's `sigv4::failure_limiter` in per-key-only mode:
  100 failed signatures per key per minute by default, charged only for
  authentication failures, never for a stale replica. Key ids are not
  secret, so the budget is generous: anyone can spend it.
- The verifier takes a second, steady clock for those windows, so the
  tests drive both the 12-hour deadline and the failure window by hand.
- The rolling-upgrade scenario's ACL gains `iam-cache`, an `auth=iam`
  user with the secret `disabled`. The new case proves old and new builds
  load that file and both refuse the user. The token path itself is not in
  the container scenario, which has no IAM cluster; `iam_embedded_replica_test`
  and the gateway tests cover it.

**As built (task 23).**
- `services/iam/tests/docker/conformance.yml` is one stack: three voters,
  one `ca_cluster_node` with its OIDC issuer at `https://ca-1` (HTTPS on
  443 with a test-PKI certificate, so the issuer URL needs no port), a
  test issuer serving a volume the driver writes, and two Redis gateways
  forming one cache cluster, each with an embedded IAM replica (ids 6 and
  7). One image, `kythira-iam-conformance:dev`, carries all five binaries;
  each service picks its own with `entrypoint`.
- The accounts are fixed with `create-account --account-id`
  (111111111111 and 222222222222), so the gateways name theirs in their
  environment before it exists. The first STS key is rotated in after the
  first account, because that account sets the sealing salt.
- The checks are in `conformance/driver.py`, run in a pinned boto3 image
  (Alpine, the base the test issuer already pulls). It prints one
  `PASS`/`FAIL` line per check and the harness turns each `FAIL` into a
  test error. The SDK credential providers run in child processes with
  only `AWS_ENDPOINT_URL_STS` and the standard files or variables, as in a
  user's shell; the harness then runs the same three providers in the AWS
  CLI v2 from files the driver leaves in a shared volume.
- DecodeAuthorizationMessage is checked on what the stack can produce: a
  message it did not seal is refused and a caller without the permission
  is denied. No service here asks for an encoded message (IAM's Query API
  does not, matching AWS), so a message from a denied request is decoded
  only in `iam_sts_test`. The first service module that sets
  `want_encoded_message` adds the end-to-end check.
- STS allows five minutes of clock skew, so the expired-token check uses
  a token that expired ten minutes before.
- `KYTHIRA_IAM_CONFORMANCE_NO_REDIS=1` leaves the gateways out, for a host
  that cannot build `redis_gateway_node`; the driver skips its Redis checks.
  CI always runs the whole stack, under Docker and rootless Podman, in
  `iam-module.yml`'s container job, which now sets up its runtime with the
  shared `setup-container-runtime` action.

**As built (task 24, Terraform half).**
- `conformance/terraform/main.tf` pins Terraform 1.9.8 and the AWS
  provider 5.70.0 and covers Requirement 15.2: two users (one with a
  permissions boundary, tags, an inline policy and an attachment), a group
  with a member, an inline policy and an attachment, a boundary policy and
  a customer managed policy, an OIDC provider, a web identity role with a
  boundary, inline policy and attachment, and an EC2 role with an AWS
  managed attachment and an instance profile.
- The conformance harness runs it in the pinned `hashicorp/terraform`
  image as the account root: apply, a second plan with
  `-detailed-exitcode` that must find no changes (so every resource reads
  back as written, including policy JSON and tags), then destroy. Any
  `NotImplemented` in the output fails the case even when the provider
  tolerates it. Users are created without `force_destroy`, so destroy does
  not call the out-of-scope credential listings (signing certificates, SSH
  keys, MFA, login profiles).
- The configuration was written without a local run (the sandbox may not
  fetch Terraform); the first CI run is its test.
- The simulator half is `tests/tools/simulate_corpus_on_aws.py`: the 73
  corpus `evaluate` cases iam_query_api_test already sends to Kythira's
  SimulateCustomPolicy, sent to AWS's with the example account replaced by
  the caller's and each context value typed (`ip`, `date`, `numeric`,
  `boolean`, else `string`, with the `List` form for arrays). Each
  disagreement becomes a case in `corpus/evaluate/aws_simulator.json` with
  AWS's decision expected. It needs only `iam:SimulateCustomPolicy` on
  `*`, creates nothing and is free; it runs by hand, not in CI.
- First run, 2026-11-10 (kythira-dev, boto3 1.43): 71 of 73 cases agreed.
  The two that did not are places where the simulator departs from the
  enforcement AWS documents: it denies a caller exempted by a Deny
  `NotPrincipal` listing the user's ARN with its account, and it lets a
  same-account grant to the account escape a permissions boundary, which
  the boundaries page reserves for a grant naming the user's ARN. Both
  corpus cases keep the documented decision and carry the simulator's in
  `aws_simulator` (Clark, 2026-11-10: follow real enforcement, which the
  docs describe); the script reports them as
  known divergences.

## Configuration

| Flag | Daemon | Default |
|---|---|---|
| `--oidc-issuer-url` | ca_cluster_node | unset (issuer off) |
| `--oidc-audience` (repeatable) | ca_cluster_node | none |
| `--oidc-alg` | ca_cluster_node | `RS256` |
| `--oidc-key-rotation-days` | ca_cluster_node | 90 |
| `--oidc-max-token-lifetime-secs` | ca_cluster_node | 43200 |
| `--oidc-workload-uri-prefix` | ca_cluster_node | unset |
| `--oidc-publish-url` | ca_cluster_node | unset |
| `--arn-partition` | iam_node, front ends | `aws` |
| `--sts-regions` | iam_node | `us-east-1` |
| `--sts-key-rotation-hours` | iam_node | 24 |
| `--iam-max-staleness-secs` | front ends | 60 |
| `--iam-audit-log`, `--iam-audit-reads` | iam_node, front ends | stderr, off |
| `--oidc-fetch-allowlist` | iam_node | unset (any public https host) |
| `--admin-uri-san` | iam_node | none |

## Correctness properties

1. **Determinism.** The same log gives byte-identical `get_state()` on every
   member and replica, including after restore from a snapshot.
2. **Single evaluator.** Every allow decision in the codebase traces to one
   `evaluate()` call.
3. **Fail closed.** Any parse error, unseal failure, stale replica, missing
   key or exception gives a deny or an authentication error, never an allow.
4. **Corpus agreement.** `evaluate()` agrees with the corpus on every case,
   and with AWS's simulator where Requirement 15.5 has run.
5. **Secrecy.** No plaintext secret or token is in the Raft log, snapshots,
   logs or metrics. A test greps captured output for planted secrets.
6. **Rotation safety.** For every OIDC and STS key rotation, a token minted
   just before the switch verifies until its expiry.

## Error handling

| Situation | Result |
|---|---|
| Replica stale or not yet caught up | `ServiceUnavailable`; the audit line notes staleness |
| Unseal key missing on a replica | Authorization works; authentication answers `ServiceUnavailable` and logs once a minute |
| IAM Raft group without a leader | Writes return `ServiceFailure` after the forwarding timeout; reads and authentication carry on |
| JWKS fetch fails | `InvalidIdentityToken` ("Couldn't retrieve verification key…"), AWS's message |
| Cloud exchange fails | Old credentials until expiry, then the client's call fails with the provider's error |

## Risks

- **Fidelity drift.**
  - AWS's evaluation has edge cases the documentation states only loosely,
    such as boundary interplay and `NotPrincipal`.
  - Mitigation: the corpus, plus the optional differential run against
    AWS's simulator (Requirement 15.5). When the two disagree, AWS wins and
    a corpus case is added.
- **Terraform's call surface.**
  - The provider reads more than it writes. For example, role deletion lists
    instance profiles, attached policies and inline policies.
  - Mitigation: task 15 records the actual calls before building, and
    task 23 fails on any `NotImplemented`.
- **Learner fan-out.**
  - Each front end is a learner of the IAM Raft group, so the leader sends
    AppendEntries to every gateway.
  - At an expected scale of tens of front ends with low write volume, this
    is fine. Past about 50 learners, the follow-up is a learner that
    replicates from a follower.
  - That follow-up is not needed now and is noted in `doc/iam.md`.
- **OCI token exchange.**
  - The OCI identity-domain token exchange to a UPST is newer than the other
    clouds' mechanisms, and its availability by region and tenancy type is
    not confirmed.
  - If task 4 finds it missing, OCI keeps using instance principals, which
    already need no stored key, and the OCI exchange provider is dropped
    with a note.
- **Issuer reachability.**
  - AWS STS and GCP fetch the discovery document themselves.
  - A cluster on a private network must use `--oidc-publish-url`.
  - Azure and Alibaba also fetch discovery. All four are covered by the
    real-cloud test in task 5.
- **Secrets on front ends.**
  - Giving every replica the unseal passphrase widens where it lives.
  - The alternative, verifying signatures on `iam_node` over RPC, defeats
    "no network call per request".
  - The passphrase is read from a file with `private_file` permissions and
    can be held in a cloud secret store. `doc/iam.md` states the trade-off.
