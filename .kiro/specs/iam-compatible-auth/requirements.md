# Requirements Document — IAM-Compatible Authentication and Authorization

## Introduction

This spec adds an AWS IAM-compatible identity layer to Kythira, and makes
Kythira an OpenID Connect (OIDC) issuer so its nodes and workloads can obtain
short-lived credentials in every cloud it runs on.

Two goals, built in this order:

1. **Kythira as an OIDC issuer (outbound federation).** A node or workload
   that holds a certificate from the Kythira CA cluster exchanges it for a
   signed JWT. AWS, GCP, Azure, Alibaba Cloud and OCI each trade that JWT for
   their own short-lived credentials. No cloud keys are stored on Kythira
   nodes.
2. **An AWS IAM-compatible layer (inbound authorization).** Kythira serves the
   IAM Query API and STS. It verifies AWS Signature Version 4 (SigV4) on
   requests to Kythira-served AWS-compatible APIs, and evaluates IAM policies
   with AWS's semantics. The first consumer is the Redis gateway. The layer
   is built before the Kinesis- and DynamoDB-compatible APIs, which plug into
   it, and it is designed so an S3-compatible API can plug in later.

The scoping document is the Claude Doc "IAM for Kythira — scoping" (project
thread "Scope IAM support on Kythira", 2026-11-02). Its decisions were made by
Clark on 2026-11-02:

| Question | Decision |
|---|---|
| Scope | Redis now. Design so S3 can be added later. Kinesis and DynamoDB come after this spec. |
| Tenancy | Multiple accounts with cross-account roles. |
| Management | The IAM Query API, so the AWS CLI and the Terraform AWS provider work. |
| Policy fidelity | Match AWS. |
| Federation | Inbound OIDC first, then Kythira-CA web identity. |
| Redis ACL | Supports both ACL users and IAM credentials. |
| ARN partition | Configurable. |
| Propagation | A delay after an IAM change is acceptable, as on AWS. |
| Priority | Before Kinesis and DynamoDB. |
| OIDC issuer | Yes, before the IAM layer. |

### What exists today (read from `main` on 2026-11-02)

- **Transport identity.** Peer certificates are bound to node ids by a
  reserved SAN: DNS `ca-cluster-node-<n>` for numeric ids, URI
  `urn:kythira:node:<canonical>` for textual and composite ids
  (`include/raft/ca_http_helpers.hpp`, `peer_identity_name`). TLS-TCP RPC and,
  opt-in, CoAP DTLS enforce that binding. The HTTP transports check the chain
  only.
- **The CA cluster** (`cmd/ca_cluster_node`, `include/raft/ca_state_machine.hpp`)
  issues, renews and revokes certificates and serves a CRL. The CA private key
  is kept in the Raft log, sealed with AES-256-GCM under a key derived from an
  unseal passphrase.
- **Client auth.** The Redis gateway (`include/raft/redis_acl.hpp`) has users
  with PBKDF2 secrets, three roles, key-prefix scopes and certificate login.
  The cache HTTP front end reuses the same ACL. The CA APIs use a Bearer token.
- **Request signing is client-side only.** S3 and ACM-PCA sign through the AWS
  SDK. Alibaba (`alibaba_signing.hpp`) and OCI (`oci_signing.hpp`) signers are
  hand-written. Nothing verifies SigV4.
- **Cloud credentials** are static keys or per-cloud mechanisms such as OCI
  instance-principal federation (`oci_federation.hpp`) and instance metadata
  (`self_node_id.hpp`).
- **Learners.** `raft::add_learner()` exists (`include/raft/raft.hpp`). A
  learner replicates and applies the log without voting.

### Where the code lives

