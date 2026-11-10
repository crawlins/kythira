# IAM and STS

Kythira's IAM module (`services/iam/`, `.kiro/specs/iam-compatible-auth/`)
speaks the AWS IAM and STS Query APIs, so the AWS SDKs, the AWS CLI and
Terraform's AWS provider manage users, roles and policies on it and get
temporary credentials from it unchanged. Other Kythira services (the Redis
gateway today, then S3, Kinesis and DynamoDB) authorize requests against the
same policies through a local read replica, with no network call per
request.

This page covers running it, pointing clients at it, what it implements, and
where it differs from AWS. The module's own layout and build switches are in
`services/iam/README.md`; the Kythira CA as an identity provider is in
[oidc-issuer.md](oidc-issuer.md); Redis `AUTH` with IAM tokens is in
[redis-gateway.md](redis-gateway.md#iam-authentication).

## Deployment

`iam_node` is one member of the IAM Raft group. Run three or five voters,
each with:

- `--node-id`, and `--peers` naming every other voter as
  `<id>:<rpc host>:<rpc port>@<https url>`;
- `--data-dir` (or `IAM_DATA_DIR`) for the log and snapshots;
- `--unseal-key-file` (or `IAM_UNSEAL_KEY_FILE`): the cluster's unseal
  passphrase. Access-key secrets and STS signing keys are sealed in the log
  with AES-256-GCM under a key derived from it, so the log, snapshots and
  backups hold no usable secret. Every voter and every replica needs the same
  passphrase; keep it out of the data directory;
- `--tls-cert`/`--tls-key` for the HTTPS API (port 8443 by default);
- `--rpc-tls-cert`/`--rpc-tls-key`/`--rpc-tls-ca` for mutual TLS between
  members (port 7100 by default);
- `--admin-uri-san <uri>`: the URI SAN a client certificate must carry to
  use the admin API. Without it the admin API refuses everyone.

`iam_node --help` lists the rest: `--sts-regions` (the regions STS answers
for, default `us-east-1`), `--arn-partition` (default `aws`),
`--oidc-allowed-hosts`, `--oidc-proxy` and `--oidc-ca-file` for fetching
external issuers' keys, `--kythira-issuer`/`--kythira-issuer-local` and
`--iam-credentials-exchange` for the Kythira CA (see
[oidc-issuer.md](oidc-issuer.md)), and the Raft timing flags.

Every IAM and STS request is answered by the leader: a follower answers
with a 307 redirect to it, which the SDKs and the CLI follow. So every
answer reflects every acknowledged write.

### Replicas

A front end that authorizes requests (a Redis gateway, later S3 and the
others) embeds a read replica: a non-voting member that applies the log and
evaluates policies locally. Register it once from the admin CLI:

```sh
kythira-iam replica-add --node-id 6 --rpc-host redis-1 --rpc-port 7100
```

The replica's RPC certificate must carry its id's reserved peer name. A
replica more than its staleness bound behind the leader (60 seconds by
default) refuses to authorize anything until it catches up: it fails
closed. `iam_node --replica` runs a standalone replica.

### Bootstrap

The admin CLI talks to the admin API with a client certificate carrying the
`--admin-uri-san` URI:

```sh
export KYTHIRA_IAM_ENDPOINT=https://iam-1:8443
export KYTHIRA_IAM_CERT=admin.pem KYTHIRA_IAM_KEY=admin.key KYTHIRA_IAM_CA=ca.pem

# The first account also sets the sealing salt.
kythira-iam create-account --alias prod --root-key-file ./prod-root.ini
# STS signs nothing until it has a key; rotate one in after the first account.
kythira-iam rotate-sts-key
kythira-iam list-accounts
```

`create-account` writes the account root's access key to the named file
(mode 0600) in AWS credentials-file form and refuses to overwrite an
existing file, so the secret is never printed or lost. `--account-id` picks
the 12-digit id instead of a random one. Use the root key to create IAM
users and roles, then put it away, as on AWS.

`rotate-sts-key` adds a new STS signing key and retires the old one after
`--retire-after-secs` (36 hours by default, the longest a session can live).
Session tokens carry the key id, so rotation never invalidates a live
session early.

## Pointing clients at Kythira

Every client needs the endpoint and the CA that signed the HTTPS
certificate. IAM is signed for `us-east-1`, as on AWS; STS for any region in
`--sts-regions`.

**SDKs and the AWS CLI** read the standard variables, so nothing in the
application changes:

```sh
export AWS_ENDPOINT_URL_IAM=https://iam.example.internal:8443
export AWS_ENDPOINT_URL_STS=https://iam.example.internal:8443
export AWS_CA_BUNDLE=/etc/kythira/ca.pem
aws iam list-users
aws sts get-caller-identity
```

or the same per profile in `~/.aws/config`:

```ini
[profile kythira]
region = us-east-1
ca_bundle = /etc/kythira/ca.pem
services = kythira

[services kythira]
iam =
  endpoint_url = https://iam.example.internal:8443
sts =
  endpoint_url = https://iam.example.internal:8443
```

The SDK credential providers work as on AWS: a `source_profile` role chain,
`web_identity_token_file`, and the `AWS_ROLE_ARN` /
`AWS_WEB_IDENTITY_TOKEN_FILE` pair all resolve against Kythira's STS once
`AWS_ENDPOINT_URL_STS` points at it. The conformance job checks all three
with boto3 and the AWS CLI v2.

**Terraform** (AWS provider 5.x):

```hcl
provider "aws" {
  region                  = "us-east-1"
  skip_metadata_api_check = true
  endpoints {
    iam = "https://iam.example.internal:8443"
    sts = "https://iam.example.internal:8443"
  }
}
```

with `AWS_CA_BUNDLE` set as above. The conformance job applies, re-plans
(expecting no changes) and destroys a configuration with users, groups,
roles, managed and inline policies, attachments, permissions boundaries, an
OIDC provider and an instance profile
(`services/iam/tests/docker/conformance/terraform/main.tf`).

## What is implemented

### IAM actions

Users, groups and access keys: CreateUser, GetUser, UpdateUser, DeleteUser,
ListUsers, CreateGroup, GetGroup, UpdateGroup, DeleteGroup, ListGroups,
AddUserToGroup, RemoveUserFromGroup, ListGroupsForUser, CreateAccessKey,
UpdateAccessKey, DeleteAccessKey, ListAccessKeys, GetAccessKeyLastUsed.

Roles and instance profiles: CreateRole, GetRole, UpdateRole,
UpdateRoleDescription, UpdateAssumeRolePolicy, DeleteRole, ListRoles,
CreateInstanceProfile, GetInstanceProfile, DeleteInstanceProfile,
ListInstanceProfiles, ListInstanceProfilesForRole, AddRoleToInstanceProfile,
RemoveRoleFromInstanceProfile.

Policies: CreatePolicy, GetPolicy, DeletePolicy, ListPolicies,
CreatePolicyVersion, GetPolicyVersion, DeletePolicyVersion,
ListPolicyVersions, SetDefaultPolicyVersion, ListEntitiesForPolicy;
Attach/Detach/ListAttached for users, groups and roles; Put/Get/Delete/List
inline policies for users, groups and roles; Put/Delete permissions
boundaries for users and roles. The AWS managed policies are built in, at
the versions pinned in `services/iam/tools/aws_managed_policies.json`.

Tags: Tag, Untag and List…Tags for users, roles, policies, instance
profiles and OIDC providers.

OIDC providers: Create, Get, Delete, List, AddClientID, RemoveClientID,
UpdateThumbprint.

Account: CreateAccountAlias, DeleteAccountAlias, ListAccountAliases,
GetAccountSummary, GetAccountAuthorizationDetails.

Policy tools: SimulatePrincipalPolicy, SimulateCustomPolicy,
GetContextKeysForPrincipalPolicy, GetContextKeysForCustomPolicy.

Every action is itself authorized as `iam:<Action>` on its resource ARN,
with `iam:PolicyARN`, `iam:PermissionsBoundary`, `iam:PassedToService`,
`iam:ResourceTag/*` and `iam:AssociatedResourceArn`. Error codes follow AWS
(`NoSuchEntity`, `EntityAlreadyExists`, `DeleteConflict`, `LimitExceeded`,
`MalformedPolicyDocument`, `InvalidInput`, `ConcurrentModification`,
`ServiceFailure`).

### STS actions

AssumeRole (cross-account, `ExternalId`, session policies, session tags and
transitive tags, `SourceIdentity`), AssumeRoleWithWebIdentity (any OIDC
issuer registered as a provider, and the Kythira CA), GetSessionToken,
GetFederationToken, GetCallerIdentity, GetAccessKeyInfo and
DecodeAuthorizationMessage.

Session tokens are self-contained: sealed under the current STS key with
its id, so any member or replica verifies one with no stored session
state. As on AWS, GetSessionToken credentials cannot call IAM (no session
has MFA) and GetFederationToken credentials can call only
GetCallerIdentity in STS and nothing in IAM.

### Condition keys

Global: `aws:CurrentTime`, `aws:EpochTime`, `aws:SecureTransport`,
`aws:SourceIp`, `aws:UserAgent`, `aws:RequestedRegion`,
`aws:PrincipalArn`, `aws:PrincipalAccount`, `aws:PrincipalType`,
`aws:PrincipalTag/*`, `aws:userid`, `aws:username`, `aws:RequestTag/*`,
`aws:TagKeys`, `aws:ResourceTag/*`, `aws:ResourceAccount`,
`aws:MultiFactorAuthPresent` (always false), `aws:TokenIssueTime`,
`aws:SourceIdentity`, `aws:CalledVia`, `aws:CalledViaFirst`,
`aws:CalledViaLast`.

STS: `sts:ExternalId`, `sts:RoleSessionName`, `sts:SourceIdentity`,
`sts:TransitiveTagKeys`; for web identity, the provider's `<provider>:aud`,
`<provider>:sub` and `<provider>:amr`; for the Kythira issuer, the
`kythira:*` claims listed in [oidc-issuer.md](oidc-issuer.md).

Policy variables (`${aws:username}` and the rest) and every condition
operator AWS documents, with `IfExists` and the `ForAllValues`/
`ForAnyValue` set operators, are supported. The policy corpus
(`services/iam/tests/corpus/`) is the reference for the evaluator.

## Differences from AWS

Deliberate:

- **No Organizations.** Accounts are made with `kythira-iam
  create-account`. There are no service or resource control policies (the
  evaluator has a seam where they would go), and `sts:AssumeRoot` is not
  implemented.
- **Out of scope, answering `NotImplemented`:** AssumeRoleWithSAML and
  Identity Center, MFA devices, login profiles (console passwords), server
  and signing certificates, SSH keys, service-specific credentials,
  credential reports, service-last-accessed data and Access Analyzer.
  SigV4a is not accepted.
- **Account-level quotas** (1500 customer managed policies, 1000 roles and
  so on) are reported by GetAccountSummary but not enforced. Per-entity
  quotas (attachments, inline policy sizes, tags, versions) are enforced as
  on AWS.
- **STS request rate** has no per-account quota; the front end's own limits
  apply.
- **Strong consistency.** AWS IAM is eventually consistent; Kythira answers
  every read from the leader, so a change is visible to the next request.
  Replicas used by other services lag by at most their staleness bound and
  refuse past it.
- **Encoded authorization messages** are produced only by services that ask
  for them; the IAM and STS APIs do not, as on AWS.

Found by testing:

- **AWS's policy simulator is not the reference.** Run on the corpus
  (Requirement 15.5), `SimulateCustomPolicy` agreed on 71 of 73 cases. On
  the other two, real requests to S3 showed the simulator wrong and the
  evaluator right: a Deny `NotPrincipal` listing a user with its account
  lets that user through, and a same-account grant to the account does not
  escape a permissions boundary. Kythira follows real AWS; those corpus
  cases record the simulator's answer in `aws_simulator`.
- **Terraform's calls.** The AWS provider's IAM calls, recorded against
  real AWS, are in `services/iam/tests/terraform_calls.txt`; all are
  implemented.

## Security notes

- Secrets never enter the log in plaintext: access-key secrets and STS keys
  are sealed, and a secret is returned exactly once, at creation.
- Anything the evaluator cannot read (a policy that will not parse, a
  session policy whose ARN no longer exists, a stale replica, a missing
  key) denies.
- Web identity key fetches are https only, refuse non-public addresses and
  pin the connection to the checked address, so a token's issuer cannot
  point Kythira at an internal host.
- Failed signatures spend a per-key budget (`sigv4::failure_limiter`), and
  the Redis gateway also charges the caller's address.
