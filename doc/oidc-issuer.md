# Kythira as an OIDC issuer

`ca_cluster_node` can act as an OpenID Connect issuer. A node or workload that
holds a Kythira-CA client certificate exchanges it for a short-lived signed
JWT, and the cloud providers' token services exchange that JWT for cloud
credentials. No cloud keys are stored anywhere. Specification:
`.kiro/specs/iam-compatible-auth/`, Requirement 1.

## Turning it on

| Flag | Meaning |
|---|---|
| `--oidc-issuer-url <https url>` | Turns the issuer on. Every token's `iss` equals it byte for byte. No query, fragment or trailing `/`. |
| `--oidc-alg RS256\|ES256` | Signing algorithm. RS256 (RSA 2048) by default. |
| `--oidc-key-rotation-days <n>` | Signing key period, 90 days by default. A successor is published 24 hours before it signs. |
| `--oidc-max-token-lifetime-secs <n>` | Longest token, 12 hours by default and at most. A superseded key stays published this long after the switch, so no token outlives its key. |
| `--oidc-audience <aud>` | An audience tokens may name. Repeatable. With none, every token request is refused. |
| `--oidc-workload-uri-prefix <uri>` | URI SAN prefix that marks a workload certificate, e.g. `spiffe://example.org/wl/`. Must end in `/` or `:`. Without it only node certificates get tokens. |
| `--oidc-publish-url <scheme>://<bucket>[/<prefix>]` | Also upload the issuer documents to a bucket (see below). |

The leader generates the signing key, seals it under the unseal passphrase
like the CA key, and keeps it in the replicated CA state.

## Documents

`GET /.well-known/openid-configuration` and `GET /.well-known/jwks.json` need
no credentials. The leader answers them; a follower redirects (308) to the
leader, or answers 503 `no_known_leader`.

## Getting a token

`POST /v1/oidc/token` over the client-API TLS listener, presenting the client
certificate. No bearer token. The body is a form:

```
audience=sts.amazonaws.com&lifetime=3600
```

`lifetime` is optional: 1 hour by default, cut to the certificate's remaining
validity. An explicit lifetime must be 60 seconds to the maximum and must end
before the certificate does.

```
curl --cert node.pem --key node.key --cacert listener-root.pem \
  -d audience=sts.amazonaws.com https://ca.example.org:8443/v1/oidc/token
```

```json
{"token":"eyJ...","token_type":"urn:ietf:params:oauth:token-type:jwt",
 "expires_at":"2026-11-05T03:00:00Z","expires_in":3600}
```

The subject comes from the certificate:

- a cluster member's reserved peer name (`ca-cluster-node-<n>` or
  `urn:kythira:node:<id>`) gives `sub` = `node:<id>` and `kythira:node_id`;
- otherwise exactly one URI SAN under `--oidc-workload-uri-prefix` gives
  `sub` = `workload:<uri>` and `kythira:workload`.

Every token also carries `aud`, `iat`, `nbf`, `exp`, `jti` and
`kythira:cert_serial`.

| Status | Why |
|---|---|
| 401 | No client certificate, or one that does not chain to the CA, is revoked or has expired |
| 403 | A valid certificate that names no node and no workload under the prefix |
| 400 | Malformed body, an audience not on the allowlist (`invalid_target`), or a lifetime the policy or the certificate does not allow |
| 503 | No leader known, or no signing key yet |

Followers redirect the request to the leader with 308; the client presents
its certificate again there.

## Audience values

| Cloud | Audience |
|---|---|
| AWS | `sts.amazonaws.com`, or the client id registered on the IAM OIDC provider |
| GCP | `//iam.googleapis.com/projects/<n>/locations/global/workloadIdentityPools/<pool>/providers/<provider>` |
| Azure | `api://AzureADTokenExchange` |
| Alibaba | the client id registered on the RAM OIDC provider |
| OCI | the client id configured on the identity domain's token-exchange trust |

## Using tokens inside Kythira

`include/raft/cloud_credential_exchange.hpp` does the exchange for Kythira's
own cloud clients. `kythira_oidc_token_source` fetches and caches a token per
audience over mTLS, following a follower's redirect with the certificate.
`aws_web_identity_source` calls `AssumeRoleWithWebIdentity`, and
`gcp_workload_identity_source` calls the GCP STS token exchange and, when a
service account is named, `generateAccessToken`. `azure_federated_source`
asks Entra ID for a token per resource scope with the Kythira token as a
federated `client_assertion`. `alibaba_oidc_role_source` calls
`AssumeRoleWithOIDC`. `oci_upst_source` exchanges the token at an OCI
identity domain for a User Principal Session Token, bound to a session key
pair generated fresh for each exchange. Every source refreshes at 80% of the
lifetime and keeps serving the old credentials while a refresh fails, until
they expire.