IAM is a service module under `services/iam/`, following the module pattern
of the `redis-gateway-module` spec (PR #675). Clark chose this on
2026-11-02. Pieces that core or other modules also need stay in core
`include/raft/`:
- the JWT/JWS helpers, used by the CA cluster's issuer;
- the SigV4 verifier and the Query protocol codec, which the Kinesis,
  DynamoDB and S3 modules will share;
- the cloud credential sources;
- the request-authorizer concept that other modules call.

Requirement 17 states the boundary.

### Glossary

- **Account:** a 12-digit account id and everything owned by it. It
  corresponds to an AWS account.
- **Principal:** an IAM user, an assumed-role session, an account root user,
  or a federated web identity.
- **Partition:** the second field of an ARN (`aws`, `aws-cn`, or a value the
  operator chooses).
- **IAM group (Raft):** the Raft group that holds the `iam_state_machine`. It
  is called the *IAM Raft group* to avoid confusion with IAM user groups.
- **Replica:** a learner member of the IAM Raft group, run inside a front-end
  process so authorization never needs a network round trip.
- **Issuer:** Kythira's OIDC issuer, served by the CA cluster.

### Out of scope

- AWS Organizations: service control policies, resource control policies and
  account creation through `organizations:CreateAccount`. Accounts are created
  with the Kythira admin CLI. The evaluator leaves a seam where SCPs would go.
- SAML federation (`AssumeRoleWithSAML`) and IAM Identity Center.
- The S3, Kinesis and DynamoDB APIs themselves. Only their hooks are designed
  here.
- IAM Access Analyzer, credential reports, service-last-accessed data, MFA
  devices, login profiles (console passwords), server certificates, SSH keys
  and service-specific credentials. The API returns `NotImplemented` for
  their actions (Requirement 9.7).
- SigV4a (multi-region asymmetric signatures).
- Moving the cache HTTP front end's Basic/Bearer auth to IAM. That can follow
  once Requirement 13 works.

## Requirements

### Requirement 1: OIDC issuer

**User Story:** As an operator, I want each Kythira node to get a signed
identity token from the cluster, so that it can obtain cloud credentials
without any stored cloud keys.

#### Acceptance Criteria

1. THE CA cluster SHALL serve `GET /.well-known/openid-configuration` and
   `GET /.well-known/jwks.json`. The discovery document SHALL contain at least
   `issuer`, `jwks_uri`, `response_types_supported` (`id_token`),
   `subject_types_supported` (`public`),
   `id_token_signing_alg_values_supported` and `claims_supported`.
2. THE issuer URL SHALL be configured (`--oidc-issuer-url`), SHALL use
   `https`, and SHALL have no query string or fragment. Every token's `iss`
   claim SHALL equal it byte for byte.
3. THE issuer SHALL sign with RS256 (2048-bit or larger RSA) by default, and
   SHALL support ES256 as an option. Each key SHALL carry a `kid`.
4. THE signing private key SHALL be generated on the leader and stored in the
   CA state machine, sealed with the same unseal-derived AES-256-GCM scheme as
   the CA key. `apply()` SHALL only record the sealed key and SHALL NOT read a
   clock or random source.
5. THE issuer SHALL rotate its signing key on a configured period (default 90
   days). The JWKS SHALL publish the new key at least 24 hours before tokens
   are signed with it, and SHALL keep the old key for at least the maximum
   token lifetime after its last use.
6. `POST /v1/oidc/token` SHALL require a TLS client certificate that chains to
   the Kythira CA and is not revoked. The subject SHALL come from the
   certificate's reserved SAN: `node:<canonical node id>` for a node
   certificate, or `workload:<URI SAN>` for a workload certificate whose URI
   SAN is under a configured prefix. Any other certificate SHALL get 403.
7. THE request SHALL name one `audience`. It SHALL be refused with 400 unless
   it is on the configured audience allowlist.
8. A token SHALL contain `iss`, `sub`, `aud`, `iat`, `nbf`, `exp` and `jti`,
   plus `kythira:node_id` or `kythira:workload` and the certificate's serial
   (`kythira:cert_serial`). Its lifetime SHALL default to 1 hour, SHALL be at
   most 12 hours, and SHALL NOT outlast the client certificate.
9. WHERE the clouds cannot reach the issuer URL, THE operator SHALL be able to
   publish the discovery document and JWKS to an object-store bucket
   (`--oidc-publish-url`). The leader SHALL republish after every key change
   and SHALL log an error and raise a metric if a publish fails.
10. Followers SHALL serve discovery and JWKS from their replica. Only the
    leader SHALL sign tokens; a follower SHALL proxy the token request to the
    leader or answer 503 with `no_known_leader`, as the other `/v1/*` routes do.

### Requirement 2: Cloud credential exchange

**User Story:** As an operator, I want Kythira's cloud clients to get their
credentials by exchanging a Kythira OIDC token, so that no node holds a
long-lived cloud key.

#### Acceptance Criteria

1. THE system SHALL provide a `kythira_oidc_token_source` that fetches and
   caches a token per audience, and refreshes it when less than 20% of its
   lifetime is left.
2. THE system SHALL provide one exchange provider per cloud:
   - AWS: `sts:AssumeRoleWithWebIdentity`.
   - GCP: the Security Token Service token exchange for a Workload Identity
     Federation provider, then optional service-account impersonation.
   - Azure: Entra ID client-credentials flow with a federated
     `client_assertion`.
   - Alibaba Cloud: `AssumeRoleWithOIDC`.
   - OCI: identity-domain OAuth token exchange to a User Principal Session
     Token. If a target region lacks it, the spec's Risks section applies.
3. EACH provider SHALL refresh its cloud credentials before they expire. It
   SHALL retry with the existing `async_retry` backoff, and SHALL keep serving
   the old credentials until they actually expire.
4. THE existing clients (`aws_s3_client`, the ACM-PCA provider,
   `gcp_gcs_client`, `azure_blob_client`, `alibaba_oss_client`,
   `oci_object_storage_client`) SHALL accept the provider as a credential
   source, chosen by configuration. The current credential sources SHALL keep
   working unchanged.
5. A real-cloud test SHALL show each provider getting credentials and making
   one read-only call. It SHALL follow `kythira-real-cloud-test-rules`: it
   runs only on Clark's go, tags and cleans up every resource, and records
   its spend.

### Requirement 3: SigV4 verification

**User Story:** As a client using an AWS SDK, I want my signed requests
accepted by Kythira exactly as AWS would accept them, so that I need no
custom client code.

#### Acceptance Criteria

1. THE verifier SHALL accept `Authorization: AWS4-HMAC-SHA256` headers and
   presigned query strings (`X-Amz-Algorithm`, `X-Amz-Credential`,
   `X-Amz-Date`, `X-Amz-Expires`, `X-Amz-SignedHeaders`, `X-Amz-Signature`,
   `X-Amz-Security-Token`).
2. THE canonical request SHALL be built exactly as AWS documents it. That
   covers URI encoding, the per-service rule that S3 does not double-encode
   the path or normalize it, sorted and trimmed headers, and query-parameter
   sorting. The verifier SHALL pass every case of AWS's published SigV4 test
   suite.
3. THE verifier SHALL check:
   - the credential scope's date, region and service;
   - that `host` and every `x-amz-*` header present are signed;
   - a 15-minute clock-skew window for header signatures;
   - `X-Amz-Expires` of 1 to 604800 seconds for presigned URLs.
4. THE verifier SHALL support `x-amz-content-sha256` values:
   - a hex payload hash;
   - `UNSIGNED-PAYLOAD`;
   - `STREAMING-AWS4-HMAC-SHA256-PAYLOAD`, with each chunk signature verified
     before its bytes are released;
   - `STREAMING-UNSIGNED-PAYLOAD-TRAILER` and
     `STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER`, with signed trailers.

   Each front end SHALL declare which forms it accepts. The S3 forms exist so
   an S3 API can be added without changing the verifier.
5. Signature comparison SHALL be constant time.
6. THE verifier SHALL cache derived signing keys per (access key, date,
   region, service). The cache SHALL be bounded and SHALL be invalidated when
   the key is deactivated or deleted.
7. Failures SHALL map to AWS error codes:
   - `IncompleteSignature`, `InvalidSignatureException` or
     `SignatureDoesNotMatch`, per protocol;
   - `InvalidClientTokenId` for an unknown key;
   - `ExpiredToken`;
   - `RequestExpired`;
   - `AccessDeniedException` or `AccessDenied`, per protocol.

   The message text SHALL match AWS's wording where AWS documents it.
8. Failed verifications SHALL be rate-limited per source address and per
   access key id, reusing the `redis_auth_limiter` design.

### Requirement 4: IAM data model and state machine

**User Story:** As an administrator, I want users, groups, roles, keys and
policies stored durably and consistently across the cluster, so that every
node makes the same decision.

#### Acceptance Criteria

1. `iam_state_machine` SHALL satisfy the `state_machine` concept
   (`include/raft/types.hpp`). Its `apply()` SHALL be deterministic. The leader
   SHALL stamp times, generated ids, nonces and sealed secrets into the
   command before proposing it.
2. It SHALL store, per account:
   - users, user groups and roles (with trust policies, maximum session
     duration and paths);
   - customer-managed policies with up to 5 versions and a default version;
   - inline policies and attachments;
   - permissions boundaries and tags;
   - access keys (id, sealed secret, status, create date, last used);
   - OIDC providers (URL, client ids, thumbprints).
3. AWS-managed policies (`arn:<partition>:iam::aws:policy/...`) SHALL be
   available read-only. They come from a bundled, versioned set that the
   build generates from a pinned snapshot. At least these are needed:
   `AdministratorAccess`, `ReadOnlyAccess`, `IAMFullAccess`,
   `IAMReadOnlyAccess` and the ElastiCache policies needed by
   Requirement 13.
4. Entity names, ids and limits SHALL follow AWS:
   - unique ids use the `AIDA`, `AGPA`, `AROA`, `ANPA` and `AKIA`/`ASIA`
     prefixes with 16 base32 characters;
   - names are case-insensitively unique per account;
   - size quotas are enforced: a managed policy at most 6144 characters, a
     role trust policy 2048, inline policies 2048 per user, 5120 per group and
     10240 per role, and 10 managed policies attached per entity. These are
     configurable upward.
5. Access-key secrets SHALL be sealed with AES-256-GCM under a key-encryption
   key derived from the unseal passphrase, with the access key id as
   associated data. The plaintext secret SHALL leave the cluster only once,
   in the `CreateAccessKey` response.
6. Each account SHALL have a root user. Its first access key SHALL be created
   by the admin CLI's `create-account` and written with `private_file.hpp`
   permissions.
7. `get_state()` and `restore_from_snapshot()` SHALL round-trip everything,
   and the snapshot SHALL carry a format version.
8. Access-key last-used data SHALL be batched. At most one Raft command per
   key per configurable interval (default 5 minutes; AWS reports
   last use only to the hour) SHALL be written, so authentication does not turn
   reads into writes.

### Requirement 5: Policy language

**User Story:** As an administrator, I want to write the same policy JSON I
would write for AWS, so that existing policies and tooling carry over.

#### Acceptance Criteria

1. THE parser SHALL accept policy versions `2012-10-17` and `2008-10-17`. It
   SHALL accept `Statement` as an object or an array, and `Sid`, `Effect`,
   `Principal`/`NotPrincipal`, `Action`/`NotAction`, `Resource`/`NotResource`
   and `Condition`. Each element SHALL accept a string or an array where AWS
   does.
2. Invalid documents SHALL be refused with `MalformedPolicyDocument` and a
   message naming the problem. Rules that differ by policy type SHALL be
   enforced: a `Principal` is required in trust and resource policies and
   forbidden in identity policies.
3. Action and ARN matching SHALL use AWS's `*` and `?` wildcards, with actions
   matched case-insensitively and resources case-sensitively.
4. Every documented condition operator SHALL be supported:
   - the `String*`, `Numeric*`, `Date*` and `Arn*` families;
   - `Bool`, `BinaryEquals`, `IpAddress`/`NotIpAddress` (IPv4 and IPv6 CIDR)
     and `Null`;
   - the `...IfExists` suffix on all of them;
   - the `ForAllValues:` and `ForAnyValue:` set prefixes, with AWS's
     empty-set results.
5. Policy variables SHALL be substituted in `Resource` and in condition
   values: `${aws:username}`, `${aws:userid}`, `${aws:PrincipalTag/...}`,
   `${aws:PrincipalAccount}`, `${aws:SourceIp}`, `${aws:CurrentTime}`,
   `${aws:TokenIssueTime}`, `${aws:EpochTime}`, `${aws:SecureTransport}` and
   the escapes `${*}`, `${?}` and `${$}`. A variable with no value SHALL make
   its statement not match, as on AWS.
6. THE request context SHALL provide the AWS global condition keys that apply
   to Kythira:
   - `aws:CurrentTime`, `aws:EpochTime`, `aws:SecureTransport`, `aws:SourceIp`
     and `aws:UserAgent`;
   - `aws:PrincipalArn`, `aws:PrincipalAccount`, `aws:PrincipalType`,
     `aws:PrincipalTag/*`, `aws:userid` and `aws:username`;
   - `aws:RequestTag/*`, `aws:ResourceTag/*`, `aws:TagKeys` and
     `aws:ResourceAccount`;
   - `aws:TokenIssueTime`, `aws:MultiFactorAuthPresent` (always false) and
     `aws:SourceIdentity`;
   - `aws:RequestedRegion` and `aws:CalledVia*`.

   Each service SHALL add its own keys. Keys that Kythira cannot provide,
   such as `aws:SourceVpc`, SHALL be absent, never invented.

### Requirement 6: Policy evaluation

**User Story:** As an administrator, I want Kythira to allow or deny a request
exactly as AWS would, so that a policy I test on AWS behaves the same here.

#### Acceptance Criteria

1. THE evaluator SHALL implement AWS's documented evaluation logic in order:
   1. Explicit deny in any applicable policy.
   2. The SCP seam (always allows; out of scope).
   3. Resource-based policies.
   4. Identity-based policies.
   5. Permissions boundaries.
   6. Session policies.

   It SHALL also apply AWS's special cases:
   - a same-account resource-policy allow for a user ARN or role-session ARN
     is not limited by that principal's boundary or session policy;
   - a same-account resource-policy allow naming the role ARN is limited by
     them.
2. Cross-account requests SHALL be allowed only when the principal's account
   allows them through identity policies and the resource's account allows
   them through the resource policy.
3. Role trust policies SHALL be evaluated as resource-based policies for
   `sts:AssumeRole`, `sts:AssumeRoleWithWebIdentity`, `sts:TagSession` and
   `sts:SetSourceIdentity`.
4. THE root user of an account SHALL be allowed everything in its own
   account, except where a resource policy explicitly denies it.
5. THE evaluator SHALL return the decision and which statement decided it.
   The reason SHALL be logged and SHALL NOT be shown to the client beyond
   AWS's messages.
6. A corpus of at least 300 cases SHALL be checked into
   `services/iam/tests/corpus/`
   and run in CI. Each case gives policies, a request context and the
   expected decision. Cases SHALL come from the AWS IAM User Guide examples
   and the evaluation-logic page, and SHALL cover every operator, every
   variable and each step of the evaluation order.
7. THE evaluator SHALL be the only place decisions are made. `SimulatePrincipalPolicy`,
   `SimulateCustomPolicy`, the authorizer hook and STS SHALL all call it.

### Requirement 7: Authorizer hook and propagation

**User Story:** As the author of a Kythira-served AWS API, I want one call that
authenticates and authorizes a request, so that each API only declares its
actions and resources.

#### Acceptance Criteria

1. THE system SHALL provide an `iam_authorizer`:
   - `authenticate(request)` returns a principal context or an AWS error;
   - `authorize(principal, action, resource ARN, extra keys)` returns a
     decision.
2. A service SHALL register its actions with the service prefix, the resource
   ARN templates and the service condition keys of each. Requests for an
   unregistered action SHALL be denied.
3. A front end SHALL hold an IAM replica (a learner of the IAM Raft group)
   and SHALL authenticate and authorize without a network call.
4. A change committed to the IAM Raft group SHALL take effect on a healthy
   replica within 5 seconds at p99. Replica lag SHALL be exported as a metric.
   A replica more than a configured bound behind (default 60 s) SHALL fail
   closed with `ServiceUnavailable`.
5. THE ARN partition SHALL be configured once per cluster (default `aws`) and
   used in every ARN the system produces or matches. The account-id width
   SHALL stay 12 digits.
6. EVERY authorization decision on a mutating action, and every denial, SHALL
   be written to an audit log as one JSON line. Each line SHALL hold the event
   time, request id, principal ARN, access key id, source IP, action,
   resource, decision and deciding statement id. Allowed reads SHALL be
   logged when `--iam-audit-reads` is set. Secrets and session tokens SHALL
   never be logged.

### Requirement 8: IAM daemon and admin CLI

**User Story:** As an operator, I want to run the IAM service and bootstrap
accounts without already having IAM credentials.

#### Acceptance Criteria

1. A new `services/iam/cmd/iam_node` daemon SHALL host the IAM Raft group and serve the IAM
   Query API and STS on an HTTPS listener. Its health, leader-forwarding and
   unseal behaviour SHALL match `ca_cluster_node`.
2. `iam_node` SHALL use mTLS between members, with certificates from the
   Kythira CA and the reserved-SAN node binding.
3. A `kythira-iam` admin CLI SHALL create and list accounts and print the root
   access key once. It SHALL authenticate over mTLS with an admin certificate
   whose URI SAN is on a configured list. These are Kythira-only operations
   with no AWS API.
4. Clients SHALL reach the IAM and STS endpoints through the SDKs'
   service-specific endpoint settings (`AWS_ENDPOINT_URL_IAM`,
   `AWS_ENDPOINT_URL_STS` or `endpoint_url` per service in config files).
   IAM SHALL be signed for region `us-east-1`; STS SHALL accept any region the
   cluster is configured with.
5. THE daemon and CLI SHALL be built only when a Kconfig option enables
   them (`IAM`, declared in `services/iam/Kconfig`, default n), so builds
   without OpenSSL, and builds that leave the module out, keep working.

### Requirement 9: IAM Query API

**User Story:** As an administrator, I want to manage identities with the AWS
CLI and Terraform, so that I use the same tools I use on AWS.

#### Acceptance Criteria

1. THE API SHALL speak the AWS Query protocol:
   - form-encoded `Action` and `Version=2010-05-08` requests, by `POST`, or
     by `GET` where AWS accepts it;
   - XML responses in namespace `https://iam.amazonaws.com/doc/2010-05-08/`
     with `ResponseMetadata/RequestId`;
   - AWS's XML error shape;
   - `Marker`/`MaxItems`/`IsTruncated` pagination.
2. Users and keys:
   - CreateUser, GetUser, ListUsers, UpdateUser and DeleteUser;
   - TagUser, UntagUser and ListUserTags;
   - CreateAccessKey, ListAccessKeys, UpdateAccessKey, DeleteAccessKey and
     GetAccessKeyLastUsed.
3. Groups:
   - CreateGroup, GetGroup, ListGroups, UpdateGroup and DeleteGroup;
   - AddUserToGroup, RemoveUserFromGroup and ListGroupsForUser.
4. Roles:
   - CreateRole, GetRole, ListRoles, UpdateRole, UpdateRoleDescription,
     UpdateAssumeRolePolicy and DeleteRole;
   - TagRole, UntagRole and ListRoleTags.
5. Policies:
   - CreatePolicy, GetPolicy, ListPolicies and DeletePolicy;
   - CreatePolicyVersion, GetPolicyVersion, ListPolicyVersions,
     DeletePolicyVersion and SetDefaultPolicyVersion;
   - TagPolicy, UntagPolicy and ListPolicyTags;
   - Attach-, Detach- and ListAttached- `User`/`Group`/`RolePolicies`, and
     ListEntitiesForPolicy;
   - Put-, Get-, Delete- and List- `User`/`Group`/`RolePolicy`;
   - Put- and Delete- `User`/`RolePermissionsBoundary`.
6. OIDC providers and simulation:
   - CreateOpenIDConnectProvider, GetOpenIDConnectProvider,
     ListOpenIDConnectProviders and DeleteOpenIDConnectProvider;
   - UpdateOpenIDConnectProviderThumbprint and
     Add-/RemoveClientIDTo/FromOpenIDConnectProvider;
   - SimulatePrincipalPolicy, SimulateCustomPolicy and
     GetContextKeysForPrincipalPolicy;
   - GetAccountAuthorizationDetails and GetAccountSummary.
7. Actions outside this list SHALL return `NotImplemented` (HTTP 501). The one
   exception is the instance-profile calls the Terraform provider makes while
   managing roles (ListInstanceProfilesForRole and others found by
   Requirement 15). Those SHALL be implemented with real instance-profile
   records, so `terraform destroy` works.
8. EVERY action SHALL itself be authorized as `iam:<Action>` against its
   resource ARN, with AWS's resource types and condition keys:
   `iam:PolicyARN`, `iam:PermissionsBoundary`, `iam:PassedToService`,
   `iam:ResourceTag/*` and `iam:AssociatedResourceArn`.
9. Error codes SHALL match AWS: `NoSuchEntity`, `EntityAlreadyExists`,
   `DeleteConflict`, `LimitExceeded`, `MalformedPolicyDocument`,
   `InvalidInput`, `ConcurrentModification` and `ServiceFailure`.

### Requirement 10: STS

**User Story:** As a workload, I want temporary credentials for a role,
including a role in another account, so that I hold no long-lived keys.

#### Acceptance Criteria

1. STS SHALL speak the Query protocol (`Version=2011-06-15`, namespace
   `https://sts.amazonaws.com/doc/2011-06-15/`) and implement AssumeRole,
   AssumeRoleWithWebIdentity, GetSessionToken, GetCallerIdentity and
   GetAccessKeyInfo.
2. AssumeRole SHALL support:
   - `RoleSessionName`, `DurationSeconds` (900 up to the role's maximum,
     default 3600, at most 43200);
   - `ExternalId`, available to the trust policy as `sts:ExternalId`;
   - `Policy` and `PolicyArns` (session policies);
   - `Tags` and `TransitiveTagKeys`;
   - `SourceIdentity`.

   Role chaining SHALL cap the duration at 1 hour, as AWS does.
3. Temporary credentials SHALL be an `ASIA` access key id, a secret, and a
   session token. The token SHALL be self-contained and sealed with
   AES-256-GCM under an STS key. Issuing credentials SHALL need no Raft
   write.
4. THE STS sealing key SHALL rotate on a configured period. Old keys SHALL be
   kept for at least 36 hours, the longest GetSessionToken lifetime.
5. `aws:TokenIssueTime` SHALL be set from the token. Revoking a role's active
   sessions SHALL work as on AWS: attach an inline deny policy conditioned on
   `aws:TokenIssueTime`.
6. A session token over AWS's packed-policy limit SHALL be refused with
   `PackedPolicyTooLarge`.
7. GetCallerIdentity SHALL never be denied by policy, as on AWS.

### Requirement 11: Inbound OIDC federation

**User Story:** As a workload in Kubernetes, GitHub Actions or another OIDC
issuer, I want to assume a Kythira IAM role with my identity token.

#### Acceptance Criteria

1. AssumeRoleWithWebIdentity SHALL validate the JWT against an OIDC provider
   registered in the role's account:
   - the provider is found by the URL in `iss`;
   - the signature is checked against the provider's JWKS (RS256, RS384,
     RS512, ES256, ES384, ES512);
   - `exp`, `nbf` and `iat` are checked, allowing 5 minutes of skew;
   - `aud` must be one of the provider's client ids.
