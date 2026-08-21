# OCI real-cloud test credentials and tenancy setup

Setup for Kythira's OCI components against a real tenancy
(`.kiro/specs/oci-cloud-provider/` Requirement 14.3). Sibling of
[`../aws/`](../aws/README.md) and [`../azure/`](../azure/README.md), with two
deliberate differences called out where they occur: OCI policies are statements
rather than JSON documents, and **this directory issues no credentials** — see
[Credentials](#credentials).

## Contents

| | |
|---|---|
| [`oci_tenancy_check.cpp`](oci_tenancy_check.cpp) | Progressive validator — build it and run it before anything else |
| [`provision-ci-identity.sh`](provision-ci-identity.sh) | Creates the IAM group and policy for the given bundles |
| [`policies/`](policies/) | One policy fragment per test bundle |

## Read this first: run the checker before you build anything

`oci_tenancy_check` makes one authenticated, read-only call and tells you
whether the foundation works. **Stage 1 launches nothing and costs nothing.**

```bash
cmake --build build-default --target oci_tenancy_check
./build-default/tests/oci_tenancy_check
```

It is not a formality. The first time anything in this project made a real
signed request to OCI, it failed — and getting from there to a working
provision surfaced **four defects that the entire mock test suite could not
see**:

1. The endpoint domain was missing its `oci` label, so two of the four service
   hostnames **did not resolve at all**.
2. cpp-httplib appended `:443` to `Host`, a *signed* header, so every request
   failed signature verification.
3. Every request with a body carried a **duplicate `Content-Type`**, which OCI
   answers with a bare `400`.
4. `provision_node` tagged the new instance before OCI would accept the write,
   and read its VNIC before OCI would answer for it.

None of these were reachable locally: `endpoint_override` replaces the whole
host, a mock necessarily binds a non-default port, cpp-httplib's *server*
tolerates duplicate headers, and a mock materialises instances already
`RUNNING`. Coverage was irrelevant to all four. See `spike-notes.md`
Findings 5-8.

### The stages

Each is skipped when its env var is unset, so the same binary is useful at
every point of the setup below.

| Stage | Needs | Proves |
|---|---|---|
| 1. `ListRegions` | credentials only | signing, host derivation, error unwrapping |
| 2. `ListInstances` | `..._COMPARTMENT_ID` | the compartment and read permission |
| 3. `GetInstancePool` | `..._INSTANCE_POOL_ID` | exactly what the manager's constructor does, and that the pool has **one** placement config |
| 4. `ListInstancePoolInstances` | both of the above | the listing `assess_quorum` drives |
| 5. `GetCertificateAuthorityBundle` | `..._CERTIFICATE_AUTHORITY_ID` | the *other* service host |
| 6. full lifecycle | `KYTHIRA_OCI_ALLOW_LAUNCH=1` | **launches a billable instance**: provision → assess → decommission |

Stage 6 is gated twice over because it is the only one that costs money. It
cleans up whatever happens, including on failure.

On failure the checker prints the canonical string it signed — the one thing a
401 never contains.

### Environment

```bash
export KYTHIRA_OCI_REGION=us-phoenix-1
export KYTHIRA_OCI_TENANCY_ID=ocid1.tenancy.oc1..…
export KYTHIRA_OCI_USER_ID=ocid1.user.oc1..…
export KYTHIRA_OCI_FINGERPRINT=aa:bb:…
export KYTHIRA_OCI_PRIVATE_KEY_PEM="$(cat ~/.oci/oci_api_key.pem)"
# optional, per stage:
export KYTHIRA_OCI_COMPARTMENT_ID=…
export KYTHIRA_OCI_INSTANCE_POOL_ID=…
export KYTHIRA_OCI_CERTIFICATE_AUTHORITY_ID=…
```

If you already have a working OCI CLI config, take them from it rather than
retyping:

```bash
eval "$(python3 - <<'PY'
import configparser, os, shlex
c = configparser.ConfigParser(); c.read(os.path.expanduser("~/.oci/config"))
s = c["DEFAULT"]
out = {"KYTHIRA_OCI_REGION": s["region"], "KYTHIRA_OCI_TENANCY_ID": s["tenancy"],
       "KYTHIRA_OCI_USER_ID": s["user"], "KYTHIRA_OCI_FINGERPRINT": s["fingerprint"],
       "KYTHIRA_OCI_PRIVATE_KEY_PEM": open(os.path.expanduser(s["key_file"])).read()}
for k, v in out.items(): print(f"export {k}={shlex.quote(v)}")
PY
)"
```

## Bundles and toggles

A bundle is one group of permissions mapped 1:1 to one real-cloud test binary,
so enabling one never grants blast radius for another. None of these binaries
is CTest-registered — they need real credentials and spend real money, so the
`oci` job builds them as named targets and runs them directly.

| Bundle | Test binary | Toggle | Extra variables |
|---|---|---|---|
| `instance-pool` | `oci_quorum_manager_real_test` | `REAL_CLOUD_TESTS_OCI_INSTANCE_POOL_ENABLED` | `OCI_CI_INSTANCE_POOL_ID` |
| `certificates` | `oci_certificates_provider_real_test` | `REAL_CLOUD_TESTS_OCI_CERTIFICATES_ENABLED` | `OCI_CI_CERTIFICATE_AUTHORITY_ID` |
| `object-persistence` | `oci_object_storage_persistence_real_test` | `REAL_CLOUD_TESTS_OCI_OBJECT_PERSISTENCE_ENABLED` | `OCI_OBJECT_PERSISTENCE_BUCKET` |
| `heartbeat` | *(no binary of its own — grants the `instance-pool` bundle's artifact-delivery path)* | *(covered by `instance-pool`)* | — |

All sit under `REAL_CLOUD_TESTS_OCI_ENABLED` and the master
`REAL_CLOUD_TESTS_ENABLED`, per [`../README.md`](../README.md)'s three-level
model.

Each bundle also takes a `workflow_dispatch` input — `oci_bundle_instance_pool`,
`oci_bundle_certificates`, `oci_bundle_object_persistence` — so a single bundle
can be dispatched without touching repository state.

**This job had none until August 20, 2026**, and the reason it gained them is
worth keeping: with variables as the only switch, running *just* the
object-persistence bundle meant setting the other two to `false`, dispatching,
and setting them back. That is a window in which a scheduled run silently skips
two bundles, and it leaves them off permanently if anything interrupts the
restore. Adding an input for the new bundle alone would have given "how do I
enable this for one run?" two answers inside one job; adding all three settles
it the other way.

### `object-persistence` (cloud key-object persistence spec)

`oci_object_storage_client` under the shared five-check suite that all five
providers run (`.kiro/specs/cloud-object-persistence/`). It is the one bundle
here that **provisions nothing**: it reuses the `kythira-ci-artifacts` bucket
the `heartbeat` bundle already created, under the separate prefix
`kythira-real-test/` that the suite creates and tears down for itself.

```sh
scripts/ci-cloud-credentials/oci/provision-ci-identity.sh \
    --compartment-id "$OCI_CI_COMPARTMENT_ID" \
    --bundles instance-pool,certificates,heartbeat,object-persistence
gh variable set OCI_OBJECT_PERSISTENCE_BUCKET --body kythira-ci-artifacts
gh variable set REAL_CLOUD_TESTS_OCI_OBJECT_PERSISTENCE_ENABLED --body true
```

The policy statements are **replaced, not merged**, so name every bundle the
group should keep.

Its grant is a single statement — `Allow group kythira-ci to manage objects in
compartment <c>` — deliberately narrower than the `heartbeat` bundle's `manage
object-family` (which additionally carries buckets and pre-authenticated
requests, neither of which this client touches) and deliberately carrying **no
`where` clause**. `policies/object-persistence.txt` gives the full argument;
the short version is that a `where` clause on this compartment's Object
Storage policy broke a *different* principal's `put_object` on August 12,
2026, presenting as `404 BucketNotFound`, and this tenancy already declines
**6.87%** of valid Object Storage requests with exactly that error (95% CI
5.6-8.4%, n=1222; spike-notes.md Finding 25). Another
condition would make an open question unanswerable rather than merely open.

**Cost:** a few hundred object operations and a few hundred kilobytes for the
length of the run. The bucket already exists for the heartbeat path, so this
bundle adds no standing cost at all.

**A red run of this bundle is weaker evidence than the other four providers'.**
The suite's own header and the workflow step both say so, and re-running until
green would launder a regression into a flake.

**To investigate one, read the data-plane service log**, with
`./read-object-storage-log.sh <start> <end>` — *not* the audit log, which was
the instruction here until August 21, 2026 and was never performable: Object
Storage data-plane operations are not audited by default, so
`oci audit event list` over a failure window returns nothing (spike-notes.md
Finding 24). The log group `kythira-ci-object-storage` and its two
`OCISERVICE` logs over `kythira-ci-artifacts` (categories `read` and `write`,
30-day retention) exist to make that reading possible; deleting them puts the
flake back out of reach.

What that log established on its first use, so the next reader does not repeat
it: a declined `ListObjects` had a **byte-identical twin that succeeded 6.7 s
earlier** in the same job — same URI, same UPST, same principal, same
`bucketId` — which exonerates the client from the service's side of the wire;
the declined entry carries a **populated `bucketId`**, so the bucket resolved
and the 404 is an authorization decline wearing a not-found status; and the
same request as an Administrator ran **60 times with zero declines**, so the
flake tracks the principal rather than the bucket. Read **both** categories:
the job's second decline was a PUT, invisible in the `read` log, and it was
absorbed by the engine's retry — which is why a green run is not proof the
tenancy behaved.

## Monitoring-config test

The OCI Monitoring monitoring-config test (`oci-monitoring` job;
`scripts/real-cloud-monitoring/oci-monitoring.sh`; doc/TODO.md "Metrics
Backends") is separate from the bundles above: it installs the vendor's
**Management Agent** (in an `oraclelinux:8` container on the runner),
points its PrometheusEmitter at a static exposition via the example config
`docker/cloud-monitoring/oci-management-agent-prometheus-emitter.properties`,
and confirms through `summarize-metrics-data` that the metric reached OCI
Monitoring under the `kythira_chaos_node` namespace. Toggle:
`REAL_CLOUD_TESTS_OCI_MONITORING_ENABLED`. It reuses the WIF credentials
above plus:

- secret `OCI_MGMT_AGENT_INSTALL_KEY` — a Management Agent install key
  (console: Management Agents → Downloads and Keys), whose configured
  compartment should be `OCI_CI_COMPARTMENT_ID`;
- variable `OCI_MGMT_AGENT_INSTALLER_URL` — a URL for the Linux x86_64 ZIP
  installer (download it once from the console and, e.g., publish it to
  the `kythira-ci-artifacts` bucket behind a pre-authenticated request);
- a policy allowing the agents' dynamic group to post metrics, e.g.
  `Allow dynamic-group MgmtAgentGroup to use metrics in compartment <c>
  where target.metrics.namespace='kythira_chaos_node'`;
- the CI service user's group needs `read metrics` in the compartment for
  the query-side assertion, and `manage management-agents` (delete) for
  the post-run deregistration.

**This test has never run against a live tenancy** — the installer URL and
install key are not provisioned. On first use, check the response-file keys
the script writes against the `input.rsp.example` inside the downloaded
installer ZIP; the script comments flag the same caveat.

## Tenancy setup

### 1. Compartment

```bash
COMPARTMENT_ID=$(oci iam compartment create --compartment-id "$TENANCY_OCID" \
  --name kythira-ci --description "kythira real-cloud tests" \
  --wait-for-state ACTIVE --query 'data.id' --raw-output)
```

Everything else goes inside it: one IAM scope, one blast radius, one place to
audit for leaks.

### 2. Group and policy

```bash
./provision-ci-identity.sh --compartment-id "$COMPARTMENT_ID" \
    --bundles instance-pool,certificates --dry-run   # inspect first
./provision-ci-identity.sh --compartment-id "$COMPARTMENT_ID" \
    --bundles instance-pool,certificates
```

Then **run stage 1 of the checker**, before creating anything billable.

### 3. VCN and subnet

Check your ADs first — many OCI regions have only one, which limits how much
per-AD behaviour you can exercise:

```bash
oci iam availability-domain list --compartment-id "$COMPARTMENT_ID" --query 'data[].name'
```

Then a VCN and **one regional subnet**. Regional (no `--availability-domain`)
is enough for every pool: a placement configuration takes
`{availabilityDomain, primarySubnetId}`, so one subnet serves all ADs.
Confirmed working — instances launched into a regional subnet from an AD-scoped
placement config and received addresses from its range.

```bash
VCN_ID=$(oci network vcn create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-ci-vcn --cidr-blocks '["10.0.0.0/16"]' --dns-label kythiraci \
  --wait-for-state AVAILABLE --query 'data.id' --raw-output)
SL_ID=$(oci network vcn get --vcn-id "$VCN_ID" --query 'data."default-security-list-id"' --raw-output)
SUBNET_ID=$(oci network subnet create --compartment-id "$COMPARTMENT_ID" --vcn-id "$VCN_ID" \
  --display-name kythira-ci-subnet --cidr-block 10.0.1.0/24 \
  --prohibit-public-ip-on-vnic true --security-list-ids "[\"$SL_ID\"]" --dns-label nodes \
  --wait-for-state AVAILABLE --query 'data.id' --raw-output)
```

**Gateways: a Service Gateway and nothing else.** With `heartbeat_timeout =
0` the instances make no outbound call at all — the manager talks to OCI's
control plane from wherever *it* runs, not from the instance — so this setup
originally had no gateways whatsoever. The heartbeat work (Requirement 4.4,
nodes writing their own `kythira-last-heartbeat` tag from inside the
instance) is exactly the case that changes that, and a Service Gateway is
its answer: free, keeps the subnet private, and routes only to OCI services
(the API endpoints the Instance Principal writer calls, and Object Storage
for fetching the writer binary). Provisioned live August 12, 2026
(`kythira-ci-sgw`, plus the route rule below — the route table's first and
only rule). Still no NAT Gateway (bills hourly for as long as it exists) and
no Internet Gateway.

```bash
SGW_ID=$(oci network service-gateway create --compartment-id "$COMPARTMENT_ID" \
  --vcn-id "$VCN_ID" --display-name kythira-ci-sgw \
  --services "[{\"serviceId\":\"$(oci network service list \
    --query 'data[?contains(name, `All`)].id | [0]' --raw-output)\"}]" \
  --query 'data.id' --raw-output)
RT_ID=$(oci network vcn get --vcn-id "$VCN_ID" --query 'data."default-route-table-id"' --raw-output)
oci network route-table update --rt-id "$RT_ID" --force --route-rules "[{
  \"destination\": \"all-phx-services-in-oracle-services-network\",
  \"destinationType\": \"SERVICE_CIDR_BLOCK\",
  \"networkEntityId\": \"$SGW_ID\"}]"
```

(The `destination` string is region-specific — read the exact value from
`oci network service list`'s `cidr-block` field for your region.)

The heartbeat bundle's other artifacts — dynamic group
`kythira-ci-instance-dg`, policies `kythira-ci-instance-hb` and
`kythira-ci-artifacts`, bucket `kythira-ci-artifacts` — are documented with
their reasoning in `policies/heartbeat.txt`, all provisioned live the same
day.

The default security list is fine. Nothing in these tests connects *to* the
instances — the manager only calls the REST API.

### 4. Instance Configuration

Shape and image live here, which is why
`oci_instance_pool_quorum_manager_config` has no `image_id` or `shape` field.

```bash
cat > /tmp/ic.json <<JSON
{
  "instanceType": "compute",
  "launchDetails": {
    "compartmentId": "$COMPARTMENT_ID",
    "displayName": "kythira-node",
    "shape": "VM.Standard.E2.1",
    "sourceDetails": { "sourceType": "image", "imageId": "$IMAGE_ID" },
    "createVnicDetails": { "subnetId": "$SUBNET_ID", "assignPublicIp": false }
  }
}
JSON
IC_ID=$(oci compute-management instance-configuration create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-node-config --instance-details file:///tmp/ic.json \
  --query 'data.id' --raw-output)
```

Two deliberate omissions. **No `availabilityDomain`** — the pool's placement
configuration supplies it, and two sources of truth for placement means the
losing one is what you read when an instance lands in the wrong AD. **No
`shapeConfig`** — `E2.1` is a fixed shape; any `*.Flex` shape requires
`"shapeConfig": {"ocpus": 1, "memoryInGBs": 6}` and is rejected without it.

On shape choice: Ampere (`VM.Standard.A1.Flex`) is the Always-Free-eligible
family and much cheaper, but is frequently out of capacity. Prove the loop on
something that launches first. A stockout is not wasted, though — the exact
error OCI returns is what Task 0(g) needs recorded, and Requirement 13.14 says
outright it cannot be written from inspection.

No cloud-init, no `kythira-node` binary, no Object Storage bucket: a stock
Oracle Linux image is enough, because with `heartbeat_timeout = 0` the tests
only observe lifecycle state. The cost is that the heartbeat-freshness path
stays mock-only.

### 5. Instance Pool — size 0, exactly one placement configuration

```bash
oci compute-management instance-pool create --compartment-id "$COMPARTMENT_ID" \
  --instance-configuration-id "$IC_ID" --size 0 \
  --placement-configurations "[{\"availabilityDomain\":\"$AD\",\"primarySubnetId\":\"$SUBNET_ID\"}]" \
  --display-name kythira-ci-pool-ad1 --wait-for-state RUNNING
```

**One placement configuration per pool** is the constraint the whole design
rests on — `UpdateInstancePool` takes one pool-wide `size` and OCI chooses
placement, so a multi-AD pool cannot honour `provision_node(target_group)`. A
three-AD cluster is three pools and three manager instances. Stage 3 of the
checker asserts the count.

**Size 0**: start non-zero and the pool launches untagged instances before a
manager exists to tag them, which the first `provision_node` then adopts
instead of the instance it just paid for.

Now run stages 3-4, then stage 6.

### 6. Certificate Authority (only for the `certificates` bundle)

A CA requires a Vault and a master encryption key — a dependency that is not
obvious from the CA documentation. Despite appearances, **the whole
certificates setup has a standing cost of zero**: a `DEFAULT` vault has no
charge of its own, key versions are USD 0/month (Oracle price list SKU B92092
"Key Versions"), and no Certificates SKU exists at all. Confirmed empirically:
COST and USAGE queries covering a live campaign against this setup returned no
row for Key Management or Certificates. The one real cost hazard is the vault
type — `VIRTUAL_PRIVATE` is a dedicated HSM partition at USD 3.724/hour
(~USD 2,700/month), billed while idle. Use `DEFAULT`. The per-bundle toggle
still lets you enable only `instance-pool` when certificates aren't needed.

Three things to get right, each of which costs a full create cycle to discover:

```bash
# 1. DEFAULT, not VIRTUAL_PRIVATE. A virtual private vault is a dedicated HSM
#    partition and is *substantially* more expensive; nothing here needs one.
VAULT_ID=$(oci kms management vault create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-ci-vault --vault-type DEFAULT \
  --wait-for-state ACTIVE --query 'data.id' --raw-output)
EP=$(oci kms management vault get --vault-id "$VAULT_ID" \
  --query 'data."management-endpoint"' --raw-output)

# 2. The key must be RSA. A CA signs, so a symmetric key is not usable — an AES
#    key is accepted by `key create` and then rejected by the CA with
#    `InvalidParameter: The encryption key with the OCID ... has an invalid
#    shape.`, which does not mention the algorithm. `length` is in BYTES:
#    256 = RSA-2048.
KEY_ID=$(oci kms management key create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-ci-ca-key --key-shape '{"algorithm":"RSA","length":256}' \
  --endpoint "$EP" --wait-for-state ENABLED --query 'data.id' --raw-output)

# 3. The subcommand ends in `-details`. Without it the CLI suggests the right
#    name, but only after the vault and key already exist.
oci certs-mgmt certificate-authority create-root-ca-by-generating-config-details \
  --compartment-id "$COMPARTMENT_ID" --name kythira-ci-ca --kms-key-id "$KEY_ID" \
  --subject '{"commonName": "kythira ci root"}' \
  --wait-for-state ACTIVE --max-wait-seconds 1200
```

Vault and CA creation are both slow — minutes each, not seconds.

**And a fourth, the one that cost the most: the CA reaches its key as a
*resource principal*, matched by a Dynamic Group.**

```
ALL {resource.type='certificateauthority', resource.compartment.id='<compartment ocid>'}
```

```
Allow dynamic-group kythira-ci-ca-dg to use keys in compartment kythira-ci
Allow dynamic-group kythira-ci-ca-dg to use vaults in compartment kythira-ci
```

`provision-ci-identity.sh` creates both when the `certificates` bundle is
selected, so this is only worth knowing when something goes wrong — and it will
go wrong *quietly*. Without the grant, `CreateCertificateAuthority` accepts the
request and fails **asynchronously**: minutes later the CA sits in
`lifecycle-state: FAILED` with `lifecycle-details: Authorization failed or
requested resource not found: Key Id ...`. Nothing fails at request time, so
`--wait-for-state ACTIVE` simply waits until the CA gives up.

A service-principal statement is the *wrong mechanism* and looks plausible
enough to burn an afternoon: three attempts with `Allow service certificates to
use keys` — then `use vaults`, then `use key-delegate` — failed identically.
The attempt that added the Dynamic Group succeeded. (`certificatesmanagement`
is not a valid service principal at all, despite being the management API's
hostname; OCI at least rejects that one immediately with `Service {x} does not
exist.`)

**Your CSRs must be RSA.** An OCI CA requires an RSA master key, and OCI
rejects a certificate whose CSR key is from a different algorithm family —
**asynchronously**, so `CreateCertificate` succeeds and the certificate lands in
`lifecycleState: FAILED` minutes later with "The key algorithm is in a
different algorithm family from the issuing certificate authority's algorithm
family." This project's `leaf_certificate_options` defaults to **ECDSA P-256**,
so a deployment using that default cannot use this CA without switching to
`key_algorithm::rsa_2048` (`spike-notes.md` Finding 19).

`oci_certificates_provider` issues with
`configType = MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA`: the caller generates
the key pair and submits **only the CSR**, so the private key never reaches
OCI. OCI's *default* flow does the opposite and deposits the key in Vault —
worth knowing if you create certificates through the console and wonder why.

### 7. A budget alert

Set an OCI Budget on the compartment before the first launch. Every provider in
this project leaked something on its first live run; a budget alert is the
cheapest version of the audit that catches it.

## Credentials

**Federation is provisioned and wired** (Requirement 14.2, `spike-notes.md`
Finding 17). OCI IAM **Workload Identity Federation** exchanges the CI job's
GitHub OIDC JWT plus a locally generated public key for a short-lived **User
Principal Session Token** (UPST) via the Token Exchange grant; the job then
signs OCI API calls with the matching session private key
(`oci_client_config::security_token` — `keyId = "ST$" + token`). No
long-lived key exists anywhere in the path, which is exactly the posture
Requirement 14.2 wanted; its API-key fallback was never needed.

What exists in the tenancy's **Default identity domain** (created August 12,
2026, via the identity-domains CLI):

| artifact | value |
|---|---|
| service user | `kythira-ci-wif` (SCIM id `dd1458d3b03b45a5a352caf52b2d0a5f`, `serviceUser: true`), member of `kythira-ci` |
| confidential app | `kythira-ci-token-exchange`, client_id `2638970cead24539a393a19765fb6b6e` |
| trust | `github-actions-kythira` (id `21dc4fc815454c7a93dd34e45400cdca`): issuer `https://token.actions.githubusercontent.com`, JWKS `…/.well-known/jwks`, rule `sub eq repo:crawlins/kythira:environment:real-cloud-tests` → impersonates the service user |

Two facts about the trust that cost a live dispatch each to learn:

- **The impersonation rule's value must be unquoted.** Both
  `sub eq "repo:…"` and `sub eq 'repo:…'` fail to match with
  `unauthorized_client: No rules matched from given token to find
  impersonation user` — the same error a wrong value gives, so the
  workflow's failure path prints the JWT's decoded `sub` claim alongside
  OCI's response to keep the two distinguishable.
- **The impersonated user must be a genuine `serviceUser`.** A regular user
  with every credential capability disabled is rejected at exchange time
  with `unauthorized_client: User requesting is not a service user`. (A
  leftover regular user `kythira-ci-federation` from that attempt may still
  exist; it holds no credentials and can be deleted.)

What the workflow consumes: `real-cloud-tests` **environment secrets**
`OCI_DOMAIN_URL`, `OCI_WIF_CLIENT_ID`, `OCI_WIF_CLIENT_SECRET`, and
**repository variables** `OCI_CI_REGION` / `OCI_CI_COMPARTMENT_ID` /
`OCI_CI_INSTANCE_POOL_ID` / `OCI_CI_CERTIFICATE_AUTHORITY_ID` plus the
`REAL_CLOUD_TESTS_OCI_*` toggles. The toggles must be **repository**
variables: the job's gate is a job-level `if:`, which is evaluated before
the environment binds, so environment-scoped toggles read as unset and the
job silently never runs.

Domain-side notes that cost time to learn (all via
`oci identity-domains …`, which signs with an ordinary API key):

- `--from-json` wants the **CLI model key names**
  (`--generate-full-command-json-input` shows them), not raw SCIM URNs —
  URN-keyed attributes are **silently dropped**. That silently produced a
  non-service user twice; and since `serviceUser` is immutable post-create,
  the fix was not a patch but a recreate via **`oci raw-request`** (`POST
  {domain}/admin/v1/Users` with the raw SCIM body), which bypasses the CLI
  model entirely and is how `kythira-ci-wif` was made.
- `impersonationServiceUsers` is a returned-on-request attribute: a bare GET
  on the trust shows `null`. Pass `--attributes impersonationServiceUsers
  --attribute-sets request` before concluding the rule is missing.
- The App schema rejects the token-exchange grant in `allowedGrants` —
  exchange authorization comes from the trust's `oauthClients` list; the
  confidential app needs only `client_credentials`.
- The client_id:client_secret pair is not a stored OCI credential: alone it
  authenticates as nothing, and only authorises presenting a valid
  GitHub-signed JWT (matching the trust rule above) to the exchange.

Two things that remain true from the original wiring notes:

- This is an **identity-domain** mechanism producing a *User* Principal
  Session Token, so policy targets a user/group principal. It is **not** the
  Dynamic Group model Requirement 14.2 originally assumed — Dynamic Groups
  remain the model for the certificate authority's resource principal (see
  [`policies/certificates.txt`](policies/certificates.txt)).
- `provision-ci-identity.sh` still issues no credentials; the UPST exchange
  happens in the job, not at provisioning time.

**Instance Principal is implemented** (`oci_federation::
instance_principal_signer`, `include/raft/oci_federation.hpp`) — the auth
mode for a kythira process running *on* an OCI instance, verified against
the mock tier and `oci-go-sdk`'s contract but not yet on a real instance.
It is unrelated to CI federation: a GitHub runner is not an OCI instance
and has no metadata service.

## Operational notes from real runs

Things that cost time to learn and are not in Oracle's documentation in this
form:

- **Pool scale-down terminates with a lag.** `UpdateInstancePool(size=0)`
  returns, and the instance is still `RUNNING` for a minute or two. Do not
  conclude you have leaked one until you have watched it for a few minutes.
- **`ListInstancePoolInstances` and `GetInstance` disagree on case.** The
  listing reports `state: "Provisioning"`/`"Running"`; `GetInstance` reports
  `lifecycleState: "PROVISIONING"`/`"RUNNING"`. The manager reads the latter.
- **A new instance is not taggable immediately.** `UpdateInstance` against one
  the pool is still building answers `409 Conflict: instance ... is currently
  being modified, try again later`.
- **`GetVnic` answers `404 NotAuthorizedOrNotFound` before the VNIC is
  visible** — the same status and code as a missing permission, so a transient
  race reads exactly like a broken policy. Check whether the CLI can read the
  same VNIC with the same credentials before touching IAM.
- **Boot volumes are released on terminate.** Verified after a full lifecycle:
  both volumes went `TERMINATED` with their instances. Azure's equivalent
  orphaned a permanently-billing disk on every VM, so this is worth confirming
  yourself after your first run rather than assuming.