- AWS components take
  `aws_client_config::credentials_provider = make_web_identity_provider_chain(source)`
  (`include/raft/aws_web_identity_credentials.hpp`). That covers the S3
  client, the ACM-PCA provider, and the EC2 and ASG components.
- `gcp_gcs_client` takes `gcp_client_config::access_token`, a function
  returning the source's current token. The other GCP components do not read
  it yet.
- Azure components take
  `azure_client_config::credential = make_shared<azure_federated_token_credential>(source)`
  (`include/raft/azure_federated_credential.hpp`). That covers the blob
  client, the Key Vault CA provider and the VM and VMSS components, each
  asking for its own scope.
- Alibaba components take `alibaba_client_config::credentials`, a function
  returning the source's current keys. Both signers read it on every
  request, so the OSS client and the control-plane components are covered.
- OCI components take `oci_client_config::session_credentials`, a function
  returning the source's current UPST and session key. `oci_signing` signs
  with them under `keyId = "ST$<token>"`.

Each cloud has its own trust setup and audience:

| Cloud | Trust | Kythira token audience (default) |
|---|---|---|
| AWS | IAM OIDC provider and role | `sts.amazonaws.com` |
| GCP | Workload identity pool provider | the provider's resource name |
| Azure | Federated credential on an app registration or managed identity | `api://AzureADTokenExchange` |
| Alibaba | RAM OIDC provider and role | one of the provider's client ids, `sts.aliyuncs.com` |
| OCI | Identity propagation trust on an identity domain | `https://cloud.oracle.com` |

OCI's exchange authenticates as the confidential application the trust
names, with its client id and secret over HTTP Basic, so that secret is the
one stored value on the OCI path. The application can exchange tokens and
holds no OCI permissions of its own.

## Publishing to a bucket

AWS and GCP fetch the discovery document from `<issuer>/.well-known/...`, and
need a publicly trusted HTTPS origin. When the CA cluster should not be that
origin, publish the documents to a bucket instead:

1. Make a bucket (or a prefix in one) publicly readable for
   `<prefix>/.well-known/*`.
2. Set `--oidc-issuer-url` to the bucket's public HTTPS URL plus the prefix,
   e.g. `https://kythira-oidc.s3.us-east-1.amazonaws.com/prod`.
3. Set `--oidc-publish-url` to the same place in the store's own scheme,
   e.g. `s3://kythira-oidc/prod`.

The issuer URL, the discovery URL and `jwks_uri` then all point at the
bucket, and nothing else changes.

| Scheme | Store | Credentials |
|---|---|---|
| `s3://` | Amazon S3 | the AWS SDK default chain |
| `gs://` | Google Cloud Storage | Application Default Credentials, `GOOGLE_CLOUD_PROJECT` |
| `az://<container>` | Azure Blob Storage | `KYTHIRA_AZURE_STORAGE_ACCOUNT` plus the Azure SDK default chain |
| `oci://` | OCI Object Storage | the `KYTHIRA_OCI_*` variables (see `raft_object_backup --help`) |
| `oss://` | Alibaba OSS | the `KYTHIRA_ALIBABA_*` variables |

A provider is available when the build found its dependency and its
persistence Kconfig option is on. A node asked for a provider it lacks
refuses to start and says what would enable it.

The leader uploads the JWKS first, then the discovery document:

- whenever the JWKS differs from the last upload (a successor is published,
  or a retiring key drops out, with no command needed);
- every six hours anyway, which restores deleted objects;
- after a failure, on a retry that backs off from 5 seconds to 5 minutes.

A new leader uploads at once. Objects are written with the store's default
content type, because the object-store clients do not set one. Whether each
cloud's token service accepts that is checked by the real-cloud federation
run (task 5); if one does not, the clients gain a content-type option.

Failures are logged at `[error]` and counted on `GET /metrics` (bearer token
required, Prometheus text format):

| Metric | Meaning |
|---|---|
| `kythira_oidc_publish_failures_total` | Failed uploads on this node |
| `kythira_oidc_publish_success_total` | Successful uploads on this node |
| `kythira_oidc_publish_last_success_timestamp_seconds` | Unix time of the last successful upload |

Only the leader publishes, so alert on the cluster-wide sum of failures, and
on the largest last-success time being older than a few hours.