2. THE trust policy SHALL be evaluated with the federated principal
   (`arn:<partition>:iam::<account>:oidc-provider/<host/path>`) and the keys
   `<host/path>:sub`, `:aud`, `:amr` and `:oaud` (where AWS defines them).
3. JWKS documents SHALL be fetched over HTTPS, cached by `kid` for at most 1
   hour, and refetched once on an unknown `kid`. The fetch SHALL have a 5 s
   timeout and a 64 KiB size limit. Thumbprints SHALL be stored and returned
   for AWS API compatibility. Like AWS today, verification SHALL rely on the
   system trust store.
4. Fetches SHALL go out through the configured proxy. Only `https` URLs SHALL
   be allowed, and only to hosts on an optional allowlist. When the allowlist
   is set, private and link-local addresses SHALL be refused, to block
   server-side request forgery.

### Requirement 12: Kythira-CA web identity

**User Story:** As a Kythira node, I want to assume a Kythira IAM role using my
CA certificate, so that the cluster's own components need no IAM keys.

#### Acceptance Criteria

1. THE Kythira issuer (Requirement 1) SHALL be registrable as an OIDC provider
   in an account. A node SHALL then call AssumeRoleWithWebIdentity with a
   Kythira-issued token, through the same path as Requirement 11.
2. Trust policies SHALL be able to match on `sub` (`node:<id>` or
   `workload:<uri>`) and on the composite node id parts, through the token's
   `kythira:*` claims exposed as `<issuer host/path>:kythira:*` condition keys.
3. WHERE the operator enables it, a direct exchange (`POST /v1/iam/credentials`
   on `iam_node`, authenticated by the client certificate, modelled on IAM
   Roles Anywhere `CreateSession`) SHALL return role credentials without a
   JWT step. It SHALL use the same trust-policy evaluation, with the
   certificate's SANs as condition keys.

### Requirement 13: Redis authentication with IAM

**User Story:** As a Redis client, I want to authenticate with either an ACL
password or IAM credentials, so that teams already on IAM need no Redis
passwords.

#### Acceptance Criteria

1. An ACL user SHALL be marked `auth=password`, `auth=iam` or `auth=both`. The
   default SHALL be `password`, so existing ACL files are unchanged.
   - `auth=` SHALL be parsed as a reserved attribute like `cert=`, not as a
     key prefix.
   - A repeated or unknown value SHALL be rejected.
   - Older gateways read `auth=iam` as a key prefix and still accept the
     user's password. `doc/redis-gateway.md` SHALL therefore say that every
     gateway must run the new build before any ACL file uses `auth=`. Where
     a hard guarantee is needed, an IAM-only user can have the secret
     `disabled`, which old builds refuse.
2. FOR an `iam` user, `AUTH <user> <token>` SHALL accept an ElastiCache-style
   IAM auth token: a SigV4 presigned request for
   `GET http://<cache name>/?Action=connect&User=<user>`, service
   `elasticache`, valid for at most 15 minutes. HELLO's `AUTH` form SHALL work
   the same way.
3. THE gateway SHALL authorize `elasticache:Connect` on both:
   - `arn:<partition>:elasticache:<region>:<account>:user:<user>`;
   - `arn:<partition>:elasticache:<region>:<account>:replicationgroup:<cache name>`.

   The cluster's account, region and cache name come from configuration.
4. After IAM authentication, the ACL user's role and key prefixes SHALL still
   apply.
5. AN IAM-authenticated connection SHALL be closed after 12 hours, as on
   ElastiCache. A client SHALL be able to re-AUTH on the open connection to
   extend it.
6. THE gateway SHALL hold an IAM replica (Requirement 7.3) and SHALL need no
   network call per AUTH. The Redis module SHALL NOT depend on IAM.
   - IAM provides an adapter that satisfies core's `credential_verifier`
     concept (`include/raft/credential_verifier.hpp`, from
     `redis-gateway-module` task 6).
   - `redis_gateway_node`'s composition root wires that adapter only when
     `CONFIG_REDIS_GATEWAY_IAM` is set. That option depends on
     `REDIS_GATEWAY && IAM` and is declared in `services/redis/Kconfig`.
7. Forwarded internal links (`redis_forward_link.hpp`) SHALL keep using their
   internal ACL user. IAM SHALL NOT be required between gateways.

### Requirement 14: Security properties

**User Story:** As a security reviewer, I want the identity layer to fail
safely and leak nothing.

#### Acceptance Criteria

1. Every listener serving IAM, STS, the issuer or IAM-authenticated APIs SHALL
   require TLS. Plaintext SHALL be available only behind an explicit
   `--insecure-plaintext` flag used by tests, which logs a warning on every
   start.
2. Secrets, session tokens, JWTs and signatures SHALL NOT appear in logs,
   metrics, error messages or core dumps where avoidable. Buffers holding
   secrets SHALL be cleared with `OPENSSL_cleanse` after use.
3. Unknown access keys and bad signatures SHALL take the same code path and
   comparable time.
4. Any internal error during evaluation SHALL deny.
5. THE categories from the project's 2026-10-02 vulnerability audit SHALL
   be applied to the new listeners before the spec closes.

### Requirement 15: Conformance

**User Story:** As a maintainer, I want proof that real AWS tooling works
against Kythira, so that compatibility claims are tested.

#### Acceptance Criteria

1. A container-based CI job SHALL run `iam_node`, the CA cluster and a Redis
   gateway, and SHALL drive them with the AWS CLI v2 and boto3 at pinned
   versions. The tests SHALL cover:
   - creating a user, a key and a policy;
   - an allowed and a denied call;
   - cross-account AssumeRole with ExternalId;
   - AssumeRoleWithWebIdentity with a test issuer and with the Kythira
     issuer;
   - an expired token, a bad signature, and policy-change propagation;
   - Redis AUTH with an IAM token and with a password.
2. THE job SHALL apply and destroy a Terraform configuration (pinned AWS
   provider) that creates users, groups, roles, policies, attachments,
   boundaries and an OIDC provider.
3. THE compose files SHALL follow the CLAUDE.md container rules: Docker and
   rootless Podman, service-name addressing, and no static IPs or privileged
   mode.
4. THE IAM tests and container scenarios SHALL run in a path-filtered
   `.github/workflows/iam-module.yml`, built like `redis-module.yml`
   (`redis-gateway-module` spec): module on and module off, plus a container
   matrix for Docker and Podman. Like that workflow, it SHALL NOT be a
   required check, so whoever owns an IAM PR waits for it by hand.
5. WHERE Clark gives the go, an optional job SHALL compare the evaluator with
   AWS's own IAM policy simulator (`SimulateCustomPolicy`, which is free) on
   the corpus, and SHALL record any disagreement as a corpus case.

### Requirement 16: Documentation

**User Story:** As an operator, I want to know how to run and use this.

#### Acceptance Criteria

1. `doc/iam.md` SHALL cover:
   - deployment and unseal, account bootstrap and endpoint settings for the
     SDKs, the CLI and Terraform;
   - the supported actions and condition keys, and known differences from
     AWS.
2. `doc/oidc-issuer.md` SHALL give per-cloud setup for trusting the Kythira
   issuer, with the exact audience values.
3. `doc/redis-gateway.md` SHALL document `auth=iam`, the token format, and an
   example with `redis-cli` and a boto3 token generator.

### Requirement 17: Module boundary

**User Story:** As a maintainer, I want IAM to be a self-contained module, so
that core never depends on it and other modules use it only through core
concepts.

#### Acceptance Criteria

1. IAM code SHALL live in `services/iam/`:
   - headers in `include/kythira/iam/`, included as `<kythira/iam/...>`;
   - `cmd/iam_node/` and `cmd/kythira-iam/`;
   - `tests/` (unit tests and the policy corpus), `tests/docker/` and
     `docker/`;
   - `Kconfig`, `CMakeLists.txt`, `boundary-allow.txt` and `README.md`.
2. These SHALL live in core `include/raft/`, because core or several modules
   use them:
   - `jwt.hpp`;
   - `sigv4_verifier.hpp`;
   - `aws_query_protocol.hpp`;
   - `cloud_credential_source.hpp` and the exchange providers;
   - `aws_request_authorizer.hpp`, the concept and plain types (principal
     context, decision, action table) that the Kinesis, DynamoDB and S3
     modules call.

   The SigV4 test suite SHALL live in core `tests/`.
3. `scripts/check_module_boundaries.py` (`redis-gateway-module` task 3) SHALL
   pass for `services/iam/`:
   - it includes public core headers only;
   - its composition-root `*_impl.hpp` includes are listed in
     `boundary-allow.txt`;
   - it includes nothing from `tests/` or `cmd/`;
   - it includes no other module's headers.
4. Core SHALL NOT include anything from `services/iam/`.
5. Another module that needs IAM SHALL depend on the core concept, and its
   composition root SHALL wire in IAM's adapter through a `module-adapter`
   line in that module's `boundary-allow.txt`.
